#include "expression_parser.h"
#include "statement_parser.h" // For interpret_statement declaration
#include "parser_utils.h"     // For interpreter_eat
#include "scope.h"            // For symbol_table_get
#include "value_utils.h"      // For DynamicString helpers (ds_init, ds_append_str, ds_finalize)
#include "dictionary.h"       // For dictionary_create, dictionary_set, dictionary_get
#include "modules/builtins.h" // For builtin_slice
#include "module_loader.h"    // Added: for resolve_module_path and load_module_from_path
#include "interpreter.h"      // For add_to_ready_queue

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

// Note: Statement parsing functions (interpret_show, interpret_let, interpret_if, interpret_loop, 
// interpret_funct, interpret_return, etc.) are defined in statement_parser.c.
// Expression parser relies on statement_parser.h for interpret_statement if needed (e.g. in execute_echoc_function).

Value interpret_instance_creation(Interpreter* interpreter, Blueprint* bp_to_instantiate, Token* call_site_token); // Made non-static

static void coroutine_add_waiter(Coroutine* self, Coroutine* waiter_coro_to_add); // Moved forward declaration earlier

// Forward declarations for deep equality helpers
static bool array_deep_equal(Interpreter* interpreter, Array* arr1, Array* arr2, Token* error_token);
static bool tuple_deep_equal(Interpreter* interpreter, Tuple* tup1, Tuple* tup2, Token* error_token);
static bool dictionary_deep_equal(Interpreter* interpreter, Dictionary* dict1, Dictionary* dict2, Token* error_token);
static bool values_are_deep_equal(Interpreter* interpreter, Value v1, Value v2, Token* error_token);
static bool values_are_identical(Value v1, Value v2);
bool value_is_truthy(Value v); // Declaration for use within this file

// Forward declarations for other moved helper functions
Value interpret_dictionary_literal(Interpreter* interpreter);
static void parse_call_arguments_with_named(Interpreter* interpreter, ParsedArgument args_out[], int* arg_count_out, int max_args, Token* call_site_token_for_errors);

// Helper to check if a function name is a built-in
static bool is_builtin_function(const char* name) {
    if (strcmp(name, "slice") == 0 ||
        strcmp(name, "show") == 0 ||
        strcmp(name, "type") == 0) {
        return true;
    }
    return false;
}

// Moved helper functions (definitions)
static bool array_deep_equal(Interpreter* interpreter, Array* arr1, Array* arr2, Token* error_token) {
    if (arr1 == arr2) return true; // Same instance
    if (!arr1 || !arr2) return false; // One is null
    if (arr1->count != arr2->count) return false;
    for (int i = 0; i < arr1->count; ++i) {
        if (!values_are_deep_equal(interpreter, arr1->elements[i], arr2->elements[i], error_token)) {
            return false;
        }
    }
    return true;
}

static bool tuple_deep_equal(Interpreter* interpreter, Tuple* tup1, Tuple* tup2, Token* error_token) {
    if (tup1 == tup2) return true; // Same instance
    if (!tup1 || !tup2) return false; // One is null
    if (tup1->count != tup2->count) return false;
    for (int i = 0; i < tup1->count; ++i) {
        if (!values_are_deep_equal(interpreter, tup1->elements[i], tup2->elements[i], error_token)) {
            return false;
        }
    }
    return true;
}

static bool dictionary_deep_equal(Interpreter* interpreter, Dictionary* dict1, Dictionary* dict2, Token* error_token) {
    if (dict1 == dict2) return true; // Same instance
    if (!dict1 || !dict2) return false; // One is null
    if (dict1->count != dict2->count) return false;

    for (int i = 0; i < dict1->num_buckets; ++i) {
        DictEntry* entry1 = dict1->buckets[i];
        while (entry1) {
            Value val2;
            // dictionary_try_get with false for deep_copy as we only need to compare
            if (!dictionary_try_get(dict2, entry1->key, &val2, false)) {
                return false; // Key from dict1 not in dict2
            }
            // Now val2 holds a shallow copy of the value from dict2.
            // We need to recursively compare entry1->value and val2.
            if (!values_are_deep_equal(interpreter, entry1->value, val2, error_token)) {
                // Note: if val2 was a complex type and dictionary_try_get did not deep copy,
                // its contents are not owned by val2 here. This is fine for comparison.
                return false;
            }
            entry1 = entry1->next;
        }
    }
    return true;
}

static bool values_are_deep_equal(Interpreter* interpreter, Value v1, Value v2, Token* error_token) {
    if (v1.type != v2.type) {
        // Special case: allow int/float comparison
        if ((v1.type == VAL_INT && v2.type == VAL_FLOAT) || (v1.type == VAL_FLOAT && v2.type == VAL_INT)) {
            double d1 = (v1.type == VAL_INT) ? (double)v1.as.integer : v1.as.floating;
            double d2 = (v2.type == VAL_INT) ? (double)v2.as.integer : v2.as.floating;
            return d1 == d2;
        }
        return false; // Different types are generally not equal
    }

    switch (v1.type) {
        case VAL_INT:
            return v1.as.integer == v2.as.integer;
        case VAL_FLOAT:
            return v1.as.floating == v2.as.floating;
        case VAL_STRING:
            if (v1.as.string_val == NULL && v2.as.string_val == NULL) return true;
            if (v1.as.string_val == NULL || v2.as.string_val == NULL) return false;
            return strcmp(v1.as.string_val, v2.as.string_val) == 0;
        case VAL_BOOL:
            return v1.as.bool_val == v2.as.bool_val;
        case VAL_NULL:
            return true; // null is always equal to null
        case VAL_ARRAY:
            return array_deep_equal(interpreter, v1.as.array_val, v2.as.array_val, error_token);
        case VAL_TUPLE:
            return tuple_deep_equal(interpreter, v1.as.tuple_val, v2.as.tuple_val, error_token);
        case VAL_DICT:
            return dictionary_deep_equal(interpreter, v1.as.dict_val, v2.as.dict_val, error_token);
        case VAL_FUNCTION:
            // Functions are equal if they are the same instance (pointer equality)
            return v1.as.function_val == v2.as.function_val;
        case VAL_BLUEPRINT:
            // Blueprints are equal if they are the same instance
            return v1.as.blueprint_val == v2.as.blueprint_val;
        case VAL_OBJECT:
            // Objects are generally considered equal only if they are the same instance.
            // Deep content equality for objects would require comparing all instance attributes,
            // which could be complex and might not be desired default behavior.
            return v1.as.object_val == v2.as.object_val;
        case VAL_COROUTINE: // Intentional fall-through
        case VAL_GATHER_TASK:
            return v1.as.coroutine_val == v2.as.coroutine_val; // Pointer equality
        default:
            return false; // Unknown or unhandled types are not equal
    }
}

static bool values_are_identical(Value v1, Value v2) {
    if (v1.type != v2.type) {
        return false;
    }

    switch (v1.type) {
        case VAL_INT:
            return v1.as.integer == v2.as.integer;
        case VAL_FLOAT:
            return v1.as.floating == v2.as.floating;
        case VAL_BOOL:
            return v1.as.bool_val == v2.as.bool_val;
        case VAL_NULL:
            return true;
        case VAL_STRING:
            return v1.as.string_val == v2.as.string_val;
        case VAL_ARRAY:
            return v1.as.array_val == v2.as.array_val;
        case VAL_TUPLE:
            return v1.as.tuple_val == v2.as.tuple_val;
        case VAL_DICT:
            return v1.as.dict_val == v2.as.dict_val;
        case VAL_FUNCTION:
            return v1.as.function_val == v2.as.function_val;
        case VAL_BLUEPRINT:
            return v1.as.blueprint_val == v2.as.blueprint_val;
        case VAL_OBJECT:
            return v1.as.object_val == v2.as.object_val;
        case VAL_BOUND_METHOD:
            return v1.as.bound_method_val == v2.as.bound_method_val;
        case VAL_COROUTINE: // Intentional fall-through
        case VAL_GATHER_TASK:
            return v1.as.coroutine_val == v2.as.coroutine_val; // Pointer equality
        default:
            return false;
    }
}

bool value_is_truthy(Value v) {
    switch (v.type) {
        case VAL_NULL:
            return false;
        case VAL_BOOL:
            return v.as.bool_val;
        case VAL_INT:
            return v.as.integer != 0;
        case VAL_FLOAT:
            return v.as.floating != 0.0;
        case VAL_STRING:
            return v.as.string_val[0] != '\0'; // Check if not empty string
        case VAL_ARRAY:
            return v.as.array_val->count > 0;
        case VAL_TUPLE:
            return v.as.tuple_val->count > 0;
        case VAL_DICT:
            return v.as.dict_val->count > 0;
        // All other types are considered "truthy" by default
        case VAL_FUNCTION:
        case VAL_BLUEPRINT:
        case VAL_OBJECT:
        case VAL_BOUND_METHOD:
        case VAL_COROUTINE:
        case VAL_GATHER_TASK:
        case VAL_SUPER_PROXY:
            return true;
        default:
            return false; // Should not happen
    }
}

static void coroutine_add_waiter(Coroutine* self, Coroutine* waiter_coro_to_add) {
    CoroutineWaiterNode* new_node = malloc(sizeof(CoroutineWaiterNode));
    if (!new_node) {
        report_error("System", "Failed to allocate CoroutineWaiterNode.", NULL);
    }
    new_node->waiter_coro = waiter_coro_to_add;
    new_node->next = NULL;

    // Append to the list of waiters
    if (self->waiters_head == NULL) {
        self->waiters_head = new_node;
    } else {
        CoroutineWaiterNode* current = self->waiters_head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }
    // The waiter_coro_to_add's ref_count is NOT incremented here.
    // The link is considered weak for ref_counting purposes to avoid cycles.
    DEBUG_PRINTF("COROUTINE_ADD_WAITER: Coro %s (%p) added as waiter to %s (%p). RefCount: %d",
                 waiter_coro_to_add->name ? waiter_coro_to_add->name : "unnamed_waiter",
                 (void*)waiter_coro_to_add,
                 self->name ? self->name : "unnamed_target",
                 (void*)self,
                 waiter_coro_to_add ? waiter_coro_to_add->ref_count : 0);
}

Value interpret_dictionary_literal(Interpreter* interpreter) {
    Token* lbrace_token = interpreter->current_token;
    interpreter_eat(interpreter, TOKEN_LBRACE);
    Dictionary* dict = dictionary_create(16, lbrace_token);
    
	if (interpreter->current_token->type != TOKEN_RBRACE) {
		while (1) {
			ExprResult key_res = interpret_expression(interpreter);
			if (interpreter->exception_is_active) {
				if (key_res.is_freshly_created_container) free_value_contents(key_res.value);
				Value temp_dict_val_to_free = { .type = VAL_DICT, .as.dict_val = dict };
			    free_value_contents(temp_dict_val_to_free);
				return create_null_value();
			}
			Value key = key_res.value;
			if (key.type != VAL_STRING) {
				Value temp_dict_val_to_free = { .type = VAL_DICT, .as.dict_val = dict };
				free_value_contents(temp_dict_val_to_free);
				if (key_res.is_freshly_created_container) free_value_contents(key);
				report_error("Syntax", "Dictionary keys must be (or evaluate to) strings.", interpreter->current_token);
                return create_null_value(); // Unreachable, but for consistency
			}
			interpreter_eat(interpreter, TOKEN_COLON);
			ExprResult value_res = interpret_expression(interpreter);
			if (interpreter->exception_is_active) {
				if (value_res.is_freshly_created_container) free_value_contents(value_res.value);
				if (key_res.is_freshly_created_container) free_value_contents(key);
				Value temp_dict_val_to_free = { .type = VAL_DICT, .as.dict_val = dict };
				free_value_contents(temp_dict_val_to_free);
				return create_null_value();
			}
			dictionary_set(dict, key.as.string_val, value_res.value, interpreter->current_token);
			if (key_res.is_freshly_created_container) free_value_contents(key);
			if (value_res.is_freshly_created_container) free_value_contents(value_res.value);
			if (interpreter->current_token->type == TOKEN_RBRACE) break;
			interpreter_eat(interpreter, TOKEN_COMMA);
		}
	}
    
    interpreter_eat(interpreter, TOKEN_RBRACE);
    Value val;
    val.type = VAL_DICT;
    val.as.dict_val = dict;
    return val;
}

static void parse_call_arguments_with_named(
    Interpreter* interpreter,
    ParsedArgument args_out[],
    int* arg_count_out,
    int max_args,
    Token* call_site_token_for_errors
) {
    *arg_count_out = 0;
    bool named_args_started = false;

    if (interpreter->current_token->type != TOKEN_RPAREN) {
        do {
            if (*arg_count_out >= max_args) {
                report_error("Syntax", "Exceeded maximum number of function arguments (10).", call_site_token_for_errors);
            }

            // Peek ahead to see if it's a named argument (ID = ...)
            Token* peeked_token = NULL;
            bool is_named_arg = false;
            if (interpreter->current_token->type == TOKEN_ID) {
                peeked_token = peek_next_token(interpreter->lexer);
                if (peeked_token && peeked_token->type == TOKEN_ASSIGN) {
                    is_named_arg = true;
                }
            }

            if (is_named_arg) {
                named_args_started = true;
                
                args_out[*arg_count_out].name = strdup(interpreter->current_token->value);
                interpreter_eat(interpreter, TOKEN_ID);
                interpreter_eat(interpreter, TOKEN_ASSIGN);
                
                ExprResult arg_expr_res = interpret_expression(interpreter);
                args_out[*arg_count_out].value = arg_expr_res.value;
                args_out[*arg_count_out].is_fresh = arg_expr_res.is_freshly_created_container;
                // START FIX: Check for exception or yield during argument evaluation
                if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
                    // An exception occurred while evaluating an argument.
                    // The argument value might be a partial/dummy value.
                    // The caller (interpret_any_function_call) will see the exception flag and handle cleanup.
                    if (peeked_token) free_token(peeked_token);
                    return; // Exit argument parsing immediately.
                }
                // END FIX
            } else {
                if (named_args_started) {
                    if (peeked_token) free_token(peeked_token);
                    report_error("Syntax", "Positional argument follows named argument.", interpreter->current_token);
                }
                
                args_out[*arg_count_out].name = NULL;
                
                ExprResult arg_expr_res = interpret_expression(interpreter);
                args_out[*arg_count_out].value = arg_expr_res.value;
                args_out[*arg_count_out].is_fresh = arg_expr_res.is_freshly_created_container;

                // START FIX: Check for exception or yield during argument evaluation
                if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
                    // An exception occurred while evaluating an argument.
                    // The argument value might be a partial/dummy value.
                    // The caller (interpret_any_function_call) will see the exception flag and handle cleanup.
                    if (peeked_token) free_token(peeked_token);
                    return; // Exit argument parsing immediately.
                }
                // END FIX
            }

            if (peeked_token) {
                free_token(peeked_token);
            }

            (*arg_count_out)++;

            if (interpreter->current_token->type == TOKEN_COMMA) {
                interpreter_eat(interpreter, TOKEN_COMMA);
                if (interpreter->current_token->type == TOKEN_RPAREN) {
                    report_error("Syntax", "Trailing comma in argument list.", interpreter->current_token);
                }
            } else {
                break;
            }
        } while (interpreter->current_token->type != TOKEN_RPAREN && interpreter->current_token->type != TOKEN_EOF);
    }

    interpreter_eat(interpreter, TOKEN_RPAREN);
}

Value interpret_any_function_call(Interpreter* interpreter, const char* func_name_str_or_null_for_bound, Token* func_name_token_for_error_reporting, Value* bound_method_val_or_null) {
    // This function is called after the ID (func_name or method_name) has been processed
    // and TOKEN_LPAREN is the current token. It will consume LPAREN, args, RPAREN.
    
    // START: Short-circuit check
    if (interpreter->prevent_side_effects) {
        interpreter_eat(interpreter, TOKEN_LPAREN);
        // We must still parse arguments to advance the lexer, but they will also be in "dry-run" mode.
        ParsedArgument parsed_args[10];
        int arg_count = 0;
        parse_call_arguments_with_named(interpreter, parsed_args, &arg_count, 10, func_name_token_for_error_reporting);
        // The argument names are heap-allocated and must be freed. The values are dummies.
        for (int i = 0; i < arg_count; ++i) {
            if (parsed_args[i].name) free(parsed_args[i].name);
            if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
        }
        return create_null_value();
    }
    // END: Short-circuit check

    DEBUG_PRINTF("ANY_FUNC_CALL_START: Func name '%s', Current token before LPAREN eat: %s ('%s')",
                 func_name_str_or_null_for_bound ? func_name_str_or_null_for_bound : (bound_method_val_or_null ? "BOUND_METHOD" : "NULL_NAME"),
                 token_type_to_string(interpreter->current_token->type),
                 interpreter->current_token->value ? interpreter->current_token->value : "N/A");

    interpreter_eat(interpreter, TOKEN_LPAREN); // Consume '('

    ParsedArgument parsed_args[10];
    int arg_count = 0;
    parse_call_arguments_with_named(interpreter, parsed_args, &arg_count, 10, func_name_token_for_error_reporting);

    // If an argument expression yielded or raised an exception, we must stop and propagate.
    // The arguments that were parsed will be re-evaluated on resume (for yield) or are now irrelevant (for exception).
    if ((interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) || interpreter->exception_is_active) {
        // Clean up any arguments that were successfully parsed before the yield/exception.
        for (int i = 0; i < arg_count; ++i) {
            if (parsed_args[i].name) free(parsed_args[i].name);
            if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
        }
        // Return a dummy value; the caller will see the yield/exception state in the interpreter.
        return create_null_value();
    }

    Value result;

    if (bound_method_val_or_null && bound_method_val_or_null->type == VAL_BOUND_METHOD) {
        BoundMethod* bm = bound_method_val_or_null->as.bound_method_val;
        if (bm->type == FUNC_TYPE_C_BUILTIN && bm->func_ptr.c_builtin == builtin_append) {
            // For C builtins, convert ParsedArgument to simple Value array and disallow named args.
            Value final_args_for_c_builtin[11]; // self + max 10 args
            final_args_for_c_builtin[0] = bm->self_value; // Array is self
            for (int i = 0; i < arg_count; ++i) {
                if (parsed_args[i].name) report_error("Runtime", "Built-in method 'append' does not support named arguments.", func_name_token_for_error_reporting);
                final_args_for_c_builtin[i+1] = parsed_args[i].value;
            }
            result = builtin_append(interpreter, final_args_for_c_builtin, arg_count + 1, func_name_token_for_error_reporting);
            // Cleanup parsed_args
            for (int i = 0; i < arg_count; ++i) {
                if (parsed_args[i].name) free(parsed_args[i].name);
                if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
            }
        } else if (bm->type == FUNC_TYPE_ECHOC) { // Regular EchoC method
            Object* self_obj_ptr = NULL;
            Function* func_to_run = bm->func_ptr.echoc_function;

            if (bm->self_value.type == VAL_OBJECT) {
                self_obj_ptr = bm->self_value.as.object_val;
            } else {
                for (int i = 0; i < arg_count; ++i) {
                    if (parsed_args[i].name) free(parsed_args[i].name);
                    if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                }
                report_error("Internal", "Bound method 'self' is not an object for a non-builtin method call.", func_name_token_for_error_reporting);
            }

            if (func_to_run->is_async) {
                // --- ASYNC METHOD CALL ---
                Coroutine* coro = calloc(1, sizeof(Coroutine));
                if (!coro) {
                    for (int i = 0; i < arg_count; ++i) {
                        if (parsed_args[i].name) free(parsed_args[i].name);
                        if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                    }
                    report_error("System", "Failed to allocate Coroutine object for async method.", func_name_token_for_error_reporting);
                }
                coro->magic_number = COROUTINE_MAGIC;
                coro->creation_line = func_name_token_for_error_reporting->line;
                coro->creation_col = func_name_token_for_error_reporting->col;
                coro->function_def = func_to_run;
                coro->ref_count = 1;
                coro->statement_resume_state = func_to_run->body_start_state;
                coro->statement_resume_state.text = func_to_run->source_text_owned_copy;
                coro->statement_resume_state.text_length = func_to_run->source_text_length;

                Scope* old_interpreter_scope = interpreter->current_scope;
                interpreter->current_scope = func_to_run->definition_scope;
                enter_scope(interpreter);
                coro->execution_scope = interpreter->current_scope;

                // Manually insert 'self' into the new coroutine's scope
                SymbolNode* self_node = (SymbolNode*)malloc(sizeof(SymbolNode));
                if (!self_node) { /* error handling */ }
                self_node->name = strdup("self");
                self_node->value.type = VAL_OBJECT;
                self_node->value.as.object_val = self_obj_ptr;
                self_node->next = coro->execution_scope->symbols;
                coro->execution_scope->symbols = self_node;

                // Argument handling for async methods
                int self_offset = 1; // 'self' is param 0
                int non_self_param_count = func_to_run->param_count - self_offset;

                int min_required_args = 0;
                for (int i = self_offset; i < func_to_run->param_count; ++i) {
                    if (!func_to_run->params[i].default_value) min_required_args++;
                }

                if (arg_count < min_required_args || arg_count > non_self_param_count) {
                    exit_scope(interpreter); // This frees coro->execution_scope
                    interpreter->current_scope = old_interpreter_scope;
                    free(coro); // Free the coroutine struct itself
                    for (int i = 0; i < arg_count; ++i) {
                        if (parsed_args[i].name) free(parsed_args[i].name);
                        if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                    }
                    char err_msg[250];
                    snprintf(err_msg, sizeof(err_msg), "Async method '%s' expects %d arguments (%d required), but %d were given.",
                             func_to_run->name, non_self_param_count, min_required_args, arg_count);
                    report_error("Runtime", err_msg, func_name_token_for_error_reporting);
                }

                // Set provided arguments
                for (int i = 0; i < arg_count; ++i) {
                    symbol_table_set(coro->execution_scope, func_to_run->params[i + self_offset].name, parsed_args[i].value);
                }

                // Set default values for remaining parameters
                for (int i = arg_count; i < non_self_param_count; ++i) {
                    int param_idx = i + self_offset;
                    if (func_to_run->params[param_idx].default_value) {
                        symbol_table_set(coro->execution_scope, func_to_run->params[param_idx].name, *(func_to_run->params[param_idx].default_value));
                    }
                }

                // Cleanup parsed args
                for (int i = 0; i < arg_count; ++i) {
                    if (parsed_args[i].name) free(parsed_args[i].name);
                    if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                }

                interpreter->current_scope = old_interpreter_scope;

                coro->name = strdup(func_to_run->name);
                coro->state = CORO_NEW;
                coro->result_value = create_null_value();
                coro->exception_value = create_null_value();
                coro->value_from_await = create_null_value();
                coro->gather_first_exception_idx = -1;
                coro->try_catch_stack_top = NULL;
                coro->has_yielding_await_state = false;
                coro->yielding_await_token = NULL;

                result.type = VAL_COROUTINE;
                result.as.coroutine_val = coro;
            } else {
                // --- SYNC METHOD CALL ---
                result = execute_echoc_function(interpreter, func_to_run, self_obj_ptr, parsed_args, arg_count, func_name_token_for_error_reporting);
            }
        } else {
            report_error("Internal", "Unknown bound method type in call.", func_name_token_for_error_reporting);
        }
    }
    // =================== START: ADD THIS NEW BLOCK ===================
    else if (bound_method_val_or_null && bound_method_val_or_null->type == VAL_FUNCTION) {
        // This handles calls to already-resolved functions (e.g., static methods).
        Function* func_to_run = bound_method_val_or_null->as.function_val;

        // The following logic is copied from the name-lookup path in the 'else' block below.
        if (func_to_run->is_async) {
            // Use calloc to ensure all fields are zero-initialized (e.g. is_cancelled, pointers)
            Coroutine* coro = calloc(1, sizeof(Coroutine));
            if (!coro) {
                for (int i = 0; i < arg_count; ++i) {
                    if (parsed_args[i].name) free(parsed_args[i].name);
                    if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                }
                report_error("System", "Failed to allocate Coroutine object.", func_name_token_for_error_reporting);
            }
            coro->magic_number = COROUTINE_MAGIC;
            coro->creation_line = func_name_token_for_error_reporting->line;
            coro->creation_col = func_name_token_for_error_reporting->col;
            coro->function_def = func_to_run;
            coro->ref_count = 1;
            coro->statement_resume_state = func_to_run->body_start_state;
            coro->statement_resume_state.text = func_to_run->source_text_owned_copy;
            coro->statement_resume_state.text_length = func_to_run->source_text_length;
            DEBUG_PRINTF("CORO_CREATE (Resolved VAL_FUNCTION): Initialized ref_count for %s (%p) to 1", func_to_run->name, (void*)coro);

            Scope* old_interpreter_scope = interpreter->current_scope;
            interpreter->current_scope = func_to_run->definition_scope;
            enter_scope(interpreter);
            coro->execution_scope = interpreter->current_scope;

            int min_required_args = 0;
            for (int i = 0; i < func_to_run->param_count; ++i) {
                if (!func_to_run->params[i].default_value) min_required_args++;
            }
            if (arg_count < min_required_args || arg_count > func_to_run->param_count) {
                exit_scope(interpreter);
                interpreter->current_scope = old_interpreter_scope;
                free(coro);
                for (int i = 0; i < arg_count; ++i) {
                    if (parsed_args[i].name) free(parsed_args[i].name);
                    if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                }
                char err_msg[250];
                snprintf(err_msg, sizeof(err_msg), "Async function '%s' expects %d arguments (%d required), but %d were given.",
                            func_to_run->name, func_to_run->param_count, min_required_args, arg_count);
                report_error("Runtime", err_msg, func_name_token_for_error_reporting);
            }

            // This simplified logic for async functions does not yet support named args.
            // A full implementation would require mapping logic similar to execute_echoc_function.
            for (int i = 0; i < arg_count; ++i) {
                if (i < func_to_run->param_count) {
                    symbol_table_set(coro->execution_scope, func_to_run->params[i].name, parsed_args[i].value);
                }
            }
            // Set defaults for remaining params
            for (int i = arg_count; i < func_to_run->param_count; ++i) {
                if (func_to_run->params[i].default_value) {
                     symbol_table_set(coro->execution_scope, func_to_run->params[i].name, *(func_to_run->params[i].default_value));
                }
            }

            // Now, safely clean up all parsed arguments.
            for (int i = 0; i < arg_count; ++i) {
                if (parsed_args[i].name) free(parsed_args[i].name);
                if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
            }
            interpreter->current_scope = old_interpreter_scope;

            coro->name = strdup(func_to_run->name);
            coro->state = CORO_NEW;
            coro->result_value = create_null_value();
            coro->exception_value = create_null_value();
            coro->value_from_await = create_null_value();
            coro->gather_first_exception_idx = -1;
            coro->try_catch_stack_top = NULL;
            coro->has_yielding_await_state = false; // Initialize new field
            coro->yielding_await_token = NULL;

            result.type = VAL_COROUTINE;
            result.as.coroutine_val = coro;
        } else {
            result = execute_echoc_function(interpreter, func_to_run, NULL, parsed_args, arg_count, func_name_token_for_error_reporting);
        }
    }
    // =================== START: ADD THIS NEW BLOCK ===================
    else { // Regular function call (not bound method)
        if (!func_name_str_or_null_for_bound) {
            report_error("Internal", "Function name missing for non-bound call.", func_name_token_for_error_reporting);
        }
        const char* func_name_str = func_name_str_or_null_for_bound;
        
        if (is_builtin_function(func_name_str)) {
            if (strcmp(func_name_str, "show") == 0) {
                result = builtin_show(interpreter, parsed_args, arg_count, func_name_token_for_error_reporting);
            } else {
                // Convert ParsedArgument to simple Value array for other built-ins and disallow named args.
                Value simple_args[arg_count];
                for (int i = 0; i < arg_count; ++i) {
                    if (parsed_args[i].name) {
                        char err_msg[250];
                        snprintf(err_msg, sizeof(err_msg), "Built-in function '%s' does not support named arguments.", func_name_str);
                        report_error("Runtime", err_msg, func_name_token_for_error_reporting);
                    }
                    simple_args[i] = parsed_args[i].value;
                }
                if (strcmp(func_name_str, "slice") == 0) {
                    result = builtin_slice(interpreter, simple_args, arg_count, func_name_token_for_error_reporting);
                } else if (strcmp(func_name_str, "type") == 0) {
                    result = builtin_type(interpreter, simple_args, arg_count, func_name_token_for_error_reporting);
                }
            }
            // Centralized cleanup for ALL built-ins.
            // The built-in functions themselves do not free the argument values.
            for (int i = 0; i < arg_count; ++i) {
                if (parsed_args[i].name) free(parsed_args[i].name);
                if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
            }
        } else { // It must be a user-defined function.
            Value* func_val_ptr = symbol_table_get(interpreter->current_scope, func_name_str);
            if (func_val_ptr && func_val_ptr->type == VAL_FUNCTION) {
                Function* func_to_run = func_val_ptr->as.function_val;
                if (func_to_run->is_async) {
                    // Use calloc to ensure all fields are zero-initialized (e.g. is_cancelled, pointers)
                    Coroutine* coro = calloc(1, sizeof(Coroutine));
                    if (!coro) {
                        for (int i = 0; i < arg_count; ++i) {
                            if (parsed_args[i].name) free(parsed_args[i].name);
                            if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                        }
                        report_error("System", "Failed to allocate Coroutine object.", func_name_token_for_error_reporting);
                    }
                    coro->magic_number = COROUTINE_MAGIC;
                    coro->creation_line = func_name_token_for_error_reporting->line;
                    coro->creation_col = func_name_token_for_error_reporting->col;
                    coro->function_def = func_to_run;
                    coro->ref_count = 1;
                    coro->statement_resume_state = func_to_run->body_start_state;
                    coro->statement_resume_state.text = func_to_run->source_text_owned_copy;
                    coro->statement_resume_state.text_length = func_to_run->source_text_length;
                    DEBUG_PRINTF("CORO_CREATE (EchoC): Initialized ref_count for %s (%p) to 1", func_to_run->name, (void*)coro);

                    Scope* old_interpreter_scope = interpreter->current_scope;
                    interpreter->current_scope = func_to_run->definition_scope;
                    enter_scope(interpreter);
                    coro->execution_scope = interpreter->current_scope;

                    int min_required_args = 0;
                    for (int i = 0; i < func_to_run->param_count; ++i) {
                        if (!func_to_run->params[i].default_value) min_required_args++;
                    }
                    if (arg_count < min_required_args || arg_count > func_to_run->param_count) {
                        exit_scope(interpreter);
                        interpreter->current_scope = old_interpreter_scope;
                        free(coro);
                        for (int i = 0; i < arg_count; ++i) {
                            if (parsed_args[i].name) free(parsed_args[i].name);
                            if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                        }
                        char err_msg[250];
                        snprintf(err_msg, sizeof(err_msg), "Async function '%s' expects %d arguments (%d required), but %d were given.",
                                    func_to_run->name, func_to_run->param_count, min_required_args, arg_count);
                        report_error("Runtime", err_msg, func_name_token_for_error_reporting);
                    }

                    // This simplified logic for async functions does not yet support named args.
                    // A full implementation would require mapping logic similar to execute_echoc_function.
                    for (int i = 0; i < arg_count; ++i) {
                        if (i < func_to_run->param_count) {
                            symbol_table_set(coro->execution_scope, func_to_run->params[i].name, parsed_args[i].value);
                        }
                    }
                    // Set defaults for remaining params
                    for (int i = arg_count; i < func_to_run->param_count; ++i) {
                        if (func_to_run->params[i].default_value) {
                            symbol_table_set(coro->execution_scope, func_to_run->params[i].name, *(func_to_run->params[i].default_value));
                        }
                    }

                    // Now, safely clean up all parsed arguments.
                    for (int i = 0; i < arg_count; ++i) {
                        if (parsed_args[i].name) free(parsed_args[i].name);
                        if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                    }
                    interpreter->current_scope = old_interpreter_scope;

                    coro->name = strdup(func_to_run->name);
                    coro->state = CORO_NEW;
                    coro->result_value = create_null_value();
                    coro->exception_value = create_null_value();
                    coro->value_from_await = create_null_value();
                    coro->gather_first_exception_idx = -1;
                    coro->try_catch_stack_top = NULL;
                    coro->has_yielding_await_state = false; // Initialize new field
                    coro->yielding_await_token = NULL;

                    result.type = VAL_COROUTINE;
                    result.as.coroutine_val = coro;
                } else {
                    result = execute_echoc_function(interpreter, func_to_run, NULL, parsed_args, arg_count, func_name_token_for_error_reporting);
                }
            } else {
                char err_msg[300];
                for (int i = 0; i < arg_count; ++i) {
                    if (parsed_args[i].name) free(parsed_args[i].name);
                    if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
                }
                snprintf(err_msg, sizeof(err_msg), "Undefined function '%s'", func_name_str);
                report_error("Runtime", err_msg, func_name_token_for_error_reporting);
                result = create_null_value();
            }
        }
    }
    DEBUG_PRINTF("ANY_FUNC_CALL_END: Func name '%s', Current token before return: %s ('%s')",
                 func_name_str_or_null_for_bound ? func_name_str_or_null_for_bound : (bound_method_val_or_null ? "BOUND_METHOD" : "NULL_NAME"),
                 token_type_to_string(interpreter->current_token->type),
                 interpreter->current_token->value ? interpreter->current_token->value : "N/A");
    return result;
}

Value interpret_instance_creation(Interpreter* interpreter, Blueprint* bp_to_instantiate, Token* call_site_token) {
    // This function is called when TOKEN_LPAREN is the current token, after the blueprint ID.
    interpreter_eat(interpreter, TOKEN_LPAREN); // Consume '(' before parsing arguments
    // So, we directly parse arguments here.

    ParsedArgument parsed_args[10];
    int arg_count = 0;
    parse_call_arguments_with_named(interpreter, parsed_args, &arg_count, 10, call_site_token);


    Object* new_obj = malloc(sizeof(Object));
    if (!new_obj) report_error("System", "Failed to allocate memory for new object.", call_site_token);
    new_obj->id = next_object_id++;
    DEBUG_PRINTF("INSTANCE_CREATE: Created [Object #%llu] of blueprint '%s' at %p", new_obj->id, bp_to_instantiate->name, (void*)new_obj);
    new_obj->ref_count = 1; // Initialize ref_count
    new_obj->blueprint = bp_to_instantiate;
    new_obj->instance_attributes = malloc(sizeof(Scope));
    if (!new_obj->instance_attributes) { free(new_obj); report_error("System", "Failed to allocate instance attributes scope.", call_site_token); }
    new_obj->instance_attributes->symbols = NULL;
    new_obj->instance_attributes->outer = NULL; // Instance scope is isolated

    Value instance_val;
    instance_val.type = VAL_OBJECT;
    instance_val.as.object_val = new_obj;

    // Call init method if it exists
    Function* init_method = bp_to_instantiate->init_method_cache;
    if (!init_method) { // Try to find it if not cached (e.g. first time)
        Value* init_val_ptr = symbol_table_get_local(bp_to_instantiate->class_attributes_and_methods, "init");
        if (init_val_ptr && init_val_ptr->type == VAL_FUNCTION) {
            init_method = init_val_ptr->as.function_val;
            bp_to_instantiate->init_method_cache = init_method; // Cache it
        }
    }

    if (init_method) {
        if (init_method->is_async) {
            // If init is async, it creates a coroutine. This is unusual for constructors.
            // EchoC's 'run:' or an internal mechanism would be needed to execute this async init.
            // For now, we'll disallow async init or treat it as an error.
            // Free allocated object and its attributes before reporting error.
            free_scope(new_obj->instance_attributes); // This frees the scope struct and its symbols
            free(new_obj); // Free the object struct
            report_error("Runtime", "'init' method cannot be 'async'.", call_site_token);
            for (int i = 0; i < arg_count; ++i) {
                if (parsed_args[i].name) free(parsed_args[i].name);
                if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
            }
        } else {
            Value init_result = execute_echoc_function(interpreter, init_method, new_obj, parsed_args, arg_count, call_site_token);
            if (init_result.type != VAL_NULL) {
                 DEBUG_PRINTF("Init method for %s returned non-null (type %d). Discarding and freeing its contents.", bp_to_instantiate->name, init_result.type);
                 free_value_contents(init_result);
            }
        }
    } else {
        // No explicit init. Check if any arguments were passed.
        if (arg_count > 0) { // Arguments were parsed by parse_call_arguments
            // Free allocated object and its attributes before reporting error.
            if (new_obj->instance_attributes) {
                free_scope(new_obj->instance_attributes); // This frees the scope struct and its symbols
            }
            free(new_obj); // Free the object struct
            for (int i = 0; i < arg_count; ++i) {
                if (parsed_args[i].name) free(parsed_args[i].name);
                if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
            }
            report_error("Runtime", "Blueprint has no 'init' method but arguments were provided for instantiation.", call_site_token);
        }
        // RPAREN was already consumed by parse_call_arguments
    }
    return instance_val;
}

ExprResult interpret_primary_expr(Interpreter* interpreter) {
    Token* token = interpreter->current_token;
    Value val;
    ExprResult expr_res;
    expr_res.is_standalone_primary_id = false; // Default for most primaries
    expr_res.is_freshly_created_container = false; // Default

    if (token->type == TOKEN_LBRACE) { 
        expr_res.value = interpret_dictionary_literal(interpreter);

        if (interpreter->exception_is_active) {
            expr_res.is_freshly_created_container = false; 
        } else {
            if (expr_res.value.type == VAL_DICT) expr_res.is_freshly_created_container = true;
        }
        return expr_res;
    } else if (token->type == TOKEN_INTEGER) {
        val.type = VAL_INT;
        val.as.integer = atol(token->value);
        interpreter_eat(interpreter, TOKEN_INTEGER);
        expr_res.value = val; return expr_res; // Corrected
    } else if (token->type == TOKEN_FLOAT) {
        val.type = VAL_FLOAT;
        val.as.floating = atof(token->value);
        interpreter_eat(interpreter, TOKEN_FLOAT);
        expr_res.value = val; return expr_res;
    } else if (token->type == TOKEN_STRING) {
        // Make a copy of the token's value for interpolation, as the original token
        // (and its value) will be freed by interpreter_eat.
        // Pass the token's value directly. evaluate_interpolated_string is now responsible for copying it,
        // as the token will be consumed and its value freed after this call.
        expr_res.value = evaluate_interpolated_string(interpreter, token->value, token);
        
 
        // Always consume the original string token from the stream.
        interpreter_eat(interpreter, TOKEN_STRING);
 
        // Now, handle the result.
        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            // If an error occurred, the value from evaluate_interpolated_string is a dummy VAL_NULL, so no need to free it here.
            DEBUG_PRINTF("PRIMARY_EXPR: Exception or Yield propagated from string interpolation. Cleaning up.%s", "");
            // The returned value is a dummy. Mark it as not fresh.
            expr_res.is_freshly_created_container = false;
        } else {
            // A string from evaluation/interpolation is always a new allocation.
            if (expr_res.value.type == VAL_STRING) expr_res.is_freshly_created_container = true;
        }

        return expr_res;
    } else if (token->type == TOKEN_TRUE) {
        val.type = VAL_BOOL;
        val.as.bool_val = 1;
        interpreter_eat(interpreter, TOKEN_TRUE);
        expr_res.value = val; return expr_res;
    } else if (token->type == TOKEN_FALSE) {
        val.type = VAL_BOOL;
        val.as.bool_val = 0;
        interpreter_eat(interpreter, TOKEN_FALSE);
        expr_res.value = val; return expr_res;
    } else if (token->type == TOKEN_NULL) {
        val.type = VAL_NULL;
        // No .as field for VAL_NULL
        interpreter_eat(interpreter, TOKEN_NULL);
        expr_res.value = val; return expr_res; // Corrected
    } else if (token->type == TOKEN_LPAREN) {
        Token* lparen_token_for_error_context = token;
        interpreter_eat(interpreter, TOKEN_LPAREN);

        if (interpreter->current_token->type == TOKEN_RPAREN) { // Empty tuple: ()
            interpreter_eat(interpreter, TOKEN_RPAREN);
            Tuple* tuple = malloc(sizeof(Tuple));
            if (!tuple) report_error("System", "Failed to allocate memory for empty tuple struct", lparen_token_for_error_context);
            tuple->count = 0;
            tuple->elements = NULL;
            val.type = VAL_TUPLE;
            val.as.tuple_val = tuple;
            expr_res.value = val; expr_res.is_freshly_created_container = true;
            return expr_res;
        } else { 
            ExprResult first_element_res = interpret_expression(interpreter);
            if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
                // If the very first item in a potential tuple yields, just propagate.
                // No tuple has been allocated yet. The caller will handle freeing first_element_res if needed.
                return first_element_res;
            }
            if (interpreter->current_token->type == TOKEN_COMMA) { // Non-empty tuple
                interpreter_eat(interpreter, TOKEN_COMMA);
                
                Tuple* tuple = malloc(sizeof(Tuple));
                if (!tuple) report_error("System", "Failed to allocate memory for tuple struct", lparen_token_for_error_context);
                
                int capacity = 8;
                tuple->elements = malloc(capacity * sizeof(Value));
                if (!tuple->elements) { free(tuple); report_error("System", "Failed to allocate memory for tuple elements", lparen_token_for_error_context); }
                
                tuple->count = 0;
                tuple->elements[tuple->count++] = first_element_res.value; // Store Value part

                while (interpreter->current_token->type != TOKEN_RPAREN && interpreter->current_token->type != TOKEN_EOF) {
                    if (tuple->count >= capacity) {
                        capacity *= 2;
                        tuple->elements = realloc(tuple->elements, capacity * sizeof(Value));
                        if (!tuple->elements) report_error("System", "Failed to reallocate memory for tuple elements", interpreter->current_token);
                    }
                    ExprResult next_elem_res = interpret_expression(interpreter);
                    
                    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
                        // An element's expression yielded. We must clean up the partial tuple.
                        if (next_elem_res.is_freshly_created_container) free_value_contents(next_elem_res.value);
                        // Also free the first element that was stored
                        if (first_element_res.is_freshly_created_container) free_value_contents(first_element_res.value);
                        Value temp_tuple_val = {.type = VAL_TUPLE, .as.tuple_val = tuple};
                        free_value_contents(temp_tuple_val); // This will free the tuple and its elements
                        return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                    }
                    tuple->elements[tuple->count++] = next_elem_res.value;
                    if (interpreter->current_token->type == TOKEN_COMMA) {
                        interpreter_eat(interpreter, TOKEN_COMMA);
                    } else {
                        break; 
                    }
                }
                interpreter_eat(interpreter, TOKEN_RPAREN);
                val.type = VAL_TUPLE;
                val.as.tuple_val = tuple;
                expr_res.value = val; expr_res.is_freshly_created_container = true;
                return expr_res;
            } else { // Grouped expression
                interpreter_eat(interpreter, TOKEN_RPAREN);
                return first_element_res; // Propagate ExprResult from inner expression
            }
        }
        report_error("Internal", "Reached end of TOKEN_LPAREN block in interpret_primary_expr unexpectedly.", lparen_token_for_error_context);
    } else if (token->type == TOKEN_ID) {
        char* id_name = strdup(interpreter->current_token->value);
        DEBUG_PRINTF("PRIMARY_EXPR_ID_START: Current token before eat: %s ('%s'), id_name: '%s'",
                     token_type_to_string(interpreter->current_token->type),
                     interpreter->current_token->value, id_name);
        Token* id_token_for_reporting = token_deep_copy(interpreter->current_token); // Deep copy for error reporting
        bool original_token_type_was_id = true;
        interpreter_eat(interpreter, TOKEN_ID);

        DEBUG_PRINTF("PRIMARY_EXPR_ID_AFTER_EAT: Current token after eating '%s': %s ('%s')",
                     id_name,
                     token_type_to_string(interpreter->current_token->type),
                     interpreter->current_token->value ? interpreter->current_token->value : "N/A");

        if (interpreter->current_token->type == TOKEN_LPAREN) { // It's a call expression
            // First, check if it's a built-in function call.
            if (is_builtin_function(id_name)) {
                expr_res.value = interpret_any_function_call(interpreter, id_name, id_token_for_reporting, NULL);
                // Mark the result as a fresh container if it is one
                if (expr_res.value.type >= VAL_STRING && expr_res.value.type <= VAL_GATHER_TASK) {
                    expr_res.is_freshly_created_container = true;
                }
            } else {
                // If not a built-in, look it up in the symbol table.
                Value* id_val_ptr = symbol_table_get(interpreter->current_scope, id_name);

                if (id_val_ptr && id_val_ptr->type == VAL_BLUEPRINT) {
                    // It's a blueprint instantiation.
                    expr_res.value = interpret_instance_creation(interpreter, id_val_ptr->as.blueprint_val, id_token_for_reporting);
                    if (expr_res.value.type == VAL_OBJECT) {
                        expr_res.is_freshly_created_container = true;
                    }
                } else if (id_val_ptr && id_val_ptr->type == VAL_FUNCTION) {
                    // It's a user-defined function call.
                    expr_res.value = interpret_any_function_call(interpreter, id_name, id_token_for_reporting, NULL);
                    // Mark the result as a fresh container if it is one
                    if (expr_res.value.type >= VAL_STRING && expr_res.value.type <= VAL_GATHER_TASK) {
                        expr_res.is_freshly_created_container = true;
                    }
                } else {
                    // If it's not a built-in, not a blueprint, and not a user function, it's an error.
                    char err_msg[300];
                    snprintf(err_msg, sizeof(err_msg), "Identifier '%s' is not a callable function or instantiable blueprint.", id_name);
                    free(id_name);
                    free_token(id_token_for_reporting);
                    report_error("Runtime", err_msg, id_token_for_reporting);
                }
            }
        }

        // If not a call, it's a variable or super
        else if (strcmp(id_name, "super") == 0) { // Changed to else if
            if (!interpreter->current_self_object) {
                free(id_name);
                free_token(id_token_for_reporting);
                report_error("Runtime", "'super' can only be used within an instance method.", id_token_for_reporting);
            }
            // --- Debug for 'super' path ---
            DEBUG_PRINTF("PRIMARY_EXPR_ID_SUPER_PATH: VarName was 'super'. Scope %p. Scope symbols head: %s (NodeAddr: %p). Outer: %p",
                         (void*)interpreter->current_scope,
                         interpreter->current_scope->symbols ? interpreter->current_scope->symbols->name : "NULL_HEAD",
                         (void*)interpreter->current_scope->symbols,
                         (void*)interpreter->current_scope->outer);
            SymbolNode* temp_sym_super = interpreter->current_scope->symbols;
            while(temp_sym_super) { // Log all symbols in current scope if 'super' is encountered
                DEBUG_PRINTF("    SUPER_SCOPE_SYM: '%s' (NodeAddr: %p)", temp_sym_super->name, (void*)temp_sym_super);
                temp_sym_super = temp_sym_super->next;
            }
            val.type = VAL_SUPER_PROXY;
            // No data needed in val.as for VAL_SUPER_PROXY
            expr_res.value = val; expr_res.is_standalone_primary_id = false;
        } else {
            Value* var_val_ptr = symbol_table_get(interpreter->current_scope, id_name); // Regular variable lookup
            // Add this debug log to check the scope's head just before the lookup
            DEBUG_PRINTF("PRIMARY_EXPR_ID_LOOKUP: Var '%s'. Scope %p. Scope symbols head: %s. Outer: %p",
                         id_name, (void*)interpreter->current_scope, 
                         interpreter->current_scope->symbols ? interpreter->current_scope->symbols->name : "NULL_HEAD",
                         (void*)interpreter->current_scope->outer);
            // --- Add detailed symbol list dump ---
            DEBUG_PRINTF("  Detailed symbols in scope %p (for var '%s' lookup):", (void*)interpreter->current_scope, id_name);
            SymbolNode* temp_sym = interpreter->current_scope->symbols;
            int sym_count = 0;
            while(temp_sym && sym_count < 20) { // Limit to 20 to avoid huge logs
                DEBUG_PRINTF("    -> Symbol: '%s' (Node Addr: %p, Next Addr: %p)", temp_sym->name, (void*)temp_sym, (void*)temp_sym->next);
                temp_sym = temp_sym->next;
                sym_count++;
            }
            if (temp_sym) DEBUG_PRINTF("    -> ... (more symbols)%s", "");
            // --- End detailed symbol list dump ---
            if (var_val_ptr == NULL) {
                char err_msg[100];
                snprintf(err_msg, sizeof(err_msg), "Undefined variable '%s'", id_name);
                free(id_name);
                interpreter->exception_is_active = 1;
                free_value_contents(interpreter->current_exception);
                interpreter->current_exception.type = VAL_STRING;
                interpreter->current_exception.as.string_val = strdup(err_msg);
                if (interpreter->error_token) free_token(interpreter->error_token);
                interpreter->error_token = id_token_for_reporting; // Transfer ownership of the token
                expr_res.value = create_null_value(); // Return a dummy value
                expr_res.is_freshly_created_container = false; // Not a fresh container
                return expr_res; // Propagate the error flag
            }
            // For objects, arrays, dicts from variables, pass by reference (share the pointer in the Value struct).
            // Other types (primitives, strings that are copied by value_deep_copy, functions) are deep copied.
            // When a container is retrieved by name, we want a "view" (shallow copy of the Value struct)
            // not a deep copy, so that subsequent operations like indexing work on the original data.
            // This prevents the original container from being freed prematurely by the postfix expression handler.
            if (var_val_ptr->type == VAL_OBJECT || var_val_ptr->type == VAL_ARRAY || var_val_ptr->type == VAL_DICT || var_val_ptr->type == VAL_TUPLE) {
                expr_res.value = *var_val_ptr; // Shallow copy of Value struct; shares the data pointer.
                expr_res.is_freshly_created_container = false;
                expr_res.is_standalone_primary_id = true; // This is a standalone ID lookup
            } else {
                expr_res.value = value_deep_copy(*var_val_ptr);
                // If value_deep_copy created a new container (string, array, dict, tuple, object, function, coroutine)
                // or a new reference-counted handle (coroutine), it's considered "fresh" in terms of this Value wrapper.
                if (expr_res.value.type == VAL_STRING || expr_res.value.type == VAL_ARRAY ||
                    expr_res.value.type == VAL_DICT || expr_res.value.type == VAL_TUPLE ||
                    expr_res.value.type == VAL_FUNCTION || // Functions are still copied (new Function struct)
                    expr_res.value.type == VAL_COROUTINE || expr_res.value.type == VAL_GATHER_TASK) { // Coroutines are ref-counted
                    expr_res.is_freshly_created_container = true;
                } else if (expr_res.value.type == VAL_OBJECT || expr_res.value.type == VAL_BOUND_METHOD) {
                    // For objects and bound methods, value_deep_copy increments ref_count.
                    // The returned Value is a "new handle" to existing data. The caller is
                    // responsible for releasing this handle by calling free_value_contents.
                    expr_res.is_freshly_created_container = true;
                } else { // Primitives, VAL_NULL, VAL_BLUEPRINT (ptr copy)
                    expr_res.is_freshly_created_container = false;
                    if (original_token_type_was_id) expr_res.is_standalone_primary_id = true; // If original was ID, it's standalone
                }
            }
        }
        free(id_name); // Free the strdup'd name
        free_token(id_token_for_reporting); // Free the deep-copied token
        DEBUG_PRINTF("PRIMARY_EXPR_ID_END: Current token before return: %s ('%s')",
                     token_type_to_string(interpreter->current_token->type),
                     interpreter->current_token->value ? interpreter->current_token->value : "N/A");

        return expr_res;
    } else if (token->type == TOKEN_SUPER) { // Handle 'super' keyword as a primary expression
        if (!interpreter->current_self_object) {
            report_error("Runtime", "'super' can only be used within an instance method.", token);
        }
        val.type = VAL_SUPER_PROXY;
        interpreter_eat(interpreter, TOKEN_SUPER);
        expr_res.value = val; expr_res.is_standalone_primary_id = false; return expr_res;
    } else if (token->type == TOKEN_LBRACKET) { // Array literal
        interpreter_eat(interpreter, TOKEN_LBRACKET);

        Array* array = malloc(sizeof(Array));
        if (!array) report_error("System", "Failed to allocate memory for array struct", token);
        array->count = 0;
        array->capacity = 8;
        array->elements = malloc(array->capacity * sizeof(Value));
        if (!array->elements) {
            free(array);
            report_error("System", "Failed to allocate memory for array elements", token);
        }

        if (interpreter->current_token->type != TOKEN_RBRACKET) {
            do {
                if (array->count >= array->capacity) {
                    array->capacity *= 2;
                    array->elements = realloc(array->elements, array->capacity * sizeof(Value));
                    if (!array->elements) report_error("System", "Failed to reallocate memory for array elements", interpreter->current_token);
                }
                ExprResult elem_res = interpret_expression(interpreter);

                if (interpreter->exception_is_active) {
                    if (elem_res.is_freshly_created_container) {
                        free_value_contents(elem_res.value);
                    }
                    // Clean up partially constructed array
                    Value temp_array_val_to_free = {.type = VAL_ARRAY, .as.array_val = array};
                    free_value_contents(temp_array_val_to_free);
                    ExprResult error_res = { .value = create_null_value(), .is_freshly_created_container = false, .is_standalone_primary_id = false };
                    return error_res;
                }

                array->elements[array->count++] = elem_res.value;
                if (interpreter->current_token->type == TOKEN_COMMA) {
                    interpreter_eat(interpreter, TOKEN_COMMA);
                } else {
                    break;
                }
            } while (interpreter->current_token->type != TOKEN_RBRACKET && interpreter->current_token->type != TOKEN_EOF);
        }
        interpreter_eat(interpreter, TOKEN_RBRACKET);
        val.type = VAL_ARRAY;
        val.as.array_val = array;
        expr_res.value = val; expr_res.is_freshly_created_container = true;
        expr_res.is_standalone_primary_id = false; return expr_res;
    }
    report_error("Syntax", "Expected a number, string, boolean, variable, '(', '[', or '{' to start an expression factor", token);
    // Should not be reached
    expr_res.value.type = VAL_BOOL; expr_res.value.as.bool_val = 0; return expr_res;
}

ExprResult interpret_postfix_expr(Interpreter* interpreter) {
    ExprResult current_expr_res = interpret_primary_expr(interpreter);
    // If the primary expression itself yielded or had an error, propagate immediately.
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return current_expr_res;
    }

    Value result = current_expr_res.value; // The actual value being operated on
    // is_freshly_created_container applies to this initial 'result'
    bool result_is_freshly_created = current_expr_res.is_freshly_created_container;
    bool is_still_standalone_id = current_expr_res.is_standalone_primary_id;
    
    while (interpreter->current_token->type == TOKEN_LBRACKET ||
           interpreter->current_token->type == TOKEN_DOT ||
           (result.type == VAL_FUNCTION && interpreter->current_token->type == TOKEN_LPAREN) || /* Function call on a resolved VAL_FUNCTION */
           (result.type == VAL_BOUND_METHOD && interpreter->current_token->type == TOKEN_LPAREN) || /* Bound method call */
           (result.type == VAL_BLUEPRINT && interpreter->current_token->type == TOKEN_LPAREN) || /* Blueprint instantiation */
           ((result.type == VAL_COROUTINE || result.type == VAL_GATHER_TASK) && interpreter->current_token->type == TOKEN_LPAREN) /* Calling a coroutine (error) or a C-builtin that returns coro */
          ) {
        Value next_derived_value; // This will become the new 'result'
        bool next_derived_is_fresh = false; // And its freshness
        is_still_standalone_id = false; // Any postfix operation means it's no longer a standalone ID

        if (result.type == VAL_FUNCTION && interpreter->current_token->type == TOKEN_LPAREN) {
            // Handle immediate call of a VAL_FUNCTION that was resolved (e.g., from a variable like `my_func()`)
            Value func_val_to_call = result; // This is the temporary, fresh VAL_FUNCTION
            
            // FIX: Create a copy of the LPAREN token for safe error reporting.
            Token* lparen_token_for_error = token_deep_copy(interpreter->current_token);

            // Pass the resolved function value itself, not its name, to avoid re-lookup.
            // The name argument is NULL. The last argument is a pointer to the VAL_FUNCTION,
            // which our newly added logic in interpret_any_function_call now handles.
            next_derived_value = interpret_any_function_call(interpreter, NULL, lparen_token_for_error, &func_val_to_call);
            
            // Free the copied token now that the call is complete.
            free_token(lparen_token_for_error);
            
            // Set freshness for the call result
            if (next_derived_value.type == VAL_OBJECT || next_derived_value.type == VAL_ARRAY ||
                next_derived_value.type == VAL_DICT || next_derived_value.type == VAL_STRING ||
                next_derived_value.type == VAL_TUPLE || next_derived_value.type == VAL_BOUND_METHOD ||
                next_derived_value.type == VAL_COROUTINE || next_derived_value.type == VAL_GATHER_TASK) {
                next_derived_is_fresh = true;
            } else {
                next_derived_is_fresh = false;
            }
        } else if (result.type == VAL_BOUND_METHOD && interpreter->current_token->type == TOKEN_LPAREN) {
            // Handle immediate call of a bound method: e.g. obj.method()
            Value bound_method_to_call = result; // Keep the VAL_BOUND_METHOD to free its contents

            // FIX: Create a copy of the LPAREN token for safe error reporting.
            Token* lparen_token_for_error = token_deep_copy(interpreter->current_token);

            next_derived_value = interpret_any_function_call(interpreter, NULL, lparen_token_for_error, &bound_method_to_call); // This path was already correct

            // Free the copied token.
            free_token(lparen_token_for_error);
            // Result of a function call is considered new/temporary if it's a container or coroutine
            if (next_derived_value.type >= VAL_STRING && next_derived_value.type <= VAL_GATHER_TASK && next_derived_value.type != VAL_BLUEPRINT && next_derived_value.type != VAL_SUPER_PROXY) { // More general check for containers/complex types
                next_derived_is_fresh = true;
            } else {
                next_derived_is_fresh = false;
            }
        } else if (result.type == VAL_BLUEPRINT && interpreter->current_token->type == TOKEN_LPAREN) {
            // Handle blueprint instantiation: e.g. some_expression_resulting_in_blueprint(...)
            Value blueprint_to_instantiate_val = result; // This is the VAL_BLUEPRINT
            // interpret_instance_creation expects the LPAREN to be current, then it eats it.
            // interpreter->current_token is LPAREN here.
            next_derived_value = interpret_instance_creation(interpreter, blueprint_to_instantiate_val.as.blueprint_val, interpreter->current_token);
            // interpret_instance_creation consumes LPAREN, args, RPAREN.
            if (next_derived_value.type == VAL_OBJECT) {
                next_derived_is_fresh = true;
            }
            // The VAL_BLUEPRINT in blueprint_to_instantiate_val was part of 'result'.
            // If 'result' was fresh (it was a deep copy), it needs to be freed.
            if (result_is_freshly_created) {
                free_value_contents(result); // Free the VAL_BLUEPRINT's contents
            }
        } else if ((result.type == VAL_COROUTINE || result.type == VAL_GATHER_TASK) && interpreter->current_token->type == TOKEN_LPAREN) {
            // This means something like `my_coro_obj()` which is not allowed after creation.
            if (result_is_freshly_created) { // Free the coroutine object if it was temporary
                free_value_contents(result); // Free the coroutine's contents
            }
            // Report error and set exception flag
            interpreter->exception_is_active = 1;
            free_value_contents(interpreter->current_exception);
            interpreter->current_exception.type = VAL_STRING;
            interpreter->current_exception.as.string_val = strdup("Cannot call a coroutine object directly. Use 'await' or 'weaver.spawn_task'.");
            if (interpreter->error_token) free_token(interpreter->error_token);
            interpreter->error_token = token_deep_copy(interpreter->current_token); // Use current token (LPAREN) for error context
            // The loop will break and propagate this error.
            next_derived_value = create_null_value(); // Dummy value
            next_derived_is_fresh = false;
            break; // Exit the while loop to propagate the error
        } else if (interpreter->current_token->type == TOKEN_LBRACKET) {

            Token* bracket_token = token_deep_copy(interpreter->current_token);
            interpreter_eat(interpreter, TOKEN_LBRACKET);
            ExprResult index_expr_res = interpret_expression(interpreter);
            Value index_val = index_expr_res.value;
            bool index_is_fresh = index_expr_res.is_freshly_created_container;
            
            if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) { // Exception or YIELD during index expression parsing
                if(result_is_freshly_created) free_value_contents(result); // Free base if it was fresh
                if(index_expr_res.is_freshly_created_container) free_value_contents(index_val);
                free_token(bracket_token);
                // For yield or exception, return a dummy value and let the caller see the interpreter state.
                return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false, .is_standalone_primary_id = false };
            }

            char* base_str_for_debug = value_to_string_representation(result, interpreter, bracket_token);
            char* index_str_for_debug = value_to_string_representation(index_val, interpreter, bracket_token);
            free(base_str_for_debug);
            free(index_str_for_debug);

            if (result.type == VAL_ARRAY) {
                if (index_val.type != VAL_INT) {
                    if(result_is_freshly_created) free_value_contents(result); // Free the base
                    if(index_is_fresh) free_value_contents(index_val); // Free the index
                    report_error("Runtime", "Array index must be an integer.", bracket_token);
                    interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                    free_value_contents(interpreter->current_exception);
                    interpreter->current_exception.type = VAL_STRING;
                    interpreter->current_exception.as.string_val = strdup("Array index must be an integer.");
                    if (interpreter->error_token) free_token(interpreter->error_token);
                    interpreter->error_token = token_deep_copy(bracket_token);
                    free_token(bracket_token);
                    return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                }

                Array* arr_ptr = result.as.array_val; 
                long idx = index_val.as.integer; 
                long effective_idx = idx;
                if (effective_idx < 0) effective_idx += arr_ptr->count;

                if (effective_idx < 0 || effective_idx >= arr_ptr->count) {
                    if(result_is_freshly_created) free_value_contents(result);
                    interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                    free_value_contents(interpreter->current_exception);
                    interpreter->current_exception.type = VAL_STRING;
                    interpreter->current_exception.as.string_val = strdup("Array index out of bounds.");
                    if (interpreter->error_token) free_token(interpreter->error_token);
                    interpreter->error_token = token_deep_copy(bracket_token);
                    free_token(bracket_token);
                    return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                }
                // Return a view (shallow copy of Value struct), not a deep copy.
                // The caller (e.g., assignment) is responsible for deep copying if needed.
                next_derived_value = arr_ptr->elements[effective_idx];
                next_derived_is_fresh = false;
            } else if (result.type == VAL_DICT) {
                if (index_val.type != VAL_STRING) {
                    if(result_is_freshly_created) free_value_contents(result);
                    if(index_is_fresh) free_value_contents(index_val);
                    interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                    free_value_contents(interpreter->current_exception);
                    interpreter->current_exception.type = VAL_STRING;
                    interpreter->current_exception.as.string_val = strdup("Dictionary key must be a string.");
                    if (interpreter->error_token) free_token(interpreter->error_token);
                    interpreter->error_token = token_deep_copy(bracket_token);
                    free_token(bracket_token);
                    return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                }
                Value element;

                // Get a view (shallow copy of Value struct) of the element.
                // The caller (e.g., assignment) is responsible for deep copying if needed.
                if (!dictionary_try_get(result.as.dict_val, index_val.as.string_val, &element, false)) {
                    char err_msg[150];
                    snprintf(err_msg, sizeof(err_msg), "Key '%s' not found in dictionary.", index_val.as.string_val);
                    if(result_is_freshly_created) free_value_contents(result); // Free the base
                    if (index_is_fresh) free_value_contents(index_val); // Free the index
                    interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                    free_value_contents(interpreter->current_exception);
                    interpreter->current_exception.type = VAL_STRING;
                    interpreter->current_exception.as.string_val = strdup(err_msg);
                    if (interpreter->error_token) free_token(interpreter->error_token);
                    interpreter->error_token = token_deep_copy(bracket_token);

                    free_token(bracket_token);
                    return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                }
                next_derived_value = element; // 'element' is a shallow copy of the Value struct.
                next_derived_is_fresh = false; // It's a view, not a fresh container.
            } else if (result.type == VAL_STRING) {
                if (index_val.type != VAL_INT) {
                    if(result_is_freshly_created) free_value_contents(result);
                    if(index_is_fresh) free_value_contents(index_val);
                    interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                    free_value_contents(interpreter->current_exception);
                    interpreter->current_exception.type = VAL_STRING;
                    interpreter->current_exception.as.string_val = strdup("String index must be an integer.");
                    if (interpreter->error_token) free_token(interpreter->error_token);
                    interpreter->error_token = token_deep_copy(bracket_token);
                    free_token(bracket_token);
                    return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                }
                long idx = index_val.as.integer;
                size_t str_len = strlen(result.as.string_val);
                if (idx < 0) idx = (long)str_len + idx;
                if (idx < 0 || (size_t)idx >= str_len) { // Cast idx to size_t for comparison
                    if(result_is_freshly_created) free_value_contents(result); // Free the base
                    interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                    free_value_contents(interpreter->current_exception);
                    interpreter->current_exception.type = VAL_STRING;
                    interpreter->current_exception.as.string_val = strdup("String index out of bounds.");
                    if (interpreter->error_token) free_token(interpreter->error_token);
                    interpreter->error_token = token_deep_copy(bracket_token);
                    free_token(bracket_token);
                    return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                }
                char* charStr = malloc(2);
                if (!charStr) report_error("System", "Memory allocation failed during string indexing.", bracket_token);
                charStr[0] = result.as.string_val[idx];
                charStr[1] = '\0';
                // index_val (if int) has no complex contents.
                next_derived_value.type = VAL_STRING; // This is a new string
                next_derived_value.as.string_val = charStr;
                next_derived_is_fresh = true; // New string
            } else if (result.type == VAL_TUPLE) {
                if (index_val.type != VAL_INT) {
                    if(result_is_freshly_created) free_value_contents(result);
                    if(index_is_fresh) free_value_contents(index_val);
                    interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                    free_value_contents(interpreter->current_exception);
                    interpreter->current_exception.type = VAL_STRING;
                    interpreter->current_exception.as.string_val = strdup("Tuple index must be an integer.");
                    if (interpreter->error_token) free_token(interpreter->error_token);
                    interpreter->error_token = token_deep_copy(bracket_token);
                    free_token(bracket_token);
                    return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                }
                Tuple* tuple_ptr = result.as.tuple_val;
                long idx = index_val.as.integer;
                long effective_idx = idx;
                if (effective_idx < 0) effective_idx += tuple_ptr->count;

                if (effective_idx < 0 || effective_idx >= tuple_ptr->count) {
                    if(result_is_freshly_created) free_value_contents(result);
                    interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                    free_value_contents(interpreter->current_exception);
                    interpreter->current_exception.type = VAL_STRING;
                    interpreter->current_exception.as.string_val = strdup("Tuple index out of bounds.");
                    if (interpreter->error_token) free_token(interpreter->error_token);
                    interpreter->error_token = token_deep_copy(bracket_token);
                    free_token(bracket_token);
                    return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
                }
                // Return a view (shallow copy of Value struct), not a deep copy.
                // The caller (e.g., assignment) is responsible for deep copying if needed.
                next_derived_value = tuple_ptr->elements[effective_idx];
                next_derived_is_fresh = false;
            } else {
                if(result_is_freshly_created) free_value_contents(result);
                if(index_is_fresh) free_value_contents(index_val);
                interpreter->exception_is_active = 1; // This part is likely unreachable due to report_error exiting
                free_value_contents(interpreter->current_exception);
                interpreter->current_exception.type = VAL_STRING;
                interpreter->current_exception.as.string_val = strdup("Can only index into arrays, strings, dictionaries, or tuples.");
                if (interpreter->error_token) free_token(interpreter->error_token);
                interpreter->error_token = token_deep_copy(bracket_token);
                free_token(bracket_token);
                return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
            }
            interpreter_eat(interpreter, TOKEN_RBRACKET);

            // Free the index value now that it has been used for this operation.
            if (index_is_fresh) free_value_contents(index_val);

            free_token(bracket_token);

        } else if (interpreter->current_token->type == TOKEN_DOT) {
            Token* dot_token = token_deep_copy(interpreter->current_token); // Deep copy for error reporting
            interpreter_eat(interpreter, TOKEN_DOT);
            
            TokenType actual_token_type_after_dot = interpreter->current_token->type;
            char* attr_name_value = interpreter->current_token->value; // Get value before potential eat

            // Allow TOKEN_ID or specific keywords that can act as attributes
            if (actual_token_type_after_dot != TOKEN_ID &&
                actual_token_type_after_dot != TOKEN_BLUEPRINT &&
                (strcmp(attr_name_value, "name") != 0 || actual_token_type_after_dot != TOKEN_ID) // If it's "name", it must be TOKEN_ID
                /* Add other allowed keyword tokens here if necessary */
            ) {
                if(result_is_freshly_created) free_value_contents(result);
                report_error("Syntax", "Expected identifier or valid attribute keyword after '.' for attribute/method access.", dot_token);
                free_token(dot_token); // Free the copy if report_error didn't exit (though it does)
            }
            char* attr_name = strdup(attr_name_value);
            /* Token* attr_name_token = token_deep_copy(interpreter->current_token); // Marked as unused */
            interpreter_eat(interpreter, actual_token_type_after_dot); // Eat the token based on its actual type

            bool attribute_handled_by_special_case = false;

            if (strcmp(attr_name, "len") == 0) {
                if (result.type == VAL_ARRAY) {
                    next_derived_value.type = VAL_INT;
                    next_derived_value.as.integer = result.as.array_val->count;
                    next_derived_is_fresh = false;
                    attribute_handled_by_special_case = true;
                } else if (result.type == VAL_STRING) {
                    next_derived_value.type = VAL_INT;
                    next_derived_value.as.integer = (long)strlen(result.as.string_val);
                    next_derived_is_fresh = false;
                    attribute_handled_by_special_case = true;
                } else if (result.type == VAL_DICT) {
                    next_derived_value.type = VAL_INT;
                    next_derived_value.as.integer = result.as.dict_val->count;
                    next_derived_is_fresh = false;
                    attribute_handled_by_special_case = true;
                } else if (result.type == VAL_TUPLE) {
                    next_derived_value.type = VAL_INT;
                    next_derived_value.as.integer = result.as.tuple_val->count;
                    next_derived_is_fresh = false;
                    attribute_handled_by_special_case = true;
                } else {
                    // If not one of the above, let it fall through to standard attribute access
                    // which will likely fail if 'len' is not a defined field/method for VAL_OBJECT etc.
                    // or report an error for types that don't support attributes.
                    // This allows objects to have their own 'len' method/attribute.
                    // attribute_handled_by_special_case remains false.
                }
            }

            // Value derived_value_after_dot; // Replaced by next_derived_value
            // Only proceed with general attribute lookup if not handled by a special case like "len"
            if (!attribute_handled_by_special_case) {
                if (result.type == VAL_OBJECT) {
                Object* obj = result.as.object_val;

                if (strcmp(attr_name, "blueprint") == 0) {
                    // Special case: obj.blueprint returns the blueprint itself
                    next_derived_value.type = VAL_BLUEPRINT;
                    next_derived_value.as.blueprint_val = obj->blueprint;
                    next_derived_is_fresh = false; // Blueprint is shared
                    // Do not free 'result' (the object) here, as we are returning a part of it.
                    // The 'result' will be freed by the caller if it was a temporary.
                    // No need to set attribute_handled_by_special_case here as it's the end of VAL_OBJECT specific logic for "blueprint"
                } else {
                    Value* attr_val_ptr = symbol_table_get_local(obj->instance_attributes, attr_name); // Check instance attributes first
                    if (!attr_val_ptr) { // Not in instance, check class attributes/methods
                        Blueprint* current_bp = obj->blueprint;
                        while (current_bp) {
                            attr_val_ptr = symbol_table_get_local(current_bp->class_attributes_and_methods, attr_name);
                            if (attr_val_ptr) break;
                            current_bp = current_bp->parent_blueprint;
                        }
                    }

                    if (!attr_val_ptr) {
                        char err_msg[150];
                        snprintf(err_msg, sizeof(err_msg), "Object of blueprint '%s' has no attribute or method '%s'.", obj->blueprint->name, attr_name);
                        free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                        report_error("Runtime", err_msg, dot_token);
                        free_token(dot_token);
                    }

                    if (attr_val_ptr->type == VAL_FUNCTION) {
                        BoundMethod* bm = malloc(sizeof(BoundMethod));
                        if (!bm) {
                            free(attr_name);
                            free_token(dot_token);
                            if(result_is_freshly_created) free_value_contents(result);
                            report_error("System", "Failed to allocate memory for bound method for object attribute.", dot_token);
                        }
                        bm->ref_count = 1; // Initialize ref count
                        bm->type = FUNC_TYPE_ECHOC;
                        bm->func_ptr.echoc_function = attr_val_ptr->as.function_val;
                        bm->self_value.type = VAL_OBJECT; // Assign type
                        bm->self_value.as.object_val = obj;  // Assign object pointer
                        bm->self_is_owned_copy = result_is_freshly_created ? 1 : 0;
                        next_derived_value.type = VAL_BOUND_METHOD;
                        next_derived_value.as.bound_method_val = bm;
                        next_derived_is_fresh = true; // BoundMethod is a new container struct
                    } else { // Regular attribute
                        if (!result_is_freshly_created &&
                            (attr_val_ptr->type == VAL_OBJECT || attr_val_ptr->type == VAL_ARRAY || attr_val_ptr->type == VAL_DICT)) {
                            // If the base object 'result' is not fresh (e.g., it's 'self' or a variable),
                            // and the attribute is a container, get a shallow copy (view) of the attribute.
                            next_derived_value = *attr_val_ptr; // Shallow copy of Value struct
                            next_derived_is_fresh = false;      // This view is not a fresh container
                        } else {
                            // If base object 'result' IS fresh, or attribute is not a container, deep copy the attribute.
                            next_derived_value = value_deep_copy(*attr_val_ptr);
                            // Mark as fresh if it's a container type that value_deep_copy creates anew
                            if (next_derived_value.type == VAL_STRING || next_derived_value.type == VAL_ARRAY ||
                                next_derived_value.type == VAL_DICT || next_derived_value.type == VAL_TUPLE ||
                                next_derived_value.type == VAL_OBJECT || next_derived_value.type == VAL_FUNCTION ||
                                next_derived_value.type == VAL_COROUTINE || next_derived_value.type == VAL_GATHER_TASK) {
                                next_derived_is_fresh = true;
                            } else {
                                next_derived_is_fresh = false;
                            }
                        }
                    }
                    // free_value_contents(result) was here, but it's problematic if new_result is a bound method
                    // or if we are accessing obj.blueprint. The 'result' (object) might still be needed.
                    // Let the caller manage freeing the original 'result' if it was a temporary.
                }
            } else if (result.type == VAL_BLUEPRINT) { // Accessing class attribute/static method
                Blueprint* bp = result.as.blueprint_val;
                if (strcmp(attr_name, "name") == 0) { // Special case for blueprint.name
                    next_derived_value.type = VAL_STRING;
                    next_derived_value.as.string_val = strdup(bp->name);
                    if (!next_derived_value.as.string_val) {
                        free(attr_name);
                        report_error("System", "Failed to strdup blueprint name for .name access", dot_token); // dot_token will be freed by caller or error handler
                        free_token(dot_token);
                    }
                    // No need to set attribute_handled_by_special_case here as it's the end of VAL_BLUEPRINT specific logic for "name"
                    next_derived_is_fresh = true; // New string
                } else { // Regular class attribute/static method
                    Value* class_attr_ptr = NULL;
                    Blueprint* search_bp_hierarchy = bp; // Start with the current blueprint
                    while (search_bp_hierarchy) {
                        class_attr_ptr = symbol_table_get_local(search_bp_hierarchy->class_attributes_and_methods, attr_name);
                        if (class_attr_ptr) break; // Found
                        search_bp_hierarchy = search_bp_hierarchy->parent_blueprint; // Go to parent
                    }

                    if (!class_attr_ptr) {
                        char err_msg[150];
                        snprintf(err_msg, sizeof(err_msg), "Blueprint '%s' (and its parents) has no class attribute or static method '%s'.", bp->name, attr_name);
                        free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                        report_error("Runtime", err_msg, dot_token);
                        free_token(dot_token);
                    }
                    next_derived_value = value_deep_copy(*class_attr_ptr);
                    // Mark as fresh if it's a container type that value_deep_copy creates anew
                    if (next_derived_value.type == VAL_STRING || next_derived_value.type == VAL_ARRAY ||
                        next_derived_value.type == VAL_DICT || next_derived_value.type == VAL_TUPLE ||
                        next_derived_value.type == VAL_OBJECT || next_derived_value.type == VAL_FUNCTION ||
                        next_derived_value.type == VAL_COROUTINE || next_derived_value.type == VAL_GATHER_TASK) {
                        next_derived_is_fresh = true;
                    } else {
                        next_derived_is_fresh = false;
                    }
                }
            } else if (result.type == VAL_ARRAY) {
                if (strcmp(attr_name, "append") == 0) {
                    BoundMethod* bm = malloc(sizeof(BoundMethod));
                    if (!bm) {
                        free(attr_name); free_token(dot_token);
                        if(result_is_freshly_created) free_value_contents(result);
                        report_error("System", "Failed to allocate memory for array.append bound method.", dot_token);
                    }
                    bm->ref_count = 1; // Initialize ref count
                    bm->type = FUNC_TYPE_C_BUILTIN;
                    bm->func_ptr.c_builtin = builtin_append;
                    bm->self_value = result; // The array itself. If result was fresh, bm takes ownership.
                    bm->self_is_owned_copy = result_is_freshly_created;

                    next_derived_value.type = VAL_BOUND_METHOD;
                    next_derived_value.as.bound_method_val = bm;
                    next_derived_is_fresh = true; // The BoundMethod struct is new.
                } else {
                    char err_msg[150];
                    snprintf(err_msg, sizeof(err_msg), "Array has no attribute or method '%s'.", attr_name);
                    free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                    report_error("Runtime", err_msg, dot_token);
                    free_token(dot_token);
                }
            } else if (result.type == VAL_DICT) { // Handle dict.key access
                Dictionary* dict = result.as.dict_val;
                Value val_from_dict;
                // Get a view (shallow copy) to be consistent with dict["key"] access.
                if (dictionary_try_get(dict, attr_name, &val_from_dict, false)) {
                    next_derived_value = val_from_dict; // val_from_dict is a view.
                    next_derived_is_fresh = false;      // A view is not a fresh container.
                } else { // Key not found
                    char err_msg[150];
                    snprintf(err_msg, sizeof(err_msg), "Key '%s' not found in dictionary.", attr_name);
                    free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                    report_error("Runtime", err_msg, dot_token);
                    free_token(dot_token); // Free if report_error didn't exit
                }
            } else if (result.type == VAL_SUPER_PROXY) { // super.method_name
                Object* self_obj_for_super = interpreter->current_self_object;
                if (!self_obj_for_super || !self_obj_for_super->blueprint->parent_blueprint) {
                    free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                    report_error("Runtime", "'super' used incorrectly or in a class with no parent.", dot_token);
                    free_token(dot_token);
                }
                Value* parent_member_ptr = symbol_table_get_local(self_obj_for_super->blueprint->parent_blueprint->class_attributes_and_methods, attr_name);
                if (!parent_member_ptr || parent_member_ptr->type != VAL_FUNCTION) {
                    char err_msg[150];
                    if (!parent_member_ptr)
                        snprintf(err_msg, sizeof(err_msg), "Parent blueprint of '%s' does not have attribute or method '%s'.", self_obj_for_super->blueprint->name, attr_name);
                    else
                        snprintf(err_msg, sizeof(err_msg), "Attribute '%s' in parent blueprint of '%s' is not a method.", attr_name, self_obj_for_super->blueprint->name);

                    free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                    report_error("Runtime", err_msg, dot_token);
                    free_token(dot_token);
                }
                BoundMethod* bm = malloc(sizeof(BoundMethod));
                if (!bm) { /* error */ }
                bm->ref_count = 1; // Initialize ref count
                bm->type = FUNC_TYPE_ECHOC;
                bm->func_ptr.echoc_function = parent_member_ptr->as.function_val;
                bm->self_value.type = VAL_OBJECT; // Assign type
                bm->self_value.as.object_val = self_obj_for_super; // Call parent method with current 'self'
                bm->self_is_owned_copy = 0; // 'self' is borrowed from interpreter context
                next_derived_value.type = VAL_BOUND_METHOD;
                next_derived_value.as.bound_method_val = bm;
                next_derived_is_fresh = true; // BoundMethod struct is new
            } else {
                char err_msg[100];
                snprintf(err_msg, sizeof(err_msg), "Cannot access attribute '%s' on non-object/blueprint/super_proxy type (got type %d).", attr_name, result.type);
                free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                report_error("Runtime", err_msg, dot_token);
                free_token(dot_token);
            }
            } // End of if (!attribute_handled_by_special_case)
            free(attr_name);
            free_token(dot_token); // Free the copied dot_token after successful processing or if error didn't exit

            // If we created a new BoundMethod but there's an error later, ensure it gets freed
            if (next_derived_is_fresh && interpreter->exception_is_active) {
                free_value_contents(next_derived_value);
                next_derived_value = create_null_value();
            }
        }
        // --- START: Consolidated Memory Management for 'result' ---
        if (result_is_freshly_created) {
            bool should_free_old_result = true;

            // DON'T free old 'result' if the new 'next_derived_value' is a bound method
            // that has taken ownership of the old value (e.g., array.append).
            if (next_derived_value.type == VAL_BOUND_METHOD) {
                BoundMethod* bm = next_derived_value.as.bound_method_val;
                if (bm->self_is_owned_copy) {
                    // Check if the bound method's 'self' is indeed the old result.
                    if (bm->self_value.type == result.type) {
                        if ((result.type == VAL_OBJECT && result.as.object_val == bm->self_value.as.object_val) ||
                            (result.type == VAL_ARRAY && result.as.array_val == bm->self_value.as.array_val)) {
                            should_free_old_result = false;
                        }
                    }
                }
            }

            if (should_free_old_result) {
                free_value_contents(result);
            }
        }
        // --- END: Consolidated Memory Management ---

        // Update result for the next iteration of the postfix loop
        result = next_derived_value;
        result_is_freshly_created = next_derived_is_fresh;
    }
    // Final result of the postfix expression chain
    current_expr_res.value = result;
    current_expr_res.is_freshly_created_container = result_is_freshly_created;
    current_expr_res.is_standalone_primary_id = is_still_standalone_id;
    return current_expr_res;
}

ExprResult interpret_power_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_postfix_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return left_res;
    }
    Value left = left_res.value;

    if (interpreter->current_token->type == TOKEN_POWER) {
        // --- FIX: Deep copy the operator token before it's eaten ---
        Token* op_token_copy = token_deep_copy(interpreter->current_token);

        interpreter_eat(interpreter, TOKEN_POWER);
        ExprResult right_res = interpret_power_expr(interpreter); // Right-associative

        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            if (left_res.is_freshly_created_container) free_value_contents(left_res.value);
            if (right_res.is_freshly_created_container) free_value_contents(right_res.value);
            free_token(op_token_copy);
            return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
        }

        Value right = right_res.value;

        if (!((left.type == VAL_INT || left.type == VAL_FLOAT) &&
              (right.type == VAL_INT || right.type == VAL_FLOAT))) {
            if (left_res.is_freshly_created_container) free_value_contents(left_res.value); // Use left_res.value
            if (right_res.is_freshly_created_container) free_value_contents(right_res.value); // Use right_res.value
            // --- FIX: This call is now safe because op_token_copy is a valid copy ---
            report_error("Runtime", "Operands for power operation ('^') must be numbers.", op_token_copy);
            free_token(op_token_copy); // Free the copy before returning/exiting due to error (report_error exits)
            // report_error exits, so this is more for logical completeness if it didn't.
        }
        double left_val = (left.type == VAL_INT) ? left.as.integer : left.as.floating;
        double right_val = (right.type == VAL_INT) ? right.as.integer : right.as.floating;
        
        // Free original left and right if they were complex (e.g. strings that caused error)
        if (left_res.is_freshly_created_container) free_value_contents(left_res.value);
        if (right_res.is_freshly_created_container) free_value_contents(right_res.value);

        Value result_val;
        result_val.type = VAL_FLOAT; // Result of pow() is always a double
        result_val.as.floating = pow(left_val, right_val);

        ExprResult final_res;
        final_res.value = result_val;
        final_res.is_freshly_created_container = false; // Numbers are not containers in this context
        final_res.is_standalone_primary_id = false; // Result of an operation

        // --- FIX: Free the copied token ---
        free_token(op_token_copy);
        return final_res;
    }
    return left_res; // No power op, return original ExprResult
}

ExprResult interpret_unary_expr(Interpreter* interpreter) {
    if (interpreter->current_token->type == TOKEN_NOT) {
        // --- FIX: Deep copy the operator token before it's eaten ---
        Token* op_token_copy = token_deep_copy(interpreter->current_token);

        interpreter_eat(interpreter, TOKEN_NOT);
        ExprResult operand_res = interpret_unary_expr(interpreter);
        Value operand = operand_res.value;
        
        // The result of 'not' is always a boolean.
        ExprResult final_res;
        Value result;
        result.type = VAL_BOOL;
        result.as.bool_val = !value_is_truthy(operand); // Negate the truthiness

        // We need to free the operand if it was a temporary container.
        if (operand_res.is_freshly_created_container) {
            free_value_contents(operand);
        }

        final_res.value = result;
        final_res.is_freshly_created_container = false;
        final_res.is_standalone_primary_id = false; // Result of 'not'

        // --- FIX: Free the copied token ---
        free_token(op_token_copy);
        return final_res;
    } else if (interpreter->current_token->type == TOKEN_MINUS) {
        // --- FIX: Deep copy the operator token before it's eaten ---
        Token* op_token_copy = token_deep_copy(interpreter->current_token);

        interpreter_eat(interpreter, TOKEN_MINUS);
        ExprResult operand_res = interpret_unary_expr(interpreter);
        Value operand = operand_res.value; // operand_res.is_freshly_created_container is not directly used here

                                          // as we modify operand in place or error.
        ExprResult final_res;
        if (operand.type == VAL_INT) {
            operand.as.integer = -operand.as.integer;
        } else if (operand.type == VAL_FLOAT) {
            operand.as.floating = -operand.as.floating;
        } else {
            if(operand_res.is_freshly_created_container) free_value_contents(operand); // Free if it was fresh
	        // --- FIX: This call is now safe because op_token_copy is a valid copy ---
            report_error("Runtime", "Operand for unary minus must be a number.", op_token_copy); // Use op_token_copy from TOKEN_MINUS block scope
            free_token(op_token_copy); // Free before returning due to error (report_error exits)
        }

        final_res.value = operand; // operand is modified in place
        final_res.is_freshly_created_container = operand_res.is_freshly_created_container; // Propagate if it was already fresh
        final_res.is_standalone_primary_id = false; // Result of unary minus
        // --- FIX: Free the copied token ---
        free_token(op_token_copy);
        return final_res;
    }
    ExprResult res = interpret_power_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return res;
    }
    return res;
}

ExprResult interpret_multiplicative_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_unary_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return left_res;
    }
    Value left = left_res.value;
    bool current_res_is_fresh = left_res.is_freshly_created_container;
    bool is_standalone = left_res.is_standalone_primary_id;

    while (interpreter->current_token->type == TOKEN_MUL || interpreter->current_token->type == TOKEN_DIV ||
           interpreter->current_token->type == TOKEN_MOD) {

        TokenType op_type = interpreter->current_token->type;

        // --- FIX: Deep copy the operator token before it's eaten ---
        Token* op_token_copy = token_deep_copy(interpreter->current_token);

        interpreter_eat(interpreter, op_type);
        ExprResult right_res = interpret_unary_expr(interpreter);

        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            if (current_res_is_fresh) free_value_contents(left); // Free the left-side value if it was a temporary.
            if (right_res.is_freshly_created_container) free_value_contents(right_res.value);
            free_token(op_token_copy);
            // Propagate the error/yield result.
            return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
        }

        Value right = right_res.value;
        bool right_is_fresh = right_res.is_freshly_created_container;
        
        Value result_val; // Use a new value for the result
        bool new_op_res_is_fresh = false;
        is_standalone = false; // An operation is being performed

        if (op_type == TOKEN_MUL) {
            if ((left.type == VAL_INT || left.type == VAL_FLOAT) && (right.type == VAL_INT || right.type == VAL_FLOAT)) {
                double left_val = (left.type == VAL_INT) ? left.as.integer : left.as.floating;
                double right_val = (right.type == VAL_INT) ? right.as.integer : right.as.floating;
                result_val.type = (left.type == VAL_FLOAT || right.type == VAL_FLOAT) ? VAL_FLOAT : VAL_INT;
                if (result_val.type == VAL_FLOAT) result_val.as.floating = left_val * right_val;
                else result_val.as.integer = (long)(left_val * right_val);
                new_op_res_is_fresh = false; // Numeric result
            } else if (left.type == VAL_STRING && right.type == VAL_INT) {
                long times = right.as.integer;
                if (times < 0) {
                    if(current_res_is_fresh) free_value_contents(left);
                    // right is VAL_INT, no complex contents to free.
                    report_error("Runtime", "Cannot repeat string a negative number of times.", op_token_copy);
                    free_token(op_token_copy); // Free before returning due to error (report_error exits)
                }
                size_t old_len = strlen(left.as.string_val);
                char* new_str = malloc(old_len * times + 1);
                if (!new_str) report_error("System", "Memory allocation failed for string repetition", op_token_copy);
                new_str[0] = '\0';
                for (long i = 0; i < times; i++) strcat(new_str, left.as.string_val);
                result_val.type = VAL_STRING;
                result_val.as.string_val = new_str;
                new_op_res_is_fresh = true; // New string
            } else if (left.type == VAL_INT && right.type == VAL_STRING) {
                long times = left.as.integer;
                if (times < 0) {
                    // left is VAL_INT, no complex contents.
                    if(right_is_fresh) free_value_contents(right);
                    report_error("Runtime", "Cannot repeat string a negative number of times.", op_token_copy);
                    free_token(op_token_copy); // Free before returning due to error (report_error exits)
                }
                size_t str_len = strlen(right.as.string_val);
                char* new_str = malloc(str_len * times + 1);
                if (!new_str) report_error("System", "Memory allocation failed for string repetition", op_token_copy);
                new_str[0] = '\0';
                for (long i = 0; i < times; i++) strcat(new_str, right.as.string_val);
                result_val.type = VAL_STRING;
                result_val.as.string_val = new_str;
                new_op_res_is_fresh = true; // New string
            } else {
                if(current_res_is_fresh) free_value_contents(left);
                if(right_is_fresh) free_value_contents(right);
                report_error("Runtime", "Unsupported operand types for '*' operator.", op_token_copy);
                free_token(op_token_copy); // Free before returning due to error (report_error exits)
            }
        } else if (op_type == TOKEN_MOD) {
            // Modulo logic should be strict: only integers are supported.
            if (left.type != VAL_INT || right.type != VAL_INT) {
                if (current_res_is_fresh) free_value_contents(left);
                if (right_is_fresh) free_value_contents(right);
                report_error("Runtime", "Operands for modulo ('%') must be integers.", op_token_copy);
                // report_error exits, so no need to return a value.
            }

            if (right.as.integer == 0) {
                // No need to free left/right here as they are integers and have no allocated content.
                report_error("Runtime", "Division by zero in modulo operation.", op_token_copy);
            }

            result_val.type = VAL_INT;
            result_val.as.integer = left.as.integer % right.as.integer;
            new_op_res_is_fresh = false; // Numeric result is not a "fresh container".
        } else { // TOKEN_DIV
            if (!((left.type == VAL_INT || left.type == VAL_FLOAT) && (right.type == VAL_INT || right.type == VAL_FLOAT))) {
                if(current_res_is_fresh) free_value_contents(left);
                else if (left.type != VAL_INT && left.type != VAL_FLOAT && left.type != VAL_NULL) free_value_contents(left); // Added VAL_NULL check
                if(right_is_fresh) free_value_contents(right);
                else if (right.type != VAL_INT && right.type != VAL_FLOAT && right.type != VAL_NULL) free_value_contents(right); // Added VAL_NULL check
                report_error("Runtime", "Operands for '/' must both be numbers.", op_token_copy);
                free_token(op_token_copy); // Free before returning due to error (report_error exits)
            }
            double left_val = (left.type == VAL_INT) ? left.as.integer : left.as.floating;
            double right_val = (right.type == VAL_INT) ? right.as.integer : right.as.floating;
            if (right_val == 0) {
                // Operands are numbers, no complex contents to free.
                report_error("Runtime", "Division by zero", op_token_copy);
                free_token(op_token_copy); // Free before returning due to error (report_error exits)
            }
            result_val.type = VAL_FLOAT;
            result_val.as.floating = left_val / right_val;
            new_op_res_is_fresh = false; // Numeric result
            
        }
        // Free operands only if they were fresh temporaries from this expression chain
        if (current_res_is_fresh) free_value_contents(left);
        if (right_is_fresh) free_value_contents(right);

        left = result_val;
        current_res_is_fresh = new_op_res_is_fresh;

        // --- FIX: Free the copied token at the end of the loop iteration ---
        free_token(op_token_copy);
    }

    // After the loop, if an exception is active (e.g., from the TOKEN_MOD block)
    if (interpreter->exception_is_active) {
        if (current_res_is_fresh) free_value_contents(left); // Free the dummy 'left'
        ExprResult error_res;
        error_res.value = create_null_value(); // Return a dummy error result
        error_res.is_freshly_created_container = false;
        error_res.is_standalone_primary_id = false;
        return error_res;
    }
    
    ExprResult final_res;
    final_res.value = left;
    final_res.is_freshly_created_container = current_res_is_fresh;
    final_res.is_standalone_primary_id = is_standalone;
    return final_res;
}

ExprResult interpret_additive_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_multiplicative_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return left_res;
    }

    // Value left = left_res.value; // This will be used later in the correct function body

    // Actual interpret_additive_expr logic continues here
    Value left = left_res.value;
    bool current_res_is_fresh = left_res.is_freshly_created_container;
    bool is_standalone = left_res.is_standalone_primary_id;

    while (interpreter->current_token->type == TOKEN_PLUS || interpreter->current_token->type == TOKEN_MINUS) {
        TokenType op_type = interpreter->current_token->type;

        // --- FIX: Deep copy the operator token before it's eaten ---
        Token* op_token_copy = token_deep_copy(interpreter->current_token);

        interpreter_eat(interpreter, op_type);
        ExprResult right_res = interpret_multiplicative_expr(interpreter);

        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            if (current_res_is_fresh) free_value_contents(left); // Free the left-side value if it was a temporary.
            if (right_res.is_freshly_created_container) free_value_contents(right_res.value);
            free_token(op_token_copy);
            return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
        }

        Value right = right_res.value;
        bool right_is_fresh = right_res.is_freshly_created_container;
        
        Value result_val;
        bool new_op_res_is_fresh = false;
        is_standalone = false; // An operation is being performed

        if (op_type == TOKEN_PLUS) {
            if ((left.type == VAL_INT || left.type == VAL_FLOAT) && (right.type == VAL_INT || right.type == VAL_FLOAT)) {
                if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                    double left_val = (left.type == VAL_INT) ? left.as.integer : left.as.floating;
                    double right_val = (right.type == VAL_INT) ? right.as.integer : right.as.floating;
                    result_val.type = VAL_FLOAT;
                    result_val.as.floating = left_val + right_val;
                } else { // Both VAL_INT
                    result_val.type = VAL_INT;
                    result_val.as.integer = left.as.integer + right.as.integer;
                }
                new_op_res_is_fresh = false; // Numeric result
            } else if (left.type == VAL_STRING || right.type == VAL_STRING) {
                char s1_buf[256], s2_buf[256];
                const char *s1_ptr, *s2_ptr;

                if (left.type == VAL_STRING) s1_ptr = left.as.string_val;
                else if (left.type == VAL_INT) { snprintf(s1_buf, sizeof(s1_buf), "%ld", left.as.integer); s1_ptr = s1_buf; }
                else { snprintf(s1_buf, sizeof(s1_buf), "%g", left.as.floating); s1_ptr = s1_buf; }

                if (right.type == VAL_STRING) s2_ptr = right.as.string_val;
                else if (right.type == VAL_INT) { snprintf(s2_buf, sizeof(s2_buf), "%ld", right.as.integer); s2_ptr = s2_buf; }
                else { snprintf(s2_buf, sizeof(s2_buf), "%g", right.as.floating); s2_ptr = s2_buf; }

                DynamicString ds_concat;
                ds_init(&ds_concat, strlen(s1_ptr) + strlen(s2_ptr) + 1); // Initial capacity
                ds_append_str(&ds_concat, s1_ptr);
                ds_append_str(&ds_concat, s2_ptr);

                result_val.type = VAL_STRING;
                result_val.as.string_val = ds_finalize(&ds_concat);
                new_op_res_is_fresh = true; // New string
            } else {
                if (left.type == VAL_OBJECT) { // op_add attempt
                    Object* left_obj = left.as.object_val;
                    Value* op_add_method_val = NULL;
                    Blueprint* current_bp = left_obj->blueprint;
                    while(current_bp) {
                        op_add_method_val = symbol_table_get_local(current_bp->class_attributes_and_methods, "op_add");
                        if (op_add_method_val && op_add_method_val->type == VAL_FUNCTION) break;
                        op_add_method_val = NULL;
                        current_bp = current_bp->parent_blueprint;
                    }

                    if (op_add_method_val) {
                        ParsedArgument parsed_args[1];
                        parsed_args[0].name = NULL; // It's a positional argument
                        parsed_args[0].value = value_deep_copy(right);
                        parsed_args[0].is_fresh = true;

                        result_val = execute_echoc_function(interpreter, op_add_method_val->as.function_val, left_obj, parsed_args, 1, op_token_copy);
                        // execute_echoc_function handles freeing call_args elements.
                        if (result_val.type >= VAL_STRING && result_val.type <= VAL_GATHER_TASK && result_val.type != VAL_BLUEPRINT && result_val.type != VAL_SUPER_PROXY) {
                            new_op_res_is_fresh = true;
                        }
                    } else {
                        // op_add not found on object
                        if(current_res_is_fresh) free_value_contents(left);
                        if(right_is_fresh) free_value_contents(right);
                        report_error("Runtime", "Object does not support '+' operator (missing op_add method).", op_token_copy);
                        // free_token(op_token_copy); // report_error exits, so this is not strictly needed but good for consistency if it didn't
                    }
                } else { // Fallback if not VAL_OBJECT (and not numeric/string)
                    if(current_res_is_fresh) free_value_contents(left);
                    if(right_is_fresh) free_value_contents(right);
                    report_error("Runtime", "Unsupported operand types for '+' operator.", op_token_copy);
                    // free_token(op_token_copy); // report_error exits
                }
            }
        } else { // TOKEN_MINUS
           if (!((left.type == VAL_INT || left.type == VAL_FLOAT) && (right.type == VAL_INT || right.type == VAL_FLOAT))) {
                if(current_res_is_fresh) free_value_contents(left);
                if(right_is_fresh) free_value_contents(right);
                report_error("Runtime", "Operands for '-' must both be numbers.", op_token_copy);
                // free_token(op_token_copy); // report_error exits
            }
            // If types are correct, proceed with calculation
            double left_val = (left.type == VAL_INT) ? left.as.integer : left.as.floating;
            double right_val = (right.type == VAL_INT) ? right.as.integer : right.as.floating;
            
            if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                 result_val.type = VAL_FLOAT;
                 result_val.as.floating = left_val - right_val;
            } else { // Both VAL_INT
                 result_val.type = VAL_INT;
                 result_val.as.integer = (long)(left_val - right_val);
            }
            new_op_res_is_fresh = false; // Numeric result
        }
        // Free operands only if they were fresh temporaries from this expression chain
        if (current_res_is_fresh) free_value_contents(left);
        if (right_is_fresh) free_value_contents(right);

        left = result_val;
        current_res_is_fresh = new_op_res_is_fresh;

        // --- FIX: Free the copied token at the end of the loop iteration ---
        free_token(op_token_copy);
    }

    ExprResult final_res;
    final_res.value = left;
    final_res.is_freshly_created_container = current_res_is_fresh;
    final_res.is_standalone_primary_id = is_standalone;
   return final_res;
}

// Implementation of execute_echoc_function
Value execute_echoc_function(Interpreter* interpreter, Function* func_to_call, Object* self_obj, ParsedArgument* parsed_args, int arg_count, Token* call_site_token) {
    // START: Short-circuit check
    if (interpreter->prevent_side_effects) {
        // In a short-circuited expression, we don't execute the function.
        // The arguments have already been "dry-run" parsed by the caller to advance the lexer.
        // We just need to clean them up and return a dummy value.
        for (int i = 0; i < arg_count; ++i) {
            if (parsed_args[i].name) free(parsed_args[i].name);
            // The value is a dummy from a dry-run, no complex contents to free.
        }
        return create_null_value();
    }
    // END: Short-circuit check

    // --- START: C Function Dispatch ---
    if (func_to_call->c_impl) {
        Value result;
        // Special handling for `weaver.gather` to allow a named argument.
        if (strcmp(func_to_call->name, "gather") == 0) {
            Value tasks_array_val = create_null_value();
            int positional_arg_count = 0;
            interpreter->gather_last_return_exceptions_flag = false; // Reset before parsing

            for (int i = 0; i < arg_count; i++) {
                if (parsed_args[i].name) { // Named argument
                    if (strcmp(parsed_args[i].name, "return_exceptions") == 0) {
                        if (parsed_args[i].value.type != VAL_BOOL) report_error("Runtime", "'return_exceptions' argument for gather() must be a boolean.", call_site_token);
                        interpreter->gather_last_return_exceptions_flag = parsed_args[i].value.as.bool_val;
                    } else {
                        char err_msg[250];
                        snprintf(err_msg, sizeof(err_msg), "gather() got an unexpected keyword argument '%s'", parsed_args[i].name);
                        report_error("Runtime", err_msg, call_site_token);
                    }
                } else { // Positional argument
                    if (positional_arg_count == 0) tasks_array_val = parsed_args[i].value;
                    positional_arg_count++;
                }
            }
            if (positional_arg_count != 1) report_error("Runtime", "gather() expects exactly 1 positional argument (the array of tasks).", call_site_token);

            Value simple_args[] = { tasks_array_val };
            result = func_to_call->c_impl(interpreter, simple_args, 1, call_site_token);
        } else {
            // Default behavior for other C functions: disallow named args.
            Value simple_args[arg_count];
            for (int i = 0; i < arg_count; ++i) {
                if (parsed_args[i].name) {
                    char err_msg[250];
                    snprintf(err_msg, sizeof(err_msg), "Built-in module function '%s' does not support named arguments.", func_to_call->name);
                    report_error("Runtime", err_msg, call_site_token);
                }
                simple_args[i] = parsed_args[i].value;
            }
            result = func_to_call->c_impl(interpreter, simple_args, arg_count, call_site_token);
        }

        // After the C function is called, we must clean up the original parsed arguments.
        // The C function does not take ownership of the argument values' contents, so the
        // caller (`execute_echoc_function`) is responsible for freeing any temporary containers.
        for (int i = 0; i < arg_count; ++i) {
            if (parsed_args[i].name) free(parsed_args[i].name);
            if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
        }

        // Now that cleanup is done, return the result from the C function.
        return result;
    }
    // --- END: C Function Dispatch ---

    int param_count = func_to_call->param_count;
    int self_offset = self_obj ? 1 : 0;
    int non_self_param_count = param_count - self_offset;

    // --- REFACTORED ARGUMENT AND SCOPE HANDLING ---

    // 1. Setup the new scope for the function call first.
    Scope* old_scope = interpreter->current_scope;
    Object* old_self_obj_ctx = interpreter->current_self_object;

    interpreter->current_scope = func_to_call->definition_scope;
    enter_scope(interpreter); // This new scope is now interpreter->current_scope

    if (self_obj) {
        interpreter->current_self_object = self_obj;
        // Manually insert 'self' to ensure it's a direct reference, not a deep copy.
        SymbolNode* self_node = (SymbolNode*)malloc(sizeof(SymbolNode));
        if (!self_node) report_error("System", "Failed to allocate memory for 'self' symbol node in method scope", call_site_token);
        
        self_node->name = strdup("self");
        if (!self_node->name) { free(self_node); report_error("System", "Failed to strdup 'self' name for method scope", call_site_token); }
        
        self_node->value.type = VAL_OBJECT;
        self_node->value.as.object_val = self_obj; // Direct pointer assignment
        
        self_node->next = interpreter->current_scope->symbols;
        interpreter->current_scope->symbols = self_node;
        DEBUG_PRINTF("Manually set 'self' in method scope (execute_echoc_function) %p, pointing to Object %p", (void*)interpreter->current_scope, (void*)self_obj);
    } else {
        interpreter->current_self_object = NULL; // No 'self' for regular functions
    }

    bool arg_was_provided[param_count];
    for (int i = 0; i < param_count; ++i) {
        arg_was_provided[i] = false;
    }

    #define CLEANUP_AND_REPORT(msg, token) do { \
        for (int i = 0; i < arg_count; ++i) { \
            if (parsed_args[i].name) free(parsed_args[i].name); \
            if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value); \
        } \
        exit_scope(interpreter); \
        interpreter->current_scope = old_scope; \
        interpreter->current_self_object = old_self_obj_ctx; \
        report_error("Runtime", msg, token); \
    } while (0)

    // 2. Process positional arguments
    int positional_arg_count = 0;
    for (int i = 0; i < arg_count; ++i) {
        if (parsed_args[i].name == NULL) {
            positional_arg_count++;
        }
    }

    if (positional_arg_count > non_self_param_count) {
        char err_msg[250];
        snprintf(err_msg, sizeof(err_msg), "%s() takes %d positional argument(s) but %d were given.",
                 func_to_call->name, non_self_param_count, positional_arg_count);
        CLEANUP_AND_REPORT(err_msg, call_site_token);
    }

    int current_pos_arg = 0;
    for (int i = 0; i < arg_count; ++i) {
        if (parsed_args[i].name == NULL) {
            int param_idx = current_pos_arg + self_offset;
            // Directly set the argument in the new scope. symbol_table_set handles the deep copy.
            symbol_table_set(interpreter->current_scope, func_to_call->params[param_idx].name, parsed_args[i].value);
            arg_was_provided[param_idx] = true;
            current_pos_arg++;
        }
    }

    // 3. Process named arguments
    for (int i = 0; i < arg_count; ++i) {
        if (parsed_args[i].name != NULL) {
            bool param_found = false;
            for (int j = self_offset; j < param_count; ++j) {
                if (strcmp(func_to_call->params[j].name, parsed_args[i].name) == 0) {
                    if (arg_was_provided[j]) {
                        char err_msg[250];
                        snprintf(err_msg, sizeof(err_msg), "%s() got multiple values for argument '%s'.",
                                 func_to_call->name, parsed_args[i].name);
                        CLEANUP_AND_REPORT(err_msg, call_site_token);
                    }
                    symbol_table_set(interpreter->current_scope, func_to_call->params[j].name, parsed_args[i].value);
                    arg_was_provided[j] = true;
                    param_found = true;
                    break;
                }
            }
            if (!param_found) {
                char err_msg[250];
                snprintf(err_msg, sizeof(err_msg), "%s() got an unexpected keyword argument '%s'.",
                         func_to_call->name, parsed_args[i].name);
                CLEANUP_AND_REPORT(err_msg, call_site_token);
            }
        }
    }

    // 4. Fill in defaults and check for missing required args
    for (int i = self_offset; i < param_count; ++i) {
        if (!arg_was_provided[i]) {
            if (func_to_call->params[i].default_value) {
                symbol_table_set(interpreter->current_scope, func_to_call->params[i].name, *(func_to_call->params[i].default_value));
            } else {
                char err_msg[250];
                snprintf(err_msg, sizeof(err_msg), "%s() missing 1 required positional argument: '%s'.",
                         func_to_call->name, func_to_call->params[i].name);
                CLEANUP_AND_REPORT(err_msg, call_site_token);
            }
        }
    }
    #undef CLEANUP_AND_REPORT

    // 5. Cleanup parsed_args. This is now safe as all necessary values have been copied to the new scope.
    for (int i = 0; i < arg_count; ++i) {
        if (parsed_args[i].name) free(parsed_args[i].name);
        if (parsed_args[i].is_fresh) free_value_contents(parsed_args[i].value);
    }

    // --- END REFACTOR ---

    LexerState old_lexer_state = get_lexer_state(interpreter->lexer);
    Token* old_current_token = token_deep_copy(interpreter->current_token);


    // Prepare and set lexer state for function body execution
    LexerState effective_body_start_state = func_to_call->body_start_state;
    effective_body_start_state.text = func_to_call->source_text_owned_copy;
    effective_body_start_state.text_length = func_to_call->source_text_length;
    set_lexer_state(interpreter->lexer, effective_body_start_state);

    free_token(interpreter->current_token);
    interpreter->current_token = get_next_token(interpreter->lexer);

    interpreter->function_nesting_level++;
    interpreter->return_flag = 0;
    free_value_contents(interpreter->current_function_return_value);
    interpreter->current_function_return_value = create_null_value();

    // Loop as long as the current token is part of the function body (i.e., indented more than the function definition)
    while (interpreter->current_token->col > func_to_call->definition_col &&
           // Also stop if we hit EOF, which implies an unclosed function.
           interpreter->current_token->type != TOKEN_EOF) {
        interpret_statement(interpreter);
        if (interpreter->exception_is_active && func_to_call->body_end_token_original_line == -1) {
            // If exception in a function with no pre-scanned end (e.g. op_str), ensure we break
        }
         if (interpreter->return_flag || interpreter->break_flag || interpreter->continue_flag || interpreter->exception_is_active) {
             break;
         }
    }
 
    if (interpreter->exception_is_active) {
        if (interpreter->error_token) free_token(interpreter->error_token);
        interpreter->error_token = token_deep_copy(call_site_token);
    }

    interpreter->function_nesting_level--;

    Value result = value_deep_copy(interpreter->current_function_return_value);
    
    free_value_contents(interpreter->current_function_return_value);
    interpreter->current_function_return_value = create_null_value(); // Reset for safety

    set_lexer_state(interpreter->lexer, old_lexer_state);
    free_token(interpreter->current_token);
    interpreter->current_token = old_current_token;

    exit_scope(interpreter);
    interpreter->current_scope = old_scope;
    interpreter->current_self_object = old_self_obj_ctx;
    interpreter->return_flag = 0;
    return result;
}

ExprResult interpret_comparison_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_additive_expr(interpreter);
    if (interpreter->exception_is_active) return left_res; // Propagate error result up

    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return left_res;
    }

    while (interpreter->current_token->type == TOKEN_LT || interpreter->current_token->type == TOKEN_GT ||
           interpreter->current_token->type == TOKEN_LTE || interpreter->current_token->type == TOKEN_GTE) {

        // --- FIX: Deep copy the operator token before it's eaten ---
        Token* op_token_copy = token_deep_copy(interpreter->current_token);
        TokenType op_type = interpreter->current_token->type;

        interpreter_eat(interpreter, op_type);
        ExprResult right_res = interpret_additive_expr(interpreter);

        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            if (left_res.is_freshly_created_container) free_value_contents(left_res.value); // Free the left-side value if it was a temporary.
            if (right_res.is_freshly_created_container) free_value_contents(right_res.value);
            free_token(op_token_copy);
            return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
        }

        Value right = right_res.value;

        if (!((left_res.value.type == VAL_INT || left_res.value.type == VAL_FLOAT) &&
              (right.type == VAL_INT || right.type == VAL_FLOAT))) {
            // Free operands if they were fresh containers before reporting error
            if (left_res.is_freshly_created_container && left_res.value.type != VAL_NULL) free_value_contents(left_res.value);
            if (right_res.is_freshly_created_container && right_res.value.type != VAL_NULL) free_value_contents(right_res.value);
            char err_msg[100];
            // --- FIX: Use op_token_copy for error reporting ---
            snprintf(err_msg, sizeof(err_msg), "Operands for comparison operator '%s' must be numbers.", op_token_copy->value);
            report_error("Runtime", err_msg, op_token_copy);
        }
        
        double left_val = (left_res.value.type == VAL_INT) ? (double)left_res.value.as.integer : left_res.value.as.floating;
        double right_val = (right.type == VAL_INT) ? (double)right.as.integer : right.as.floating;

        Value result_val;
        result_val.type = VAL_BOOL;
        if (op_type == TOKEN_LT) result_val.as.bool_val = left_val < right_val;
        else if (op_type == TOKEN_GT) result_val.as.bool_val = left_val > right_val;
        else if (op_type == TOKEN_LTE) result_val.as.bool_val = left_val <= right_val;
        else if (op_type == TOKEN_GTE) result_val.as.bool_val = left_val >= right_val;
        
        // Free the original values if they were fresh containers, as we now have a boolean result.
        if (left_res.is_freshly_created_container) free_value_contents(left_res.value);
        if (right_res.is_freshly_created_container) free_value_contents(right_res.value);

        left_res.value = result_val; // Update left_res with the new boolean result
        left_res.is_freshly_created_container = false; // Boolean is not a fresh container
        left_res.is_standalone_primary_id = false; // Result of comparison

        // --- FIX: Free the copied token ---
        free_token(op_token_copy);
    }

    // If the loop was entered, left_res was updated.
    // If not, original left_res is returned, preserving its freshness and standalone status.
    return left_res;
}

ExprResult interpret_identity_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_comparison_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return left_res;
    }

    while (interpreter->current_token->type == TOKEN_IS) {
        Token* op_token_copy = token_deep_copy(interpreter->current_token);
        interpreter_eat(interpreter, TOKEN_IS);

        bool is_not = false;
        if (interpreter->current_token->type == TOKEN_NOT) {
            is_not = true;
            interpreter_eat(interpreter, TOKEN_NOT);
        }

        ExprResult right_res = interpret_comparison_expr(interpreter);

        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            if (left_res.is_freshly_created_container) free_value_contents(left_res.value);
            if (right_res.is_freshly_created_container) free_value_contents(right_res.value);
            free_token(op_token_copy);
            return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
        }

        Value result_val;
        result_val.type = VAL_BOOL;

        bool are_identical = values_are_identical(left_res.value, right_res.value);

        result_val.as.bool_val = is_not ? !are_identical : are_identical;

        if(left_res.is_freshly_created_container) free_value_contents(left_res.value);
        if(right_res.is_freshly_created_container) free_value_contents(right_res.value);

        left_res.value = result_val;
        left_res.is_freshly_created_container = false;
        left_res.is_standalone_primary_id = false;

        free_token(op_token_copy);
    }

    return left_res;
}

ExprResult interpret_equality_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_identity_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return left_res;
    }

    // Value left = left_res.value; // Operate on left_res directly

    while (interpreter->current_token->type == TOKEN_EQ || interpreter->current_token->type == TOKEN_NEQ) {

        // --- FIX: Deep copy the operator token before it's eaten ---
        Token* op_token_copy = token_deep_copy(interpreter->current_token);
        TokenType op_type = interpreter->current_token->type;

        interpreter_eat(interpreter, op_type);
        ExprResult right_res = interpret_identity_expr(interpreter); // This will be the token for error reporting if types mismatch badly

        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            if (left_res.is_freshly_created_container) free_value_contents(left_res.value); // Free the left-side value if it was a temporary.
            if (right_res.is_freshly_created_container) {
                free_value_contents(right_res.value);
            }
            free_token(op_token_copy);
            return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
        }

        // Value right = right_res.value;

        Value result_val;
        result_val.type = VAL_BOOL;

        int are_equal = 0; // Assume not equal by default

        // Use the new helper function for deep equality.
        // Pass the token of the equality operator (or the right operand's start) for error context if needed by values_are_deep_equal.
        are_equal = values_are_deep_equal(interpreter, left_res.value, right_res.value, op_token_copy);

        result_val.as.bool_val = (op_type == TOKEN_EQ) ? are_equal : !are_equal;
        
        // Free original left_res.value and right_res.value if they were fresh containers
        if(left_res.is_freshly_created_container) free_value_contents(left_res.value);
        if(right_res.is_freshly_created_container) free_value_contents(right_res.value);
        
        left_res.value = result_val; // Update left_res
        left_res.is_freshly_created_container = false; // Boolean is not a fresh container
        left_res.is_standalone_primary_id = false; // Result of equality op

        // --- FIX: Free the copied token ---
        free_token(op_token_copy);
    }

    // If loop was entered, left_res was updated.
    // If not, original left_res is returned.
    return left_res;
}
ExprResult interpret_logical_and_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_equality_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return left_res;
    }

    while (interpreter->current_token->type == TOKEN_AND) {

        Token* op_token_copy = token_deep_copy(interpreter->current_token);
        interpreter_eat(interpreter, TOKEN_AND);
        
        if (!value_is_truthy(left_res.value)) {
            // Short-circuit: LHS is falsy, so the result is the LHS.
            // We must skip the RHS expression without evaluating its side effects.
            interpreter->prevent_side_effects = true;
            ExprResult right_dummy_res = interpret_equality_expr(interpreter); // This will just skip tokens
            interpreter->prevent_side_effects = false;

            // The dummy result should not have any allocated content, but we check just in case.
            if (right_dummy_res.is_freshly_created_container) free_value_contents(right_dummy_res.value);

            // The result of the 'and' operation is the original falsy LHS value.
            // We just continue the loop, and since left_res is still falsy, we'll keep short-circuiting.
        } else {
            // LHS is truthy. The result of the expression so far is the RHS.
            // We must evaluate the RHS normally.
            if (left_res.is_freshly_created_container) free_value_contents(left_res.value); // Free the old truthy LHS value
            ExprResult right_res = interpret_equality_expr(interpreter);
            if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
                // RHS yielded/errored. The LHS was already freed. Just propagate the dummy result.
                free_token(op_token_copy);
                return right_res;
            }
            left_res = right_res; // The new "left" for the next iteration is the result of the RHS.

        }
        free_token(op_token_copy);
    }

    return left_res;
}

ExprResult interpret_logical_or_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_logical_and_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return left_res;
    }

    while (interpreter->current_token->type == TOKEN_OR) {

        Token* op_token_copy = token_deep_copy(interpreter->current_token);
        interpreter_eat(interpreter, TOKEN_OR);

        if (value_is_truthy(left_res.value)) {
            // Short-circuit: LHS is truthy, so the result is the LHS.
            // We must skip the RHS expression without evaluating its side effects.
            interpreter->prevent_side_effects = true;
            ExprResult right_dummy_res = interpret_logical_and_expr(interpreter); // This will just skip tokens
            interpreter->prevent_side_effects = false;

            // The dummy result should not have any allocated content, but we check just in case.
            if (right_dummy_res.is_freshly_created_container) free_value_contents(right_dummy_res.value);

            // The result of the 'or' operation is the original truthy LHS value.
            // We just continue the loop, and since left_res is still truthy, we'll keep short-circuiting.
        } else {
            // LHS is falsy. The result of the expression so far is the RHS.
            // We must evaluate the RHS normally.
            if (left_res.is_freshly_created_container) free_value_contents(left_res.value); // Free the old falsy LHS value
            ExprResult right_res = interpret_logical_and_expr(interpreter);
            if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
                // RHS yielded/errored. The LHS was already freed. Just propagate the dummy result.
                free_token(op_token_copy);
                return right_res;
            }

            left_res = right_res; // The new "left" for the next iteration is the result of the RHS.
        }
        free_token(op_token_copy);
    }

    return left_res;
}


ExprResult interpret_conditional_expr(Interpreter* interpreter) {
    // New syntax: <true_expr> if <condition> else <false_expr>
    // This has the lowest precedence.

    ExprResult true_expr_res = interpret_await_expr(interpreter);
    if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
        return true_expr_res;
    }

    if (interpreter->current_token->type != TOKEN_IF) {
        // Not a conditional expression, just return the result of the higher-precedence expression.
        return true_expr_res;
    }

    // It's a conditional expression.
    interpreter_eat(interpreter, TOKEN_IF);

    ExprResult cond_res = interpret_await_expr(interpreter); // Parse the condition
    if (interpreter->exception_is_active) {
        if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);
        if (true_expr_res.is_freshly_created_container) free_value_contents(true_expr_res.value);
        return cond_res; // Propagate error
    }

    interpreter_eat(interpreter, TOKEN_ELSE);

    ExprResult false_expr_res = interpret_await_expr(interpreter); // Parse the 'false' part
    if (interpreter->exception_is_active) {
        if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);
        if (true_expr_res.is_freshly_created_container) free_value_contents(true_expr_res.value);
        if (false_expr_res.is_freshly_created_container) free_value_contents(false_expr_res.value);
        return false_expr_res; // Propagate error
    }

    // Now evaluate and return the correct branch
    bool condition_is_truthy = value_is_truthy(cond_res.value);
    if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);

    if (condition_is_truthy) {
        if (false_expr_res.is_freshly_created_container) free_value_contents(false_expr_res.value);
        return true_expr_res;
    } else {
        if (true_expr_res.is_freshly_created_container) free_value_contents(true_expr_res.value);
        return false_expr_res;
    }
    return cond_res; // No ternary, return result of the condition expression
}

ExprResult interpret_await_expr(Interpreter* interpreter) {
    Coroutine* self_coro = interpreter->current_executing_coroutine;

    // If we are in fast-forward mode, we must check if this is the await we are looking for.
    // If not, we skip this entire await expression to avoid executing it.
    if (interpreter->prevent_side_effects) {
        if (!self_coro || !self_coro->has_yielding_await_state ||
            !(self_coro->yielding_await_token->line == interpreter->current_token->line &&
              self_coro->yielding_await_token->col == interpreter->current_token->col))
        {
            // This is NOT the await we are resuming.
            // If the current token is an await, we must dry-run it.
            // Otherwise, we let the normal parsing functions handle it (they will respect prevent_side_effects).
            if (interpreter->current_token->type == TOKEN_AWAIT) {
                interpreter_eat(interpreter, TOKEN_AWAIT);
                ExprResult dummy_res = interpret_logical_or_expr(interpreter); // This will also be in dry-run mode
                if (dummy_res.is_freshly_created_container) free_value_contents(dummy_res.value);
                return (ExprResult){ .value = create_null_value(), .is_freshly_created_container = false };
            }
        }
        // If it IS the await we are resuming, we fall through to the main logic below.
    }

    if (interpreter->current_token->type != TOKEN_AWAIT) {
        ExprResult res = interpret_logical_or_expr(interpreter);
        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            return res;
        }
        return res;
    }

    // --- AWAIT RESUME LOGIC ---
    if (self_coro && self_coro->has_yielding_await_state) { // Check if we are in a resume context
        if (self_coro->yielding_await_token &&
            self_coro->yielding_await_token->line == interpreter->current_token->line &&
            self_coro->yielding_await_token->col == interpreter->current_token->col)
        {
            DEBUG_PRINTF("AWAIT_RESUME_MATCH: Resuming at the correct await. Fast-forward mode DISABLED.%s", "");
            interpreter->prevent_side_effects = false; // Turn off fast-forward mode.
            self_coro->has_yielding_await_state = false; // We have successfully resumed and consumed the state.
            free_token(self_coro->yielding_await_token);
            self_coro->yielding_await_token = NULL;

            ExprResult final_result;
            if (self_coro->resumed_with_exception) {
                interpreter->exception_is_active = 1;
                free_value_contents(interpreter->current_exception);
                interpreter->current_exception = value_deep_copy(self_coro->value_from_await);
                final_result.value = create_null_value(); // Return dummy value on exception
            } else {
                final_result.value = value_deep_copy(self_coro->value_from_await);
            }

            final_result.is_freshly_created_container = true;
            final_result.is_standalone_primary_id = false;

            interpreter_eat(interpreter, TOKEN_AWAIT);
            interpreter->prevent_side_effects = true;
            ExprResult dummy_res = interpret_logical_or_expr(interpreter);
            interpreter->prevent_side_effects = false;

            if (dummy_res.is_freshly_created_container) {
                free_value_contents(dummy_res.value);
            }

            return final_result; // Return the actual result from the completed await.
        }
    }

    Token* await_keyword_token = token_deep_copy(interpreter->current_token);

    if (!self_coro) {
        report_error("Syntax", "'await' can only be used inside an 'async funct'.", await_keyword_token);
    }

    Coroutine* target_coro = NULL;

    // --- Simplified FIRST TIME PATH logic ---
    interpreter_eat(interpreter, TOKEN_AWAIT);
    ExprResult awaitable_expr_res = interpret_logical_or_expr(interpreter);

    if (interpreter->exception_is_active) {
        if (awaitable_expr_res.is_freshly_created_container) free_value_contents(awaitable_expr_res.value);
        free_token(await_keyword_token);
        return awaitable_expr_res;
    }

    if (awaitable_expr_res.value.type != VAL_COROUTINE && awaitable_expr_res.value.type != VAL_GATHER_TASK) {
        if (awaitable_expr_res.is_freshly_created_container) free_value_contents(awaitable_expr_res.value);
        report_error("Runtime", "Can only 'await' a coroutine or gather task.", await_keyword_token);
    }

    target_coro = awaitable_expr_res.value.as.coroutine_val;

    if (target_coro == self_coro) {
        if (awaitable_expr_res.is_freshly_created_container) free_value_contents(awaitable_expr_res.value);
        report_error("Runtime", "A coroutine cannot await itself.", await_keyword_token);
    }

    // --- COMMON LOGIC (YIELD or GET RESULT) ---
    if (target_coro->state == CORO_DONE) {
        ExprResult final_result;
        if (target_coro->has_exception) {
            interpreter->exception_is_active = 1;
            free_value_contents(interpreter->current_exception);
            interpreter->current_exception = value_deep_copy(target_coro->exception_value);
            final_result.value = create_null_value();
        } else {
            final_result.value = value_deep_copy(target_coro->result_value);
        }
        // The awaitable was already completed. We've copied its result. We must now release the reference
        // to the temporary awaitable coroutine object if it was freshly created.
        if (awaitable_expr_res.is_freshly_created_container) {
            free_value_contents(awaitable_expr_res.value);
        }
        final_result.is_freshly_created_container = true;
        final_result.is_standalone_primary_id = false;

        free_token(await_keyword_token);
        self_coro->has_yielding_await_state = false; // We have successfully resumed and gotten the result.
        return final_result;
    }

    // --- YIELDING LOGIC ---
    if (target_coro->state == CORO_NEW) {
        if (target_coro->gather_tasks) { // It's a gather task
            target_coro->state = CORO_GATHER_WAIT;
            // When a gather task is first awaited, schedule all its children.
            for (int i = 0; i < target_coro->gather_tasks->count; ++i) {
                Coroutine* child_task = target_coro->gather_tasks->elements[i].as.coroutine_val;
                if (child_task->parent_gather_coro == NULL) {
                    child_task->parent_gather_coro = target_coro;
                }
                if (child_task->state == CORO_NEW) {
                    child_task->state = CORO_RUNNABLE;
                    add_to_ready_queue(interpreter, child_task);
                }
            }
        } else if (target_coro->name && strcmp(target_coro->name, "weaver.rest") == 0) {
            // Special handling for weaver.rest: it goes directly to the sleep queue.
            target_coro->state = CORO_SUSPENDED_TIMER;
            add_to_sleep_queue(interpreter, target_coro);
            DEBUG_PRINTF("AWAIT_EXPR: Timer coro %s (%p) put to sleep queue directly.", target_coro->name, (void*)target_coro);
        } else {
            target_coro->state = CORO_RUNNABLE;
            add_to_ready_queue(interpreter, target_coro);
        }
    }

    self_coro->state = CORO_SUSPENDED_AWAIT;
    self_coro->awaiting_on_coro = target_coro;
    coroutine_incref(target_coro);
    // --- START: Set precise resume state ---
    // Before overwriting the yielding_await_token, free the old one if it exists. This prevents a leak
    // if a new await is encountered during a resume cycle before the original await point is reached.
    if (self_coro->yielding_await_token) {
        free_token(self_coro->yielding_await_token);
    }
    self_coro->has_yielding_await_state = true;
    self_coro->yielding_await_state = get_lexer_state_for_token_start(interpreter->lexer, await_keyword_token->line, await_keyword_token->col, await_keyword_token);
    // Store the yielding token itself for the resume check. Transfer ownership of await_keyword_token.
    self_coro->yielding_await_token = await_keyword_token;
    // --- END: Set precise resume state ---

    // The references to the target_coro have now been transferred to the async machinery
    // (the ready queue and the awaiting_on_coro pointer). We can now release the temporary
    // reference held by the `awaitable_expr_res` if it was freshly created.
    if (awaitable_expr_res.is_freshly_created_container) {
        free_value_contents(awaitable_expr_res.value); // This will now just decref, not free.
    }

    coroutine_add_waiter(target_coro, self_coro);
    ExprResult pending_res = { .value = create_null_value() };
    return pending_res;
}
