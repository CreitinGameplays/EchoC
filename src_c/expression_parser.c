#include "expression_parser.h"
#include "statement_parser.h" // For interpret_statement declaration
#include "parser_utils.h"     // For interpreter_eat
#include "scope.h"            // For symbol_table_get
#include "value_utils.h"      // For evaluate_interpolated_string, value_to_string_representation
#include "dictionary.h"       // For dictionary_create, dictionary_set, dictionary_get
#include "modules/builtins.h" // For builtin_slice
#include "module_loader.h"    // <-- Added: for resolve_module_path and load_module_from_path

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

// Note: Statement parsing functions (interpret_show, interpret_let, interpret_if, interpret_loop, 
// interpret_funct, interpret_return, etc.) are defined in statement_parser.c.
// Expression parser relies on statement_parser.h for interpret_statement if needed (e.g. in execute_echoc_function).

extern void add_to_ready_queue(Interpreter* interpreter, Coroutine* coro); // From interpreter.c (via interpreter.h)
extern void run_event_loop(Interpreter* interpreter); // From interpreter.c (via interpreter.h)

/*
StatementExecStatus interpret_statement(Interpreter* interpreter) {
    DEBUG_PRINTF("INTERPRET_STATEMENT: Token type: %s, value: '%s'. Current scope: %p",
                 token_type_to_string(interpreter->current_token->type), //
                 interpreter->current_token->value ? interpreter->current_token->value : "N/A", //
                 (void*)interpreter->current_scope); //
    // Skip extra COLON tokens if they occur between statements.
    // Skip if in event loop but no current coroutine and queue is empty (event loop should handle this)
    if (interpreter->async_event_loop_active && !interpreter->current_executing_coroutine && interpreter->async_ready_queue_head == NULL) return STATEMENT_EXECUTED_OK;

    while (interpreter->current_token->type == TOKEN_COLON && interpreter->current_token->type != TOKEN_EOF) {
        interpreter_eat(interpreter, TOKEN_COLON);
    }

    if (interpreter->exception_is_active) { // If a statement raised an exception, propagate it
        return STATEMENT_PROPAGATE_FLAG; // Return appropriate status
    }

    StatementExecStatus status = STATEMENT_EXECUTED_OK;
    // Now process the statement.
    if (interpreter->current_token->type == TOKEN_SHOW) {
        interpret_show_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_RUN) {
        interpret_run_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_LET) {
        interpret_let_statement(interpreter); // This already eats its trailing colon
        return status; // Let statement handles its own trailing colon
    } else if (interpreter->current_token->type == TOKEN_ASYNC) { // Handle 'async funct'
        interpreter_eat(interpreter, TOKEN_ASYNC);
        if (interpreter->current_token->type != TOKEN_FUNCT) report_error_unexpected_token(interpreter, "'funct' after 'async'");
        interpret_funct_statement(interpreter); // interpret_funct_statement will know it's async
    } else if (interpreter->current_token->type == TOKEN_FUNCT) {
        interpret_funct_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_RETURN) {
        interpret_return_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_IF) {
        interpret_if_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_LOOP) {
        interpret_loop_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_BREAK) {
        interpret_break_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_CONTINUE) {
        interpret_continue_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_LBRACE) {
        interpret_block_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_RAISE) {
        if (!interpreter->in_try_catch_finally_block_definition) {
            report_error("Syntax", "'raise:' statement can only be used lexically inside a 'try', 'catch', or 'finally' block.", interpreter->current_token);
        }
        interpret_raise_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_TRY) {
        interpret_try_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_BLUEPRINT) {
        interpret_blueprint_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_LOAD) {
        interpret_load_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_ID ||
               interpreter->current_token->type == TOKEN_SUPER) { // Allow 'super' to start an expression statement
        interpret_expression_statement(interpreter); // This already eats its trailing colon
        return status; // Expression statement handles its own trailing colon
    } else if (interpreter->current_token->type == TOKEN_EOF) {
        return status; // End of file, do nothing
    } else {
        report_error_unexpected_token(interpreter, "a statement keyword (show, let, assign, funct, return, if, loop, etc.), an identifier (for a function call), or an opening brace '{'");
    }

    if (interpreter->coroutine_yielded_for_await) {
        status = STATEMENT_YIELDED_AWAIT;
    } else if (interpreter->break_flag || interpreter->continue_flag || interpreter->return_flag || interpreter->exception_is_active) {
        status = STATEMENT_PROPAGATE_FLAG;
    }
    return status;
}
*/

Value interpret_instance_creation(Interpreter* interpreter, Blueprint* bp_to_instantiate, Token* call_site_token); // Made non-static

static void coroutine_add_waiter(Coroutine* self, Coroutine* waiter_coro_to_add); // Moved forward declaration earlier

// static Value interpret_method_call(Interpreter* interpreter, Object* self_obj, Function* method_func, Token* call_site_token); // Marked as unused
// Value call_bound_method_with_args(Interpreter* interpreter, BoundMethod* bm, Value* call_args, int arg_count, Token* call_site_token); // Replaced

ExprResult interpret_primary_expr(Interpreter* interpreter) {
    Token* token = interpreter->current_token;
    Value val;
    ExprResult expr_res;
    expr_res.is_standalone_primary_id = false; // Default for most primaries
    expr_res.is_freshly_created_container = false; // Default

    if (token->type == TOKEN_LBRACE) { 
        expr_res.value = interpret_dictionary_literal(interpreter);
        if (expr_res.value.type == VAL_DICT) expr_res.is_freshly_created_container = true;
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
        char* string_to_interpolate = strdup(token->value);
        if (!string_to_interpolate) report_error("System", "Failed to strdup string for interpolation.", token);
        expr_res.value = evaluate_interpolated_string(interpreter, string_to_interpolate, token);
        // A string from interpolation is always new.
        if (expr_res.value.type == VAL_STRING) expr_res.is_freshly_created_container = true;
        free(string_to_interpolate); // Free the copy after interpolation is done.
        interpreter_eat(interpreter, TOKEN_STRING);
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
        Token* id_token_for_reporting = token_deep_copy(interpreter->current_token); // Deep copy for error reporting
        bool original_token_type_was_id = true;
        interpreter_eat(interpreter, TOKEN_ID);

        if (interpreter->current_token->type == TOKEN_LPAREN) {
            // Could be a regular function call OR a blueprint instantiation
            Value* id_val_ptr = symbol_table_get(interpreter->current_scope, id_name);
            if (id_val_ptr && id_val_ptr->type == VAL_BLUEPRINT) {
                // Instance creation: Dog(arg1, arg2)
                // interpret_instance_creation uses id_token_for_reporting for context, does not free it.
                free(id_name); // Free the strdup'd name
                free_token(id_token_for_reporting); // Free the deep-copied token
                expr_res.value = interpret_instance_creation(interpreter, id_val_ptr->as.blueprint_val, id_token_for_reporting);
                if (expr_res.value.type == VAL_OBJECT) expr_res.is_freshly_created_container = true;
                expr_res.is_standalone_primary_id = false; // Instance creation is an operation
                // id_name and id_token_for_reporting will be freed at the end of this block
            } else {
                // interpret_any_function_call uses id_name for lookup and id_token_for_reporting for errors.
                // It does not free them.
                expr_res.value = interpret_any_function_call(interpreter, id_name, id_token_for_reporting, NULL);
                // If the result is a coroutine, it's a fresh container.
                if (expr_res.value.type == VAL_COROUTINE || expr_res.value.type == VAL_GATHER_TASK) {
                     expr_res.is_freshly_created_container = true;
                }
                // Result of a function call is considered new/temporary if it's a container
                if (expr_res.value.type == VAL_OBJECT || expr_res.value.type == VAL_ARRAY || expr_res.value.type == VAL_DICT || expr_res.value.type == VAL_STRING) {
                    expr_res.is_freshly_created_container = true;
                } else {
                    expr_res.is_freshly_created_container = false;
                }
                expr_res.is_standalone_primary_id = false; // Function call is an operation
                // id_name and id_token_for_reporting will be freed at the end of this block
            }
        }
        // If not a call, it's a variable or super
        else if (strcmp(id_name, "super") == 0) { // Changed to else if
            if (!interpreter->current_self_object) {
                // free(id_name); // Will be freed at the end
                // free_token(id_token_for_reporting); // Will be freed at the end
                report_error("Runtime", "'super' can only be used within an instance method.", id_token_for_reporting);
            }
            val.type = VAL_SUPER_PROXY;
            // No data needed in val.as for VAL_SUPER_PROXY
            expr_res.value = val; expr_res.is_standalone_primary_id = false;
        } else {
            Value* var_val_ptr = symbol_table_get(interpreter->current_scope, id_name); // Regular variable lookup
            if (var_val_ptr == NULL) {
                char err_msg[100];
                sprintf(err_msg, "Undefined variable '%s'", id_name);
                // free(id_name); // Will be freed at the end
                // free_token(id_token_for_reporting); // Will be freed at the end
                report_error("Runtime", err_msg, id_token_for_reporting);
            }
            // For objects, arrays, dicts from variables, pass by reference (share the pointer in the Value struct).
            // Other types (primitives, strings that are copied by value_deep_copy, functions) are deep copied.
            if (var_val_ptr->type == VAL_OBJECT || var_val_ptr->type == VAL_ARRAY || var_val_ptr->type == VAL_DICT) {
                expr_res.value = *var_val_ptr; // Shallow copy of Value struct; shares the data pointer.
                expr_res.is_freshly_created_container = false;
                expr_res.is_standalone_primary_id = true; // This is a standalone ID lookup
            } else {
                expr_res.value = value_deep_copy(*var_val_ptr);
                // If value_deep_copy created a new container (string, array, dict, tuple, object), it's fresh.
                if (expr_res.value.type == VAL_STRING || expr_res.value.type == VAL_ARRAY ||
                    expr_res.value.type == VAL_DICT || expr_res.value.type == VAL_TUPLE ||
                    expr_res.value.type == VAL_OBJECT) {
                    expr_res.is_freshly_created_container = true;
                } else { // Primitives, VAL_NULL, VAL_BLUEPRINT (ptr copy), VAL_FUNCTION (ptr copy essentially)
                    expr_res.is_freshly_created_container = false;
                    if (original_token_type_was_id) expr_res.is_standalone_primary_id = true; // If original was ID, it's standalone
                }
            }
        }
        free(id_name); // Free the strdup'd name
        free_token(id_token_for_reporting); // Free the deep-copied token
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
    Value result = current_expr_res.value; // The actual value being operated on
    // is_freshly_created_container applies to this initial 'result'
    bool result_is_freshly_created = current_expr_res.is_freshly_created_container;
    bool is_still_standalone_id = current_expr_res.is_standalone_primary_id;
    
    while (interpreter->current_token->type == TOKEN_LBRACKET || 
           interpreter->current_token->type == TOKEN_DOT ||
           (result.type == VAL_BOUND_METHOD && interpreter->current_token->type == TOKEN_LPAREN) || /* Bound method call */
           (result.type == VAL_BLUEPRINT && interpreter->current_token->type == TOKEN_LPAREN) || /* Blueprint instantiation */
           ((result.type == VAL_COROUTINE || result.type == VAL_GATHER_TASK) && interpreter->current_token->type == TOKEN_LPAREN) /* Calling a coroutine (error) or a C-builtin that returns coro */
          ) {
        Value next_derived_value; // This will become the new 'result'
        bool next_derived_is_fresh = false; // And its freshness
        is_still_standalone_id = false; // Any postfix operation means it's no longer a standalone ID

        if (result.type == VAL_BOUND_METHOD && interpreter->current_token->type == TOKEN_LPAREN) {
            // Handle immediate call of a bound method: e.g. obj.method()
            Value bound_method_to_call = result; // Keep the VAL_BOUND_METHOD to free its contents
            next_derived_value = interpret_any_function_call(interpreter, NULL, interpreter->current_token /* for error context */, &bound_method_to_call);
            // Result of a function call is considered new/temporary if it's a container or coroutine
            if (next_derived_value.type == VAL_OBJECT || next_derived_value.type == VAL_ARRAY || next_derived_value.type == VAL_DICT || next_derived_value.type == VAL_STRING) {
                next_derived_is_fresh = true;
            } else {
                next_derived_is_fresh = false;
            }
            free_value_contents(bound_method_to_call); // Free the BoundMethod struct itself
            // interpret_any_function_call will consume the LPAREN, args, and RPAREN.
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
                free_value_contents(result); // Free the VAL_BLUEPRINT's contents
            }
        } else if (interpreter->current_token->type == TOKEN_LBRACKET) {
            Token* bracket_token = interpreter->current_token;
            interpreter_eat(interpreter, TOKEN_LBRACKET);
            ExprResult index_expr_res = interpret_expression(interpreter);
            Value index_val = index_expr_res.value;
            
            if (result.type == VAL_ARRAY) {
                if (index_val.type != VAL_INT) {
                    free_value_contents(result); free_value_contents(index_val);
                    report_error("Runtime", "Array index must be an integer.", bracket_token);
                }
                Array* arr_ptr = result.as.array_val; 
                long idx = index_val.as.integer; 
                long effective_idx = idx;
                if (effective_idx < 0) effective_idx += arr_ptr->count;

                if (effective_idx < 0 || effective_idx >= arr_ptr->count) {
                    free_value_contents(result); free_value_contents(index_val);
                    report_error("Runtime", "Array index out of bounds.", bracket_token);
                }
                Value element = value_deep_copy(arr_ptr->elements[effective_idx]);
                next_derived_value = element;
                next_derived_is_fresh = true; // Deep copy is fresh
                // index_val (if int) has no complex contents to free via free_value_contents.
            } else if (result.type == VAL_DICT) {
                if (index_val.type != VAL_STRING) {
                    // result is handled by result_is_freshly_created check later.
                    free_value_contents(index_val); // Free index_val if it's not a string.
                    report_error("Runtime", "Dictionary key must be a string.", bracket_token);
                }                
                Value element;
                // If the dictionary itself is a temporary/freshly created one, we must deep copy its elements
                // to avoid dangling pointers when the temporary dictionary is freed.
                // Otherwise, we can get a "view" (shallow copy of Value struct) into the persistent dictionary.
                if (!dictionary_try_get(result.as.dict_val, index_val.as.string_val, &element, result_is_freshly_created /*DEEP_COPY_CONTENTS if dict is fresh*/)) {
                     char err_msg[150];
                     sprintf(err_msg, "Key '%s' not found in dictionary.", index_val.as.string_val);
                     if(result_is_freshly_created) free_value_contents(result);
                     free_value_contents(index_val); // index_val is VAL_STRING here
                     report_error("Runtime", err_msg, bracket_token);
                }
                free_value_contents(index_val); // index_val (string key) was used by dictionary_try_get.
                next_derived_value = element; // element is a deep copy.
                // If the dictionary was fresh, 'element' is a deep copy and thus fresh.
                // If the dictionary was not fresh, 'element' is a view, so it's not fresh.
                // Exception: if dictionary_try_get with false still deep_copies strings (it shouldn't for VAL_STRING from dict).
                // value_deep_copy always makes fresh strings/arrays/dicts/tuples/objects.
                // A view (shallow copy of Value struct) is not "freshly_created_container".
                if (result_is_freshly_created && (element.type == VAL_STRING || element.type == VAL_ARRAY || element.type == VAL_DICT || element.type == VAL_TUPLE || element.type == VAL_OBJECT)) {
                    next_derived_is_fresh = true;
                } else {
                    next_derived_is_fresh = false;
                }
            } else if (result.type == VAL_STRING) {
                if (index_val.type != VAL_INT) {
                    free_value_contents(result); free_value_contents(index_val);
                    report_error("Runtime", "String index must be an integer.", bracket_token);
                }
                long idx = index_val.as.integer;
                size_t str_len = strlen(result.as.string_val);
                if (idx < 0) idx = (long)str_len + idx;
                if (idx < 0 || (size_t)idx >= str_len) { // Cast idx to size_t for comparison
                    // result is handled by result_is_freshly_created check later.
                    // index_val (if int) has no complex contents.
                    report_error("Runtime", "String index out of bounds.", bracket_token);
                }
                char* charStr = malloc(2);
                if (!charStr) report_error("System", "Memory allocation failed during string indexing.", bracket_token);
                charStr[0] = result.as.string_val[idx];
                charStr[1] = '\0';
                // index_val (if int) has no complex contents.
                next_derived_value.type = VAL_STRING;
                next_derived_value.as.string_val = charStr;
                next_derived_is_fresh = true; // New string
            } else if (result.type == VAL_TUPLE) {
                if (index_val.type != VAL_INT) {
                    free_value_contents(result); free_value_contents(index_val);
                    report_error("Runtime", "Tuple index must be an integer.", bracket_token);
                }
                Tuple* tuple_ptr = result.as.tuple_val;
                long idx = index_val.as.integer;
                long effective_idx = idx;
                if (effective_idx < 0) effective_idx += tuple_ptr->count;

                if (effective_idx < 0 || effective_idx >= tuple_ptr->count) {
                    free_value_contents(result); free_value_contents(index_val);
                    report_error("Runtime", "Tuple index out of bounds.", bracket_token);
                }
                Value element = value_deep_copy(tuple_ptr->elements[effective_idx]);
                next_derived_value = element;
                next_derived_is_fresh = true; // Deep copy is fresh
                // index_val (if int) has no complex contents.
            } else {
                free_value_contents(result); free_value_contents(index_val);
                report_error("Runtime", "Can only index into arrays, strings, dictionaries, or tuples.", bracket_token);
            }
            interpreter_eat(interpreter, TOKEN_RBRACKET);

            // Manage freeing of the old 'result' (which is 'result' before this assignment)
            bool should_free_old_result_contents = result_is_freshly_created;

            // For VAL_DICT indexing, next_derived_value is now always a deep copy,
            // so no special ownership transfer of the dictionary's contents to a view is needed.
            // The original 'result' (dictionary) can be freed if it was fresh.
            // For VAL_ARRAY, VAL_TUPLE, VAL_STRING indexing, next_derived_value is also always a new copy.
            // So if result_is_freshly_created, old 'result' should be freed. This is covered by the initial assignment.
            if (should_free_old_result_contents) free_value_contents(result);
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
                        sprintf(err_msg, "Object of blueprint '%s' has no attribute or method '%s'.", obj->blueprint->name, attr_name);
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
                                next_derived_value.type == VAL_OBJECT) {
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
                        report_error("System", "Failed to strdup blueprint name for .name access", dot_token);
                        free_token(dot_token);
                    }
                    // No need to set attribute_handled_by_special_case here as it's the end of VAL_BLUEPRINT specific logic for "name"
                    next_derived_is_fresh = true; // New string
                } else { // Regular class attribute/static method
                    Value* class_attr_ptr = symbol_table_get_local(bp->class_attributes_and_methods, attr_name);
                    if (!class_attr_ptr) {
                        char err_msg[150];
                        sprintf(err_msg, "Blueprint '%s' has no class attribute or static method '%s'.", bp->name, attr_name);
                        free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                        report_error("Runtime", err_msg, dot_token);
                        free_token(dot_token);
                    }
                    next_derived_value = value_deep_copy(*class_attr_ptr);
                    next_derived_is_fresh = true; // Deep copy is fresh
                }
            } else if (result.type == VAL_ARRAY) {
                if (strcmp(attr_name, "append") == 0) {
                    BoundMethod* bm = malloc(sizeof(BoundMethod));
                    if (!bm) {
                        free(attr_name); free_token(dot_token);
                        if(result_is_freshly_created) free_value_contents(result);
                        report_error("System", "Failed to allocate memory for array.append bound method.", dot_token);
                    }
                    bm->type = FUNC_TYPE_C_BUILTIN;
                    bm->func_ptr.c_builtin = builtin_append;
                    bm->self_value = result; // The array itself. If result was fresh, bm takes ownership.
                    bm->self_is_owned_copy = result_is_freshly_created; 

                    next_derived_value.type = VAL_BOUND_METHOD;
                    next_derived_value.as.bound_method_val = bm;
                    next_derived_is_fresh = true; // The BoundMethod struct is new.
                } else {
                    char err_msg[150];
                    sprintf(err_msg, "Array has no attribute or method '%s'.", attr_name);
                    free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                    report_error("Runtime", err_msg, dot_token);
                    free_token(dot_token);
                }
            } else if (result.type == VAL_DICT) { // Handle dict.key access
                Dictionary* dict = result.as.dict_val;
                Value val_from_dict;
                if (dictionary_try_get(dict, attr_name, &val_from_dict, result_is_freshly_created /*DEEP_COPY_CONTENTS if dict is fresh*/)) {
                    next_derived_value = val_from_dict; 
                    next_derived_is_fresh = result_is_freshly_created && (val_from_dict.type == VAL_STRING || val_from_dict.type == VAL_ARRAY || val_from_dict.type == VAL_DICT || val_from_dict.type == VAL_TUPLE || val_from_dict.type == VAL_OBJECT);
                } else { // Key not found
                    char err_msg[150];
                    sprintf(err_msg, "Key '%s' not found in dictionary.", attr_name);
                    free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                    report_error("Runtime", err_msg, dot_token);
                    free_token(dot_token); // Free if report_error didn't exit
                    next_derived_is_fresh = true; // Deep copy is fresh
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
                        sprintf(err_msg, "Parent blueprint of '%s' does not have attribute or method '%s'.", self_obj_for_super->blueprint->name, attr_name);
                    else
                        sprintf(err_msg, "Attribute '%s' in parent blueprint of '%s' is not a method.", attr_name, self_obj_for_super->blueprint->name);

                    free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                    report_error("Runtime", err_msg, dot_token);
                    free_token(dot_token);
                }
                BoundMethod* bm = malloc(sizeof(BoundMethod));
                if (!bm) { /* error */ }
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
                sprintf(err_msg, "Cannot access attribute '%s' on non-object/blueprint/super_proxy type (got type %d).", attr_name, result.type);
                free(attr_name); if(result_is_freshly_created) free_value_contents(result);
                report_error("Runtime", err_msg, dot_token);
                free_token(dot_token);
            }
            } // End of if (!attribute_handled_by_special_case)
            free(attr_name);
            free_token(dot_token); // Free the copied dot_token after successful processing or if error didn't exit

            // Manage freeing of the old 'result' (which is 'result' before this assignment)
            // before updating it to 'derived_value_after_dot'.
            bool should_free_old_result_contents = result_is_freshly_created;

            if (next_derived_value.type == VAL_BOUND_METHOD) {
                BoundMethod* bm = next_derived_value.as.bound_method_val;
                // If the bound method's self_value IS the old result (by pointer equality for complex types),
                // and the BM is marked as owning it (bm->self_is_owned_copy is true, which means result_is_freshly_created was true),
                // then the BM is now responsible for freeing it. Don't free old_result_contents here.
                if (bm->self_is_owned_copy) { 
                    if (result.type == VAL_OBJECT && bm->self_value.type == VAL_OBJECT && bm->self_value.as.object_val == result.as.object_val) {
                        should_free_old_result_contents = false;
                    } else if (result.type == VAL_ARRAY && bm->self_value.type == VAL_ARRAY && bm->self_value.as.array_val == result.as.array_val) {
                        should_free_old_result_contents = false;
                    }
                    // Add other types if they can be 'self' and are pointer-based and incorporated this way
                }
            } else if (result.type == VAL_OBJECT && next_derived_value.type == VAL_BLUEPRINT && result.as.object_val && next_derived_value.as.blueprint_val == result.as.object_val->blueprint) {
                // Accessing obj.blueprint. `result` contains the object. Don't free the object's contents.
                should_free_old_result_contents = false;
            } else {
                // Only free the 'result' container if it was freshly created in this postfix chain.
                // This 'else' branch is now covered by should_free_old_result_contents default state.
            }
            if (should_free_old_result_contents) {
                free_value_contents(result);
            }
        }
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
    Value left = left_res.value;

    if (interpreter->current_token->type == TOKEN_POWER) {
        Token* op_token = interpreter->current_token; // Save for error reporting
        interpreter_eat(interpreter, TOKEN_POWER);
        ExprResult right_res = interpret_power_expr(interpreter); // Right-associative
        Value right = right_res.value;

        if (!((left.type == VAL_INT || left.type == VAL_FLOAT) &&
              (right.type == VAL_INT || right.type == VAL_FLOAT))) {
            if (left_res.is_freshly_created_container) free_value_contents(left);
            if (right_res.is_freshly_created_container) free_value_contents(right);
            report_error("Runtime", "Operands for power operation ('^') must be numbers.", op_token);
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
        return final_res;
    }
    return left_res; // No power op, return original ExprResult
}

ExprResult interpret_unary_expr(Interpreter* interpreter) {
    ExprResult final_res;
    if (interpreter->current_token->type == TOKEN_NOT) {
        Token* op_token = interpreter->current_token;
        interpreter_eat(interpreter, TOKEN_NOT);
        ExprResult operand_res = interpret_unary_expr(interpreter);
        Value operand = operand_res.value;

        if (operand.type != VAL_BOOL) {
            free_value_contents(operand);
            report_error("Runtime", "Operand for 'not' must be a boolean.", op_token);
        }
        Value result;
        result.type = VAL_BOOL;
        result.as.bool_val = !operand.as.bool_val;
        // free_value_contents(operand) not needed as it's VAL_BOOL
        final_res.value = result;
        final_res.is_freshly_created_container = false;
        final_res.is_standalone_primary_id = false; // Result of 'not'
        return final_res;
    } else if (interpreter->current_token->type == TOKEN_MINUS) {
        Token* op_token = interpreter->current_token;
        interpreter_eat(interpreter, TOKEN_MINUS);
        ExprResult operand_res = interpret_unary_expr(interpreter);
        Value operand = operand_res.value; // operand_res.is_freshly_created_container is not directly used here
                                         // as we modify operand in place or error.
        if (operand.type == VAL_INT) {
            operand.as.integer = -operand.as.integer;
        } else if (operand.type == VAL_FLOAT) {
            operand.as.floating = -operand.as.floating;
        } else {
            free_value_contents(operand);
            report_error("Runtime", "Operand for unary minus must be a number.", op_token);
        }
        final_res.value = operand; // operand is modified in place
        final_res.is_freshly_created_container = operand_res.is_freshly_created_container; // Propagate if it was already fresh
        final_res.is_standalone_primary_id = false; // Result of unary minus
        return final_res;
    }
    return interpret_power_expr(interpreter);
}

ExprResult interpret_multiplicative_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_unary_expr(interpreter);
    Value left = left_res.value;
    bool current_res_is_fresh = left_res.is_freshly_created_container;
    bool is_standalone = left_res.is_standalone_primary_id;

    while (interpreter->current_token->type == TOKEN_MUL || interpreter->current_token->type == TOKEN_DIV ||
           interpreter->current_token->type == TOKEN_MOD) {
        TokenType op_type = interpreter->current_token->type;
        Token* op_token = interpreter->current_token;
        interpreter_eat(interpreter, op_type);
        ExprResult right_res = interpret_unary_expr(interpreter);
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
                    report_error("Runtime", "Cannot repeat string a negative number of times.", op_token);
                }
                size_t old_len = strlen(left.as.string_val);
                char* new_str = malloc(old_len * times + 1);
                if (!new_str) report_error("System", "Memory allocation failed for string repetition", op_token);
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
                    report_error("Runtime", "Cannot repeat string a negative number of times.", op_token);
                }
                size_t str_len = strlen(right.as.string_val);
                char* new_str = malloc(str_len * times + 1);
                if (!new_str) report_error("System", "Memory allocation failed for string repetition", op_token);
                new_str[0] = '\0';
                for (long i = 0; i < times; i++) strcat(new_str, right.as.string_val);
                result_val.type = VAL_STRING;
                result_val.as.string_val = new_str;
                new_op_res_is_fresh = true; // New string
            } else {
                if(current_res_is_fresh) free_value_contents(left);
                if(right_is_fresh) free_value_contents(right);
                report_error("Runtime", "Unsupported operand types for '*' operator.", op_token);
            }
        } else if (op_type == TOKEN_MOD) { // New modulo logic
            // Only support modulo for integers.
            if (left.type != VAL_INT || right.type != VAL_INT) {
                if(current_res_is_fresh) free_value_contents(left); else if (left.type != VAL_INT && left.type != VAL_FLOAT) free_value_contents(left);
                if(right_is_fresh) free_value_contents(right); else if (right.type != VAL_INT && right.type != VAL_FLOAT) free_value_contents(right);
                report_error("Runtime", "Operands for modulo ('%') must be integers.", op_token);
            }
            if (right.as.integer == 0) {
                // Operands are VAL_INT, no complex contents to free.
                report_error("Runtime", "Division by zero in modulo operation.", op_token);
            }
            result_val.type = VAL_INT;
            result_val.as.integer = left.as.integer % right.as.integer;
            new_op_res_is_fresh = false; // Numeric result
        } else { // TOKEN_DIV
            if (!((left.type == VAL_INT || left.type == VAL_FLOAT) && (right.type == VAL_INT || right.type == VAL_FLOAT))) {
                if(current_res_is_fresh) free_value_contents(left); else if (left.type != VAL_INT && left.type != VAL_FLOAT) free_value_contents(left);
                if(right_is_fresh) free_value_contents(right); else if (right.type != VAL_INT && right.type != VAL_FLOAT) free_value_contents(right);
                report_error("Runtime", "Operands for '/' must both be numbers.", op_token);
            }
            double left_val = (left.type == VAL_INT) ? left.as.integer : left.as.floating;
            double right_val = (right.type == VAL_INT) ? right.as.integer : right.as.floating;
            if (right_val == 0) {
                // Operands are numbers, no complex contents to free.
                report_error("Runtime", "Division by zero", op_token);
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
    }
    
    ExprResult final_res;
    final_res.value = left;
    final_res.is_freshly_created_container = current_res_is_fresh;
    final_res.is_standalone_primary_id = is_standalone;
    return final_res;
}
// Helper to execute a function (or method if self_obj is not NULL) with pre-evaluated arguments
Value execute_echoc_function(Interpreter* interpreter, Function* func_to_call, Object* self_obj, Value* call_args, bool* call_args_freshness, int arg_count, Token* call_site_token) {
    int min_required_args = 0;
    int expected_param_slots = func_to_call->param_count;
    int params_to_check_for_caller_arity = expected_param_slots;

    if (self_obj) { // Method call
        params_to_check_for_caller_arity = expected_param_slots - 1; // Caller doesn't provide 'self'
        for (int i = 1; i < expected_param_slots; ++i) { // Min required args, skipping 'self'
            if (!func_to_call->params[i].default_value) {
                min_required_args++;
            }
        }
        if (arg_count < min_required_args || arg_count > params_to_check_for_caller_arity) {
            // Free call_args before reporting error, as they were passed in
            for(int i=0; i<arg_count; ++i) {
                if(call_args_freshness[i]) free_value_contents(call_args[i]);
            }
            // A more robust way would be to pass freshness flags for call_args too.
            char err_msg[250];
            snprintf(err_msg, sizeof(err_msg), "Method '%s' expects %d arguments (excluding self, %d required), but %d were given.",
                     func_to_call->name, params_to_check_for_caller_arity, min_required_args, arg_count);
            report_error("Runtime", err_msg, call_site_token);
        }
    } else { // Regular function call
        for (int i = 0; i < expected_param_slots; ++i) {
            if (!func_to_call->params[i].default_value) {
                min_required_args++;
            }
        }
        if (arg_count < min_required_args || arg_count > expected_param_slots) {
            for(int i=0; i<arg_count; ++i) {
                if(call_args_freshness[i]) free_value_contents(call_args[i]);
            }

            char err_msg[250];
            snprintf(err_msg, sizeof(err_msg), "Function '%s' expects %d arguments (%d required), but %d were given.",
                     func_to_call->name, expected_param_slots, min_required_args, arg_count);
            report_error("Runtime", err_msg, call_site_token);
        }
    }

    Scope* old_scope = interpreter->current_scope;
    Object* old_self_obj_ctx = interpreter->current_self_object;

    interpreter->current_scope = func_to_call->definition_scope;
    enter_scope(interpreter);
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

    int current_arg_idx = 0;
    for (int i = (self_obj ? 1 : 0); i < func_to_call->param_count; ++i) { // Skip param 0 if 'self'
        if (current_arg_idx < arg_count) {
            // symbol_table_set makes a deep copy, so call_args[current_arg_idx] can be freed if it was fresh.
            symbol_table_set(interpreter->current_scope, func_to_call->params[i].name, call_args[current_arg_idx]);
            if (call_args_freshness[current_arg_idx]) { // Only free if it was a fresh temporary
                free_value_contents(call_args[current_arg_idx]);
            }
            current_arg_idx++;
        } else {
            if (!func_to_call->params[i].default_value) { /* Should be caught by arity */ }
            symbol_table_set(interpreter->current_scope, func_to_call->params[i].name, *(func_to_call->params[i].default_value));
        } // Default values are already deep copied when Function struct is made or stored.
    }

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

    while (!(interpreter->current_token->type == TOKEN_END &&
             (func_to_call->body_end_token_original_line == -1 || // Handle case where body might be empty and end token not pre-scanned
              (interpreter->current_token->line == func_to_call->body_end_token_original_line &&
               interpreter->current_token->col == func_to_call->body_end_token_original_col))) &&
           interpreter->current_token->type != TOKEN_EOF) {
        interpret_statement(interpreter);
        if (interpreter->exception_is_active && func_to_call->body_end_token_original_line == -1) {
            // If exception in a function with no pre-scanned end (e.g. op_str), ensure we break
        }
        if (interpreter->return_flag || interpreter->break_flag || interpreter->continue_flag || interpreter->exception_is_active) break;
    }

    interpreter->function_nesting_level--;
    Value result = value_deep_copy(interpreter->current_function_return_value);

    set_lexer_state(interpreter->lexer, old_lexer_state);
    free_token(interpreter->current_token);
    interpreter->current_token = old_current_token;

    exit_scope(interpreter);
    interpreter->current_scope = old_scope;
    interpreter->current_self_object = old_self_obj_ctx;
    interpreter->return_flag = 0;
    return result;
}

ExprResult interpret_additive_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_multiplicative_expr(interpreter);
    Value left = left_res.value;
    bool current_res_is_fresh = left_res.is_freshly_created_container;
    bool is_standalone = left_res.is_standalone_primary_id;

    while (interpreter->current_token->type == TOKEN_PLUS || interpreter->current_token->type == TOKEN_MINUS) {
        TokenType op_type = interpreter->current_token->type;
        Token* op_token = interpreter->current_token;
        interpreter_eat(interpreter, op_type);
        ExprResult right_res = interpret_multiplicative_expr(interpreter);
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

                char* new_str = malloc(strlen(s1_ptr) + strlen(s2_ptr) + 1);
                if (!new_str) report_error("System", "Memory allocation failed for string concatenation", op_token);
                strcpy(new_str, s1_ptr); strcat(new_str, s2_ptr);
                result_val.type = VAL_STRING; result_val.as.string_val = new_str;
                new_op_res_is_fresh = true; // New string
            } else {
                // Check for operator overloading (op_add)
                if (left.type == VAL_OBJECT) {
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
                        Value call_args[1];
                        call_args[0] = value_deep_copy(right); // Pass 'right' as the argument to op_add. value_deep_copy makes it fresh.
                        bool arg_freshness[1] = {true}; // The copied 'right' argument is fresh.

                        result_val = execute_echoc_function(interpreter, op_add_method_val->as.function_val, left_obj, call_args, arg_freshness, 1, op_token);
                        // execute_echoc_function is responsible for freeing the contents of call_args elements
                        // after they've been used (copied into the function's scope).
                        // So, no free_value_contents(call_args[0]) here.
                        // If op_add returns an object/array/dict, it's considered fresh
                        if (result_val.type == VAL_OBJECT || result_val.type == VAL_ARRAY || result_val.type == VAL_DICT) {
                            new_op_res_is_fresh = true;
                        }
                    } else {
                        if(current_res_is_fresh) free_value_contents(left);
                        if(right_is_fresh) free_value_contents(right);
                        report_error("Runtime", "Unsupported operand types for '+' operator (and no op_add method found).", op_token);
                    }
                } else { 
                    if(current_res_is_fresh) free_value_contents(left);
                    if(right_is_fresh) free_value_contents(right);
                    report_error("Runtime", "Unsupported operand types for '+' operator.", op_token);
                }
                new_op_res_is_fresh = false;
            }
        } else { // TOKEN_MINUS
            if (!((left.type == VAL_INT || left.type == VAL_FLOAT) && (right.type == VAL_INT || right.type == VAL_FLOAT))) {
                if(current_res_is_fresh) free_value_contents(left); else if (left.type != VAL_INT && left.type != VAL_FLOAT) free_value_contents(left);
                if(right_is_fresh) free_value_contents(right); else if (right.type != VAL_INT && right.type != VAL_FLOAT) free_value_contents(right);
                report_error("Runtime", "Operands for '-' must both be numbers.", op_token);
            }
            double left_val = (left.type == VAL_INT) ? left.as.integer : left.as.floating;
            double right_val = (right.type == VAL_INT) ? right.as.integer : right.as.floating;
            // Promote to float if either is float, otherwise int.
            if (left.type == VAL_FLOAT || right.type == VAL_FLOAT) {
                 result_val.type = VAL_FLOAT;
                 result_val.as.floating = left_val - right_val;
            } else { // Both VAL_INT
                 result_val.type = VAL_INT;
                 result_val.as.integer = (long)(left_val - right_val); // Explicit cast of result
            }
            new_op_res_is_fresh = false; // Numeric result
        }
        // Free operands only if they were fresh temporaries from this expression chain
        if (current_res_is_fresh) free_value_contents(left);
        if (right_is_fresh) free_value_contents(right);

        left = result_val;
        current_res_is_fresh = new_op_res_is_fresh;
    }

    ExprResult final_res;
    final_res.value = left;
    final_res.is_freshly_created_container = current_res_is_fresh;
    final_res.is_standalone_primary_id = is_standalone;
    return final_res;
}

ExprResult interpret_comparison_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_additive_expr(interpreter);
    // We will operate on left_res directly or its components.

    while (interpreter->current_token->type == TOKEN_LT || interpreter->current_token->type == TOKEN_GT ||
           interpreter->current_token->type == TOKEN_LTE || interpreter->current_token->type == TOKEN_GTE) {
        TokenType op_type = interpreter->current_token->type;
        Token* op_token = interpreter->current_token;
        interpreter_eat(interpreter, op_type);
        ExprResult right_res = interpret_additive_expr(interpreter);

        if (!((left_res.value.type == VAL_INT || left_res.value.type == VAL_FLOAT) &&
              (right_res.value.type == VAL_INT || right_res.value.type == VAL_FLOAT))) {
            // Free operands if they were fresh containers before reporting error
            if (left_res.is_freshly_created_container) free_value_contents(left_res.value);
            if (right_res.is_freshly_created_container) free_value_contents(right_res.value);
            char err_msg[100];
            sprintf(err_msg, "Operands for comparison operator '%s' must be numbers.", op_token->value);
            report_error("Runtime", err_msg, op_token);
        }

        double left_val = (left_res.value.type == VAL_INT) ? (double)left_res.value.as.integer : left_res.value.as.floating;
        double right_val = (right_res.value.type == VAL_INT) ? (double)right_res.value.as.integer : right_res.value.as.floating;

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
    }

    // If the loop was entered, left_res was updated.
    // If not, original left_res is returned, preserving its freshness and standalone status.
    return left_res;
}

ExprResult interpret_equality_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_comparison_expr(interpreter);
    // Value left = left_res.value; // Operate on left_res directly

    while (interpreter->current_token->type == TOKEN_EQ || interpreter->current_token->type == TOKEN_NEQ) {
        TokenType op_type = interpreter->current_token->type;
        /* Token* op_token = interpreter->current_token; */ interpreter_eat(interpreter, op_type);
        ExprResult right_res = interpret_comparison_expr(interpreter);
        // Value right = right_res.value;

        Value result_val;
        result_val.type = VAL_BOOL;

        int are_equal = 0; // Assume not equal by default

        // Handle numeric and bool comparisons with potential type coercion
        if ((left_res.value.type == VAL_INT || left_res.value.type == VAL_FLOAT || left_res.value.type == VAL_BOOL) &&
            (right_res.value.type == VAL_INT || right_res.value.type == VAL_FLOAT || right_res.value.type == VAL_BOOL)) {
            
            double left_num, right_num;

            if (left_res.value.type == VAL_BOOL) left_num = left_res.value.as.bool_val ? 1.0 : 0.0;
            else if (left_res.value.type == VAL_INT) left_num = (double)left_res.value.as.integer;
            else left_num = left_res.value.as.floating; // VAL_FLOAT

            if (right_res.value.type == VAL_BOOL) right_num = right_res.value.as.bool_val ? 1.0 : 0.0;
            else if (right_res.value.type == VAL_INT) right_num = (double)right_res.value.as.integer;
            else right_num = right_res.value.as.floating; // VAL_FLOAT
            
            are_equal = (left_num == right_num);

        } else if (left_res.value.type == VAL_STRING && right_res.value.type == VAL_STRING) {
            are_equal = (strcmp(left_res.value.as.string_val, right_res.value.as.string_val) == 0);
        } else if (left_res.value.type == VAL_ARRAY && right_res.value.type == VAL_ARRAY) {
            // TODO: Define array equality (e.g., by reference or deep comparison)
            // For now, different array instances are not equal.
            are_equal = (left_res.value.as.array_val == right_res.value.as.array_val);
        } else if (left_res.value.type == VAL_TUPLE && right_res.value.type == VAL_TUPLE) {
            // TODO: Define tuple equality
            are_equal = (left_res.value.as.tuple_val == right_res.value.as.tuple_val);
        } else if (left_res.value.type == VAL_DICT && right_res.value.type == VAL_DICT) {
            // TODO: Define dictionary equality
            are_equal = (left_res.value.as.dict_val == right_res.value.as.dict_val);
        } else if (left_res.value.type != right_res.value.type) {
            // Different types not handled above are not equal.
            are_equal = 0;
        }
        // If types are the same but not handled above (e.g. custom types in future), they are not equal.

        result_val.as.bool_val = (op_type == TOKEN_EQ) ? are_equal : !are_equal;
        
        // Free original left_res.value and right_res.value if they were fresh containers
        if(left_res.is_freshly_created_container) free_value_contents(left_res.value);
        if(right_res.is_freshly_created_container) free_value_contents(right_res.value);
        
        left_res.value = result_val; // Update left_res
        left_res.is_freshly_created_container = false; // Boolean is not a fresh container
        left_res.is_standalone_primary_id = false; // Result of equality op
    }

    // If loop was entered, left_res was updated.
    // If not, original left_res is returned.
    return left_res;
}

ExprResult interpret_logical_and_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_equality_expr(interpreter);
    // Value left = left_res.value; // Operate on left_res directly

    while (interpreter->current_token->type == TOKEN_AND) {
        Token* op_token = interpreter->current_token;
        interpreter_eat(interpreter, TOKEN_AND);
        
        if (left_res.value.type == VAL_BOOL && !left_res.value.as.bool_val) { // Short-circuit
            ExprResult right_dummy_res = interpret_equality_expr(interpreter);
            if (right_dummy_res.is_freshly_created_container) free_value_contents(right_dummy_res.value);
            // left_res.value is already false (VAL_BOOL).
            // If left_res was a fresh container that evaluated to false (e.g. empty string in future), it should be freed.
            // However, current boolean conversion is direct. For now, this is okay.
            left_res.is_standalone_primary_id = false; // Result of 'and'
        } else {
            ExprResult right_res = interpret_equality_expr(interpreter);
            Value right = right_res.value;
            if (left_res.value.type != VAL_BOOL || right.type != VAL_BOOL) {
                // Free original left_res.value and right_res.value if they were fresh containers
                if(left_res.is_freshly_created_container) free_value_contents(left_res.value);
                if(right_res.is_freshly_created_container) free_value_contents(right); // right is right_res.value
                report_error("Runtime", "Operands for 'and' must be booleans.", op_token);
            }
            Value result_val;
            result_val.type = VAL_BOOL;
            result_val.as.bool_val = left_res.value.as.bool_val && right.as.bool_val;

            if(left_res.is_freshly_created_container) free_value_contents(left_res.value);
            if(right_res.is_freshly_created_container) free_value_contents(right_res.value);

            left_res.value = result_val;
            left_res.is_freshly_created_container = false;
            left_res.is_standalone_primary_id = false; // Result of 'and'
        }
    }

    return left_res;
}

ExprResult interpret_logical_or_expr(Interpreter* interpreter) {
    ExprResult left_res = interpret_logical_and_expr(interpreter);
    // Value left = left_res.value; // Operate on left_res directly

    while (interpreter->current_token->type == TOKEN_OR) {
        Token* op_token = interpreter->current_token;
        interpreter_eat(interpreter, TOKEN_OR);

        if (left_res.value.type == VAL_BOOL && left_res.value.as.bool_val) { // Short-circuit
            ExprResult right_dummy_res = interpret_logical_and_expr(interpreter);
            if (right_dummy_res.is_freshly_created_container) free_value_contents(right_dummy_res.value);
            left_res.is_standalone_primary_id = false; // Result of 'or'
        } else {
            ExprResult right_res = interpret_logical_and_expr(interpreter);
            Value right = right_res.value;

            if (left_res.value.type != VAL_BOOL || right.type != VAL_BOOL) {
                // Free original left_res.value and right_res.value if they were fresh containers
                if(left_res.is_freshly_created_container) free_value_contents(left_res.value);
                if(right_res.is_freshly_created_container) free_value_contents(right); // right is right_res.value
                report_error("Runtime", "Operands for 'or' must be booleans.", op_token);
            }
            Value result_val;
            result_val.type = VAL_BOOL;
            result_val.as.bool_val = left_res.value.as.bool_val || right.as.bool_val;

            if(left_res.is_freshly_created_container) free_value_contents(left_res.value);
            if(right_res.is_freshly_created_container) free_value_contents(right_res.value);

            left_res.value = result_val;
            left_res.is_freshly_created_container = false;
            left_res.is_standalone_primary_id = false; // Result of 'or'
        }
    }

    return left_res;
}


ExprResult interpret_ternary_expr(Interpreter* interpreter) {
    ExprResult cond_res = interpret_logical_or_expr(interpreter);
    Value condition = cond_res.value;
    // cond_res.is_standalone_primary_id is propagated if no ternary op

    if (interpreter->current_token->type == TOKEN_QUESTION) {
        Token* q_token = interpreter->current_token; // For error context if condition is not bool
        interpreter_eat(interpreter, TOKEN_QUESTION);

        if (condition.type != VAL_BOOL) {
            // cond_res.value is condition, which is VAL_BOOL if correct.
            // If not, free its contents. cond_res.is_freshly_created_container is not used here.
            free_value_contents(condition); 
            report_error("Runtime", "Condition for ternary operator must be a boolean.", q_token);
        }

        // Parse both branches as logical-or expressions.
        ExprResult true_expr_res = interpret_logical_or_expr(interpreter);
        interpreter_eat(interpreter, TOKEN_COLON);
        ExprResult false_expr_res = interpret_logical_or_expr(interpreter);

        if (condition.as.bool_val) {
            free_value_contents(false_expr_res.value); // Free the unused branch's value
            true_expr_res.is_standalone_primary_id = false; // Result of ternary is not standalone ID
            return true_expr_res; // Return the ExprResult of the chosen branch
        } else {
            free_value_contents(true_expr_res.value);
            false_expr_res.is_standalone_primary_id = false; // Result of ternary is not standalone ID
            return false_expr_res;
        }
    }
    return cond_res; // No ternary, return result of logical_or_expr
}

// Placeholder for await expression
ExprResult interpret_await_expr(Interpreter* interpreter) {
    if (interpreter->current_token->type == TOKEN_AWAIT) {
        interpreter_eat(interpreter, TOKEN_AWAIT);

        // Check if inside an async function context
        if (!interpreter->current_executing_coroutine) {
            report_error("Syntax", "'await' can only be used inside an 'async funct'.", interpreter->current_token);
        }

        Coroutine* self_coro = interpreter->current_executing_coroutine;

        if (self_coro->is_resumed_from_await) {
            self_coro->is_resumed_from_await = 0; // Reset flag
            ExprResult resumed_result;
            resumed_result.value = value_deep_copy(self_coro->value_from_await); // Get the stored result
            free_value_contents(self_coro->value_from_await); // Clear the stored value
            self_coro->value_from_await = create_null_value();

            // Check if the awaited coroutine completed with an exception
            if (self_coro->awaiting_on_coro && self_coro->awaiting_on_coro->has_exception) {
                interpreter->exception_is_active = 1;
                free_value_contents(interpreter->current_exception); // Free any old one
                interpreter->current_exception = value_deep_copy(self_coro->awaiting_on_coro->exception_value);
                // The await expression itself doesn't "return" a value in this case; it propagates the exception.
                // We still need to return an ExprResult, but its value will be ignored.
                resumed_result.value = create_null_value(); // Dummy value
            } else if (self_coro->is_cancelled) { // Check if self_coro was cancelled while suspended
                interpreter->exception_is_active = 1;
                free_value_contents(interpreter->current_exception);
                interpreter->current_exception.type = VAL_STRING;
                interpreter->current_exception.as.string_val = strdup(CANCELLED_ERROR_MSG);
                resumed_result.value = create_null_value();
            } else {
                resumed_result.value = value_deep_copy(self_coro->value_from_await); // Get the stored result
            }
            free_value_contents(self_coro->value_from_await); self_coro->value_from_await = create_null_value();

            if (resumed_result.value.type == VAL_STRING || resumed_result.value.type == VAL_ARRAY ||
                resumed_result.value.type == VAL_DICT || resumed_result.value.type == VAL_TUPLE ||
                resumed_result.value.type == VAL_OBJECT) {
                resumed_result.is_freshly_created_container = true;
            } else {
                resumed_result.is_freshly_created_container = false;
            }
            resumed_result.is_standalone_primary_id = false;
            return resumed_result;
        }

        ExprResult awaitable_expr_res = interpret_logical_or_expr(interpreter); // Parse the expression to await
        if (awaitable_expr_res.value.type != VAL_COROUTINE && awaitable_expr_res.value.type != VAL_GATHER_TASK) {
            free_value_contents(awaitable_expr_res.value);
            report_error("Runtime", "Can only 'await' a coroutine.", interpreter->current_token);
        }

        Coroutine* target_coro = awaitable_expr_res.value.as.coroutine_val;

        if (target_coro == self_coro) {
            if (awaitable_expr_res.is_freshly_created_container) free_value_contents(awaitable_expr_res.value);
            report_error("Runtime", "A coroutine cannot await itself.", interpreter->current_token);
        }

        // If the target coroutine is new, it needs to be scheduled.
        if (target_coro->state == CORO_NEW) {
            target_coro->state = CORO_RUNNABLE; // Mark as runnable
            add_to_ready_queue(interpreter, target_coro);
        }

        if (target_coro->state == CORO_DONE) { // Await on already completed coroutine
            if (awaitable_expr_res.is_freshly_created_container) free_value_contents(awaitable_expr_res.value);
            ExprResult immediate_result;
            if (target_coro->has_exception) {
                interpreter->exception_is_active = 1;
                free_value_contents(interpreter->current_exception);
                interpreter->current_exception = value_deep_copy(target_coro->exception_value);
                immediate_result.value = create_null_value(); // Dummy
            } else {
                immediate_result.value = value_deep_copy(target_coro->result_value);
            }
            immediate_result.is_freshly_created_container = (immediate_result.value.type >= VAL_STRING && immediate_result.value.type <= VAL_OBJECT);
            immediate_result.is_standalone_primary_id = false;
            return immediate_result;
        } else if (target_coro->is_cancelled) { // Await on an already cancelled coroutine
             if (awaitable_expr_res.is_freshly_created_container) free_value_contents(awaitable_expr_res.value);
            interpreter->exception_is_active = 1;
            free_value_contents(interpreter->current_exception);
            interpreter->current_exception.type = VAL_STRING;
            interpreter->current_exception.as.string_val = strdup(CANCELLED_ERROR_MSG);
            ExprResult cancelled_res;
            cancelled_res.value = create_null_value();
            cancelled_res.is_freshly_created_container = false;
            cancelled_res.is_standalone_primary_id = false;
            return cancelled_res;
        }

        // Suspend self_coro
        self_coro->resume_state = get_lexer_state(interpreter->lexer); // Save current execution point
        self_coro->state = CORO_SUSPENDED_AWAIT; // More specific state
        self_coro->awaiting_on_coro = target_coro;
        if (target_coro) { // If we are actually awaiting something
            target_coro->ref_count++; // self_coro now holds a reference
            DEBUG_PRINTF("AWAIT_SUSPEND: Coro %s (%p) incremented ref_count of target %s (%p) to %d", self_coro->name ? self_coro->name : "unnamed_s", (void*)self_coro, target_coro->name ? target_coro->name : "unnamed_t", (void*)target_coro, target_coro->ref_count);
        }
        coroutine_add_waiter(target_coro, self_coro); // self_coro is waiting on target_coro

        interpreter->coroutine_yielded_for_await = 1; // Signal to event loop

        // The VAL_COROUTINE from awaitable_expr_res is not freed here; its pointer is now in self_coro->awaiting_on_coro.
        // The event loop will manage target_coro. If awaitable_expr_res was fresh, its Value struct will be freed by caller.

            // When an await causes a yield, the expression effectively produces no value *at this moment*.
            // The 'let' statement or other context will receive this. Upon resumption, the actual value
            // from the awaited coroutine will be injected.
            // The crucial part is that `interpret_await_expr` returns, and the lexer is positioned *after* the awaited expression.
        ExprResult pending_res;
        // Ensure current_token is advanced past the awaitable_expr if it hasn't been.
        // This is a safeguard; interpret_logical_or_expr should handle this.
        // If awaitable_expr_res.value.type was VAL_COROUTINE, it means interpret_logical_or_expr parsed it.
        // The current_token should already be past it. This line is unlikely to be the true fix if parser logic is sound.
        pending_res.value = create_null_value(); // Actual value will be injected on resume
        pending_res.is_freshly_created_container = false;
        pending_res.is_standalone_primary_id = false;
        return pending_res;
    }
    return interpret_logical_or_expr(interpreter); // Not an await, continue parsing
}

static void coroutine_add_waiter(Coroutine* self, Coroutine* waiter_coro_to_add) {
    CoroutineWaiterNode* new_node = malloc(sizeof(CoroutineWaiterNode));
    if (!new_node) {
        report_error("System", "Failed to allocate CoroutineWaiterNode.", NULL);
    }
    new_node->waiter_coro = waiter_coro_to_add;
    new_node->next = self->waiters_head;
    self->waiters_head = new_node;
    DEBUG_PRINTF("COROUTINE_ADD_WAITER: Coro %s (%p) added as waiter to %s (%p)",
                 waiter_coro_to_add->name ? waiter_coro_to_add->name : "unnamed_waiter", (void*)waiter_coro_to_add,
                 self->name ? self->name : "unnamed_target", (void*)self);
}

Value interpret_dictionary_literal(Interpreter* interpreter) {
    Token* lbrace_token = interpreter->current_token;
    interpreter_eat(interpreter, TOKEN_LBRACE);
    Dictionary* dict = dictionary_create(16, lbrace_token);
    
    if (interpreter->current_token->type != TOKEN_RBRACE) {
        while (1) {
            ExprResult key_res = interpret_expression(interpreter); // Key can be any expression
            Value key = key_res.value;
            if (key.type != VAL_STRING) {
                free_value_contents(key);
                report_error("Syntax", "Dictionary keys must be (or evaluate to) strings.", interpreter->current_token);
            }
            
            interpreter_eat(interpreter, TOKEN_COLON);
            ExprResult value_res = interpret_expression(interpreter);
            Value value = value_res.value;
            
            dictionary_set(dict, key.as.string_val, value, interpreter->current_token);
            free_value_contents(key); // key string was copied by dictionary_set
            if (value_res.is_freshly_created_container) { // Only free if the value expression resulted in a new container
                free_value_contents(value); // value was copied by dictionary_set
            }
            
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

// Helper to parse arguments for any function/method call.
// Assumes LPAREN was already consumed. Consumes tokens up to and including RPAREN.
// Populates args_out, arg_count_out, arg_is_fresh_out.
// Removed unused parameter warning for call_site_token_for_errors
static void parse_call_arguments(Interpreter* interpreter, Value args_out[], int* arg_count_out, bool arg_is_fresh_out[], int max_args, Token* call_site_token_for_errors) {
    *arg_count_out = 0;    
    if (interpreter->current_token->type != TOKEN_RPAREN) {
        do {
            if (*arg_count_out >= max_args) {
                report_error("Syntax", "Exceeded maximum number of function arguments (10).", interpreter->current_token);
                (void)call_site_token_for_errors; // Mark as unused if error path doesn't use it
            }
            ExprResult arg_expr_res = interpret_expression(interpreter);
            args_out[*arg_count_out] = arg_expr_res.value;
            arg_is_fresh_out[*arg_count_out] = arg_expr_res.is_freshly_created_container;
            (*arg_count_out)++;
            if (interpreter->current_token->type == TOKEN_COMMA) {
                interpreter_eat(interpreter, TOKEN_COMMA);
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
    interpreter_eat(interpreter, TOKEN_LPAREN); // Consume '('

    Value args[10]; // Max 10 arguments
    int arg_count = 0;
    bool arg_is_fresh[10] = {false}; // Track freshness of arguments
    // Parse arguments using the new helper
    parse_call_arguments(interpreter, args, &arg_count, arg_is_fresh, 10, func_name_token_for_error_reporting);

    Value result;

    if (bound_method_val_or_null && bound_method_val_or_null->type == VAL_BOUND_METHOD) {
        BoundMethod* bm = bound_method_val_or_null->as.bound_method_val;
        if (bm->type == FUNC_TYPE_C_BUILTIN && bm->func_ptr.c_builtin == builtin_append) {
            if (bm->self_value.type != VAL_ARRAY) {
                for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
                report_error("Internal", "append method bound to non-array.", func_name_token_for_error_reporting);
            }
            // For C builtins, pass self as the first argument
            Value final_args_for_c_builtin[11]; // self + max 10 args
            final_args_for_c_builtin[0] = bm->self_value; // Array is self
            for (int i = 0; i < arg_count; ++i) {
                final_args_for_c_builtin[i+1] = args[i];
            }
            result = builtin_append(interpreter, final_args_for_c_builtin, arg_count + 1, func_name_token_for_error_reporting);
            // Free parsed arguments if they were fresh (builtin_append doesn't consume them in a way execute_echoc_function does)
            for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
            // bm->self_value (the array) is modified in-place. If bm->self_is_owned_copy,
            // the VAL_BOUND_METHOD's free_value_contents will handle it.
        } else if (bm->type == FUNC_TYPE_ECHOC) { // Regular EchoC method
            Object* self_obj_ptr = NULL;
            if (bm->self_value.type == VAL_OBJECT) {
                self_obj_ptr = bm->self_value.as.object_val;
            } else {
                for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]); // Free args before error
                report_error("Internal", "Bound method 'self' is not an object for a non-builtin method call.", func_name_token_for_error_reporting);
            }
            result = execute_echoc_function(interpreter, bm->func_ptr.echoc_function, self_obj_ptr, args, arg_is_fresh, arg_count, func_name_token_for_error_reporting);
            // execute_echoc_function handles freeing args elements after copying them.
        } else {
            report_error("Internal", "Unknown bound method type in call.", func_name_token_for_error_reporting);
        }
    } else { // Regular function call (not bound method)
        if (!func_name_str_or_null_for_bound) {
            report_error("Internal", "Function name missing for non-bound call.", func_name_token_for_error_reporting);
        }
        const char* func_name_str = func_name_str_or_null_for_bound;

        if (strcmp(func_name_str, "slice") == 0) {
            result = builtin_slice(interpreter, args, arg_count, func_name_token_for_error_reporting);
            // builtin_slice doesn't consume args, free them if fresh
            for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
        } else if (strcmp(func_name_str, "async_sleep") == 0) {
            result = builtin_async_sleep_create_coro(interpreter, args, arg_count, func_name_token_for_error_reporting);
            // Args are consumed by the builtin, no need to free here.
        } else if (strcmp(func_name_str, "gather") == 0) {
            result = builtin_gather_create_coro(interpreter, args, arg_count, func_name_token_for_error_reporting);
            // Args (the array of coroutines) are deep copied by gather, so free original if fresh.
            for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
        } else if (strcmp(func_name_str, "cancel") == 0) {
            result = builtin_cancel_coro(interpreter, args, arg_count, func_name_token_for_error_reporting);
            for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
        } else { // Look for user-defined function
            Value* func_val_ptr = symbol_table_get(interpreter->current_scope, func_name_str);
            if (func_val_ptr && func_val_ptr->type == VAL_FUNCTION) {
                Function* func_to_run = func_val_ptr->as.function_val;
                if (func_to_run->is_async) {
                    Coroutine* coro = malloc(sizeof(Coroutine));
                    if (!coro) {
                        for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
                        report_error("System", "Failed to allocate Coroutine object.", func_name_token_for_error_reporting);
                    }
                    coro->function_def = func_to_run;
                    coro->ref_count = 1;
                    // Initialize resume_state for the first run
                    coro->resume_state = func_to_run->body_start_state; // Copy pos, line, col, etc.
                    // Ensure resume_state's text pointers are for the function's owned copy
                    coro->resume_state.text = func_to_run->source_text_owned_copy;
                    coro->resume_state.text_length = func_to_run->source_text_length;
                    DEBUG_PRINTF("CORO_CREATE (EchoC): Initialized ref_count for %s (%p) to 1", func_to_run->name, (void*)coro);

                    // Argument and scope setup
                    Scope* old_interpreter_scope = interpreter->current_scope; // Save interpreter's current scope
                    interpreter->current_scope = func_to_run->definition_scope; // Temporarily set to function's definition scope
                    enter_scope(interpreter); // Create the coroutine's execution scope
                    coro->execution_scope = interpreter->current_scope; // Assign the new scope to the coroutine

                    int min_required_args = 0;
                    for (int i = 0; i < func_to_run->param_count; ++i) {
                        if (!func_to_run->params[i].default_value) min_required_args++;
                    }
                    if (arg_count < min_required_args || arg_count > func_to_run->param_count) {
                        exit_scope(interpreter); // Clean up the entered scope
                        interpreter->current_scope = old_interpreter_scope; // Restore interpreter's scope
                        free(coro); // Free partially created coroutine
                        for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
                        char err_msg[250];
                        snprintf(err_msg, sizeof(err_msg), "Async function '%s' expects %d arguments (%d required), but %d were given.",
                                 func_to_run->name, func_to_run->param_count, min_required_args, arg_count);
                        report_error("Runtime", err_msg, func_name_token_for_error_reporting);
                    }

                    int current_arg_idx = 0;
                    for (int i = 0; i < func_to_run->param_count; ++i) {
                        if (current_arg_idx < arg_count) {
                            symbol_table_set(coro->execution_scope, func_to_run->params[i].name, args[current_arg_idx]);
                            current_arg_idx++;
                        } else { // Use default value
                            symbol_table_set(coro->execution_scope, func_to_run->params[i].name, *(func_to_run->params[i].default_value));
                        }
                    }
                    interpreter->current_scope = old_interpreter_scope; // Restore interpreter's original scope
                    // The `args` array elements (if fresh) will be freed by the outer logic of interpret_any_function_call
                    // after this block, which is correct as symbol_table_set made deep copies.

                    // Initialize coroutine fields
                    coro->name = strdup(func_to_run->name);
                    // coro->execution_scope is now set
                    coro->state = CORO_NEW;
                    coro->result_value = create_null_value();
                    coro->exception_value = create_null_value();
                    coro->has_exception = 0;
                    coro->awaiting_on_coro = NULL;
                    coro->value_from_await = create_null_value();
                    coro->is_resumed_from_await = 0;
                    coro->wakeup_time_sec = 0;
                    coro->gather_tasks = NULL;
                    coro->gather_results = NULL;
                    coro->gather_pending_count = 0;
                    coro->gather_first_exception_idx = -1;
                    coro->parent_gather_coro = NULL;
                    coro->is_cancelled = 0;
                    coro->waiters_head = NULL;

                    result.type = VAL_COROUTINE;
                    result.as.coroutine_val = coro;
                } else {
                    result = execute_echoc_function(interpreter, func_to_run, NULL, args, arg_is_fresh, arg_count, func_name_token_for_error_reporting);
                    // execute_echoc_function handles freeing args elements after copying them.
                }
            } else {
                char err_msg[300];
                snprintf(err_msg, sizeof(err_msg), "Undefined function '%s'", func_name_str);
                for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
                report_error("Runtime", err_msg, func_name_token_for_error_reporting);
                result = create_null_value(); // Should not be reached
            }
        }
    }
    return result;
}

Value interpret_instance_creation(Interpreter* interpreter, Blueprint* bp_to_instantiate, Token* call_site_token) {
    // This function is called when TOKEN_LPAREN is the current token, after the blueprint ID.
    interpreter_eat(interpreter, TOKEN_LPAREN); // Consume '(' before parsing arguments
    // So, we directly parse arguments here.

    Value args[10];
    int arg_count = 0;
    bool arg_is_fresh[10] = {false};
    parse_call_arguments(interpreter, args, &arg_count, arg_is_fresh, 10, call_site_token);

    Object* new_obj = malloc(sizeof(Object));
    if (!new_obj) report_error("System", "Failed to allocate memory for new object.", call_site_token);
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
            report_error("Runtime", "'init' method cannot be 'async'.", call_site_token);
            // Free args if they were fresh
            for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]);
        } else {
            Value init_result = execute_echoc_function(interpreter, init_method, new_obj, args, arg_is_fresh, arg_count, call_site_token);
            // Result of init is usually ignored (like Python's __init__ returning None implicitly)
            // but we must free it if it's not VAL_NULL or simple type.
            // call_site_token is owned by the caller and should not be freed here.
            if (init_result.type != VAL_NULL) {
                // Could check if init is allowed to return non-null. For now, just free.
                DEBUG_PRINTF("Init method for %s returned non-null. Discarding.", bp_to_instantiate->name);
            }
            free_value_contents(init_result);
        }
    } else {
        // No explicit init. Check if any arguments were passed.
        if (arg_count > 0) { // Arguments were parsed by parse_call_arguments
            for(int i=0; i<arg_count; ++i) if(arg_is_fresh[i]) free_value_contents(args[i]); // Free unused args
            report_error("Runtime", "Blueprint has no 'init' method but arguments were provided for instantiation.", call_site_token);
        }
        // RPAREN was already consumed by parse_call_arguments
    }
    return instance_val;
}

Value lookup_dot_attribute(Interpreter* interpreter, Value result, Token* dot_token, bool result_is_freshly_created) {
    // Get the attribute name from the next token.
    char* attr_name = strdup(interpreter->current_token->value);
    interpreter_eat(interpreter, interpreter->current_token->type); // Consume attribute token

    if (result.type == VAL_OBJECT) {
        Object* obj = result.as.object_val;
        if (strcmp(attr_name, "blueprint") == 0) {
            // Special case: obj.blueprint returns the blueprint itself.
            Value next;
            next.type = VAL_BLUEPRINT;
            next.as.blueprint_val = obj->blueprint;
            free(attr_name);
            free_token(dot_token);
            return next;
        } else {
            // Look in instance attributes.
            Value* attr_val_ptr = symbol_table_get_local(obj->instance_attributes, attr_name);
            // If not found in instance, search the class attributes/methods.
            if (!attr_val_ptr) {
                Blueprint* current_bp = obj->blueprint;
                while (current_bp) {
                    attr_val_ptr = symbol_table_get_local(current_bp->class_attributes_and_methods, attr_name);
                    if (attr_val_ptr) break;
                    current_bp = current_bp->parent_blueprint;
                }
            }
            if (!attr_val_ptr) {
                char err_msg[150];
                sprintf(err_msg, "Object of blueprint '%s' has no attribute or method '%s'.", obj->blueprint->name, attr_name);
                free(attr_name);
                if(result_is_freshly_created) free_value_contents(result);
                report_error("Runtime", err_msg, dot_token);
                free_token(dot_token);
            }
            // If the attribute is a function, return a bound method.
            if (attr_val_ptr->type == VAL_FUNCTION) {
                BoundMethod* bm = malloc(sizeof(BoundMethod));
                if (!bm) {
                    free(attr_name);
                    free_token(dot_token);
                    report_error("System", "Failed to allocate memory for bound method.", dot_token);
                }
                bm->type = FUNC_TYPE_ECHOC;
                bm->func_ptr.echoc_function = attr_val_ptr->as.function_val;
                bm->self_value.type = VAL_OBJECT;
                bm->self_value.as.object_val = obj;
                bm->self_is_owned_copy = result_is_freshly_created ? 1 : 0;
                Value bound_method_val;
                bound_method_val.type = VAL_BOUND_METHOD;
                bound_method_val.as.bound_method_val = bm;
                free(attr_name);
                free_token(dot_token);
                return bound_method_val;
            } else { // Regular attribute: perform a deep copy.
                Value next = value_deep_copy(*attr_val_ptr);
                free(attr_name);
                free_token(dot_token);
                return next;
            }
        }
    } else if (result.type == VAL_ARRAY) {
        // New branch: when the left-hand side is an array.
        if (strcmp(attr_name, "append") == 0) {
            BoundMethod* bm = malloc(sizeof(BoundMethod));
            if (!bm) {
                free(attr_name);
                free_token(dot_token);
                report_error("System", "Failed to allocate memory for bound method.", dot_token);
            }
            bm->type = FUNC_TYPE_C_BUILTIN;
            bm->func_ptr.c_builtin = builtin_append;
            bm->self_value = result; // Bind the array value as self using the new field.
            bm->self_is_owned_copy = 0;
            Value bound_method_val;
            bound_method_val.type = VAL_BOUND_METHOD;
            bound_method_val.as.bound_method_val = bm;
            free(attr_name);
            free_token(dot_token);
            return bound_method_val;
        } else {
            char err_msg[150];
            sprintf(err_msg, "Type VAL_ARRAY has no attribute '%s'.", attr_name);
            free(attr_name);
            if(result_is_freshly_created) free_value_contents(result);
            report_error("Runtime", err_msg, dot_token);
            free_token(dot_token);
        }
    } else if (result.type == VAL_DICT) {
        Dictionary* dict = result.as.dict_val;
        Value val_from_dict;
        if (dictionary_try_get(dict, attr_name, &val_from_dict, true)) {
            free(attr_name);
            free_token(dot_token);
            return val_from_dict; // dictionary_try_get returns a deep copy
        } else {
            char err_msg[150];
            sprintf(err_msg, "Key '%s' not found in dictionary.", attr_name);
            free(attr_name);
            if(result_is_freshly_created) free_value_contents(result);
            report_error("Runtime", err_msg, dot_token);
            free_token(dot_token);
        }
    } else if (result.type == VAL_SUPER_PROXY) {
        Object* self_obj_for_super = interpreter->current_self_object;
        if (!self_obj_for_super || !self_obj_for_super->blueprint->parent_blueprint) {
            free(attr_name);
            if(result_is_freshly_created) free_value_contents(result);
            report_error("Runtime", "'super' used incorrectly or in a class with no parent.", dot_token);
            free_token(dot_token);
        }
        Value* parent_method_ptr = symbol_table_get_local(self_obj_for_super->blueprint->parent_blueprint->class_attributes_and_methods, attr_name);
        if (!parent_method_ptr || parent_method_ptr->type != VAL_FUNCTION) {
            char err_msg[150];
            sprintf(err_msg, "Parent blueprint of '%s' does not have method '%s' or it's not a function.", self_obj_for_super->blueprint->name, attr_name);
            free(attr_name);
            if(result_is_freshly_created) free_value_contents(result);
            report_error("Runtime", err_msg, dot_token);
            free_token(dot_token);
        }
        BoundMethod* bm = malloc(sizeof(BoundMethod));
        if (!bm) {
            free(attr_name);
            free_token(dot_token);
            report_error("System", "Failed to allocate memory for bound method.", dot_token);
        }
        bm->type = FUNC_TYPE_ECHOC;
        bm->func_ptr.echoc_function = parent_method_ptr->as.function_val;
        bm->self_value.type = VAL_OBJECT; // Assign type
        bm->self_value.as.object_val = self_obj_for_super;
        bm->self_is_owned_copy = 0;
        Value bound_method_val;
        bound_method_val.type = VAL_BOUND_METHOD;
        bound_method_val.as.bound_method_val = bm;
        free(attr_name);
        free_token(dot_token);
        return bound_method_val;
    } else {
        char err_msg[100];
        sprintf(err_msg, "Cannot access attribute '%s' on non-object/blueprint/super_proxy type (got type %d).", attr_name, result.type);
        free(attr_name);
        if(result_is_freshly_created) free_value_contents(result);
        report_error("Runtime", err_msg, dot_token);
        free_token(dot_token);
    }
    return create_null_value();
}