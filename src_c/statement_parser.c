// src_c/statement_parser.c
#include "statement_parser.h"
#include "expression_parser.h" // For interpret_ternary_expr
#include "parser_utils.h"      // For interpreter_eat, token_type_to_string
#include "scope.h"             // For enter_scope, exit_scope, symbol_table_set, symbol_table_get
#include "value_utils.h"       // For value_to_string_representation, free_value_contents
#include "module_loader.h"     // For module loading functions
#include "dictionary.h"        // For dictionary_set

#include <stdio.h>  // For printf, sprintf
#include <string.h> // For strdup, strcmp
#include <stdlib.h> // For free
#include <math.h>   // For fmod in for loop
#include "interpreter.h" // For Coroutine struct and other interpreter specifics if needed by interpret_coroutine_body

// Forward declarations for static functions within this file
static StatementExecStatus interpret_let_statement(Interpreter* interpreter);
static StatementExecStatus interpret_if_statement(Interpreter* interpreter);
static void skip_statements_in_branch(Interpreter* interpreter, int start_col);
static StatementExecStatus interpret_loop_statement(Interpreter* interpreter);
static StatementExecStatus interpret_for_loop(Interpreter* interpreter, int loop_col, int loop_line, Token* loop_keyword_token_for_context);
static void interpret_funct_statement(Interpreter* interpreter, int statement_start_col, bool is_async_param);
static void interpret_skip_statement(Interpreter* interpreter);
static void interpret_break_statement(Interpreter* interpreter);
static bool is_builtin_module(const char* module_name);
static void interpret_continue_statement(Interpreter* interpreter);
static StatementExecStatus interpret_return_statement(Interpreter* interpreter); // Kept as StatementExecStatus
static StatementExecStatus interpret_expression_statement(Interpreter* interpreter); // Changed to StatementExecStatus
static void interpret_raise_statement(Interpreter* interpreter); // New
static void skip_all_elif_and_else_branches(Interpreter* interpreter, int if_col); // New forward declaration
static StatementExecStatus interpret_try_statement(Interpreter* interpreter);   // MODIFIED: Return a status
static void interpret_load_statement(Interpreter* interpreter);
static void interpret_blueprint_statement(Interpreter* interpreter);

static StatementExecStatus interpret_block_statement(Interpreter* interpreter);
static StatementExecStatus execute_statements_in_controlled_block(Interpreter* interpreter, int start_col, const char* block_type_for_error, TokenType terminator1, TokenType terminator2, TokenType terminator3);
static StatementExecStatus execute_loop_body_iteration(Interpreter* interpreter, int loop_start_col, int expected_body_indent_col, const char* loop_type_for_error);

extern void add_to_ready_queue(Interpreter* interpreter, Coroutine* coro); 
extern void run_event_loop(Interpreter* interpreter); 
// Forward declaration for a function from dictionary.c that is not in the header yet.
extern Value* dictionary_try_get_value_ptr(Dictionary* dict, const char* key_str);
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
    if (interpreter->current_token->type == TOKEN_ASYNC) { 
        Token* async_token_ref = interpreter->current_token;
        int start_col_for_async_funct = async_token_ref->col;
        interpreter_eat(interpreter, TOKEN_ASYNC);
        if (interpreter->current_token->type != TOKEN_FUNCT) report_error_unexpected_token(interpreter, "'funct' after 'async'");
        interpret_funct_statement(interpreter, start_col_for_async_funct, true);
    } else if (interpreter->current_token->type == TOKEN_FUNCT) {
        interpret_funct_statement(interpreter, interpreter->current_token->col, false);
    } else if (interpreter->current_token->type == TOKEN_RETURN) {
        status = interpret_return_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_LET) {
        if ((status = interpret_let_statement(interpreter)) == STATEMENT_YIELDED_AWAIT) {
            return status;
        }
    } else if (interpreter->current_token->type == TOKEN_IF) {
        status = interpret_if_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_LOOP) {
        status = interpret_loop_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_BREAK) {
        interpret_break_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_SKIP) {
        interpret_skip_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_CONTINUE) {
        interpret_continue_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_LBRACE) {
        status = interpret_block_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_RAISE) {
        interpret_raise_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_TRY) {
        status = interpret_try_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_BLUEPRINT) {
        interpret_blueprint_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_LOAD) {
        interpret_load_statement(interpreter);
    } else if (interpreter->current_token->type == TOKEN_ID ||
               interpreter->current_token->type == TOKEN_SUPER ||
               interpreter->current_token->type == TOKEN_AWAIT) {
        status = interpret_expression_statement(interpreter);
        return status;
    } else if (interpreter->current_token->type == TOKEN_EOF) {
        return status; // End of file, do nothing
    } else if (interpreter->current_token->type == TOKEN_ELIF) {
		// --- START: ADD THIS TEMPORARY DEBUG BLOCK ---
		DEBUG_PRINTF("ELIF_ERROR_DEBUG: Encountered ELIF outside of an if-statement context.%s", "");
		DEBUG_PRINTF("  Current Coroutine: %s (%p), State: %d",
					 interpreter->current_executing_coroutine ? (interpreter->current_executing_coroutine->name ? interpreter->current_executing_coroutine->name : "unnamed") : "None",
					 (void*)interpreter->current_executing_coroutine,
					 interpreter->current_executing_coroutine ? interpreter->current_executing_coroutine->state : (CoroutineState)-1);
		DEBUG_PRINTF("  Current Scope: %p (ID: %llu), Outer: %p",
					 (void*)interpreter->current_scope,
					 interpreter->current_scope ? interpreter->current_scope->id : (uint64_t)-1,
					 interpreter->current_scope ? (void*)interpreter->current_scope->outer : NULL);
		DEBUG_PRINTF("  Function Nesting Level: %d, Loop Depth: %d, Try/Catch Stack Top: %p", interpreter->function_nesting_level, interpreter->loop_depth, (void*)interpreter->try_catch_stack_top);
		// --- END: ADD THIS TEMPORARY DEBUG BLOCK ---
        report_error("Syntax", "'elif:' without a preceding 'if:' or 'elif:'.", interpreter->current_token);
    } else if (interpreter->current_token->type == TOKEN_ELSE) {
        report_error("Syntax", "'else:' without a preceding 'if:'.", interpreter->current_token);
    } else {
        report_error_unexpected_token(interpreter, "a statement keyword, an identifier (for a function call), 'await', or an opening brace '{'");
    }

    if (interpreter->break_flag || interpreter->continue_flag || interpreter->return_flag || interpreter->exception_is_active) {
        if (status == STATEMENT_EXECUTED_OK) status = STATEMENT_PROPAGATE_FLAG;
    }

    return status;
}

static StatementExecStatus interpret_expression_statement(Interpreter* interpreter) {
    DEBUG_PRINTF("INTERPRET_EXPRESSION_STATEMENT: Token type: %s, value: '%s'",
                 token_type_to_string(interpreter->current_token->type),
                 interpreter->current_token->value ? interpreter->current_token->value : "N/A");
    Token* first_token_in_expr = token_deep_copy(interpreter->current_token);

    // This function is called when interpreter->current_token is TOKEN_ID.
    // We expect this to be the start of an expression, typically a function call,
    // used as a statement.
    // The expression parser will handle parsing the function call.    
    // Changed from interpret_ternary_expr to interpret_expression
    ExprResult expr_res = interpret_expression(interpreter);

    Value result = expr_res.value;

    if (interpreter->exception_is_active) { // If expression evaluation raised an exception
        // If fresh, free it. If not, it's owned elsewhere.
        if (expr_res.is_freshly_created_container) free_value_contents(result);
        free_token(first_token_in_expr);
        return STATEMENT_PROPAGATE_FLAG; // Propagate exception
    }

    // If not resuming, and the result of a standalone expression statement is a coroutine,
    // print its representation. This avoids re-printing during a resume.
    if ((!interpreter->current_executing_coroutine || interpreter->current_executing_coroutine->state != CORO_RESUMING) &&
        (result.type == VAL_COROUTINE || result.type == VAL_GATHER_TASK))
    {
        char* str_repr = value_to_string_representation(result, interpreter, first_token_in_expr);
        debug_aware_printf("%s\n", str_repr);
        fflush(stdout); // Ensure it prints before any potential warning on stderr
        #ifdef DEBUG_ECHOC
        if (echoc_debug_log_file) {
            fflush(echoc_debug_log_file);
        }
        #endif
        free(str_repr);
    }

    DEBUG_PRINTF("      INTERPRET_STATEMENT (near show): Coro: %p, IsActive: %d, Scope: %p, Scope Head: %s",
            (void*)interpreter->current_executing_coroutine, interpreter->exception_is_active,
            (void*)interpreter->current_scope, interpreter->current_scope->symbols ? interpreter->current_scope->symbols->name : "NULL");
        
        print_scope_contents(interpreter->current_scope);
            
    // The result of an expression statement is discarded.
    if (expr_res.is_freshly_created_container && !(interpreter->exception_is_active)) {
        free_value_contents(result);
    }

    free_token(first_token_in_expr);

    // An expression statement must be terminated by a colon.
    interpreter_eat(interpreter, TOKEN_COLON);
    if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
        return STATEMENT_YIELDED_AWAIT;
    }
    return STATEMENT_EXECUTED_OK;
}
// Helper function to perform the actual indexed assignment
// target_container: The direct container (array or dict) to modify
// final_index: The index/key to use on target_container
// value_to_set: The value to assign (this function will deep_copy it if needed)
// error_token: For error reporting context
// base_var_name: Name of the original variable for error messages
void perform_indexed_assignment(Value* target_container, Value final_index, Value value_to_set, Token* error_token, const char* base_var_name) {
    if (g_interpreter_for_error_reporting->prevent_side_effects) return; // Do not perform assignment during a fast-forward resume

    if (target_container->type == VAL_ARRAY) {
        if (final_index.type != VAL_INT) {
            g_interpreter_for_error_reporting->exception_is_active = 1;
            free_value_contents(g_interpreter_for_error_reporting->current_exception);
            g_interpreter_for_error_reporting->current_exception.type = VAL_STRING;
            g_interpreter_for_error_reporting->current_exception.as.string_val = strdup("Array index for assignment must be an integer.");
            if (g_interpreter_for_error_reporting->error_token) free_token(g_interpreter_for_error_reporting->error_token);
            g_interpreter_for_error_reporting->error_token = token_deep_copy(error_token);
            // Cleanup values that were passed in and would normally be freed by caller if no error
            free_value_contents(final_index); free_value_contents(value_to_set);
            return;
        }
        Array* arr = target_container->as.array_val;
        long idx = final_index.as.integer;
        long effective_idx = idx;
        if (effective_idx < 0) effective_idx += arr->count;

        if (effective_idx < 0 || effective_idx >= arr->count) {
            char err_msg[150];
            snprintf(err_msg, sizeof(err_msg), "Array assignment index %ld out of bounds for array '%s' (size %d).", idx, base_var_name, arr->count);
            g_interpreter_for_error_reporting->exception_is_active = 1;
            free_value_contents(g_interpreter_for_error_reporting->current_exception);
            g_interpreter_for_error_reporting->current_exception.type = VAL_STRING;
            g_interpreter_for_error_reporting->current_exception.as.string_val = strdup(err_msg);
            if (g_interpreter_for_error_reporting->error_token) free_token(g_interpreter_for_error_reporting->error_token);
            g_interpreter_for_error_reporting->error_token = token_deep_copy(error_token);
            // Cleanup values that were passed in and would normally be freed by caller if no error
            free_value_contents(final_index); free_value_contents(value_to_set);
            return;
        }
        free_value_contents(arr->elements[effective_idx]); // Free old element
        arr->elements[effective_idx] = value_deep_copy(value_to_set);
    } else if (target_container->type == VAL_DICT) {
        if (final_index.type != VAL_STRING) {
            g_interpreter_for_error_reporting->exception_is_active = 1;
            free_value_contents(g_interpreter_for_error_reporting->current_exception);
            g_interpreter_for_error_reporting->current_exception.type = VAL_STRING;
            g_interpreter_for_error_reporting->current_exception.as.string_val = strdup("Dictionary key for assignment must be a string.");
            if (g_interpreter_for_error_reporting->error_token) free_token(g_interpreter_for_error_reporting->error_token);
            g_interpreter_for_error_reporting->error_token = token_deep_copy(error_token);
            free_value_contents(final_index); free_value_contents(value_to_set);
            return;
        }
        dictionary_set(target_container->as.dict_val, final_index.as.string_val, value_to_set, error_token);
    } else if (target_container->type == VAL_TUPLE) {
        g_interpreter_for_error_reporting->exception_is_active = 1;
        free_value_contents(g_interpreter_for_error_reporting->current_exception);
        g_interpreter_for_error_reporting->current_exception.type = VAL_STRING;
        g_interpreter_for_error_reporting->current_exception.as.string_val = strdup("Tuples are immutable and cannot be modified.");
        if (g_interpreter_for_error_reporting->error_token) free_token(g_interpreter_for_error_reporting->error_token);
        g_interpreter_for_error_reporting->error_token = token_deep_copy(error_token);
        free_value_contents(final_index); free_value_contents(value_to_set);
        return;
    } else {
        char err_msg[150];
        snprintf(err_msg, sizeof(err_msg), "Cannot apply indexed assignment to variable '%s' of type %d.", base_var_name, target_container->type);
        g_interpreter_for_error_reporting->exception_is_active = 1;
        free_value_contents(g_interpreter_for_error_reporting->current_exception);
        g_interpreter_for_error_reporting->current_exception.type = VAL_STRING;
        g_interpreter_for_error_reporting->current_exception.as.string_val = strdup(err_msg);
        if (g_interpreter_for_error_reporting->error_token) free_token(g_interpreter_for_error_reporting->error_token);
        g_interpreter_for_error_reporting->error_token = token_deep_copy(error_token);
        free_value_contents(final_index); free_value_contents(value_to_set);
        return;
    }
    free_value_contents(final_index); // final_index is consumed
    // value_to_set is consumed by deep_copy or dictionary_set which makes its own copy
}

static StatementExecStatus interpret_let_statement(Interpreter* interpreter) {
    DEBUG_PRINTF("INTERPRET_LET_STMT: Entering. Current token: %s ('%s')",
                 token_type_to_string(interpreter->current_token->type),
                 interpreter->current_token->value ? interpreter->current_token->value : "N/A");
    fflush(stderr); // Ensure this prints immediately
    StatementExecStatus status = STATEMENT_EXECUTED_OK;
    interpreter_eat(interpreter, TOKEN_LET);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'let'
    
    // Deep copy the token that represents the variable/attribute being assigned to.
    // This copy will be used for error reporting if needed later in the statement,
    // as the original token might be consumed by interpreter_eat.
    Token* target_name_token_for_error = token_deep_copy(interpreter->current_token);
    if (target_name_token_for_error->type != TOKEN_ID) {
        // If target_name_token_for_error itself is not an ID, it's an immediate syntax error.
        // No var_name_str would have been allocated yet.
        report_error("Syntax", "Expected variable name after 'let:'", target_name_token_for_error);
        free_token(target_name_token_for_error); // Free the copy before exiting
    }
    char* var_name_str = strdup(target_name_token_for_error->value);
    interpreter_eat(interpreter, TOKEN_ID);

    DEBUG_PRINTF("LET_STMT: Variable name: '%s'. Current token before assignment part: %s ('%s')",
                 var_name_str,
                 token_type_to_string(interpreter->current_token->type),
                 interpreter->current_token->value ? interpreter->current_token->value : "N/A");
    // Handle 'let: self.attribute = value'
    if (strcmp(var_name_str, "self") == 0 && interpreter->current_token->type == TOKEN_DOT) { // Starts with self.
        if (!interpreter->current_self_object) {
            free(var_name_str); free_token(target_name_token_for_error);
            report_error("Runtime", "'self' can only be used within an instance method.", target_name_token_for_error); 
        }
        interpreter_eat(interpreter, TOKEN_DOT); // Eat '.'
        Token* attr_name_token = interpreter->current_token;
        if (attr_name_token->type != TOKEN_ID) {
            // attr_name_str not yet allocated
            free(var_name_str); free_token(target_name_token_for_error);
            report_error("Syntax", "Expected attribute name after 'self.'.", attr_name_token);
        } // attr_name_token is consumed by strdup or eat
        char* attr_name_str = strdup(attr_name_token->value);
        interpreter_eat(interpreter, TOKEN_ID); // Eat attribute name

        DEBUG_PRINTF("LET_STMT (self): Attribute name: '%s'. Current token: %s ('%s')",
                     attr_name_str,
                     token_type_to_string(interpreter->current_token->type),
                     interpreter->current_token->value ? interpreter->current_token->value : "N/A");

        if (interpreter->current_token->type == TOKEN_LBRACKET) { // self.attribute[index] = value
            Object* self_obj = interpreter->current_self_object;
            Value* base_container_val_ptr = symbol_table_get_local(self_obj->instance_attributes, attr_name_str);
            if (!base_container_val_ptr) {
                // Could also check blueprint attributes if self.CLASS_ATTR[idx] was allowed (not currently supported this way)
                char err_msg[200]; sprintf(err_msg, "Attribute '%s' not found on 'self' for indexed assignment.", attr_name_str); 
                free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error); 
                report_error("Runtime", err_msg, target_name_token_for_error); // Use target_name_token_for_error as attr_name_token might be invalid
            }

            Value* container_to_modify = base_container_val_ptr;
            Value* parent_container_for_final_assignment = NULL;

            Value final_index_for_assignment;
            bool final_index_is_fresh = false;

            while (interpreter->current_token->type == TOKEN_LBRACKET) {
                parent_container_for_final_assignment = container_to_modify;

                interpreter_eat(interpreter, TOKEN_LBRACKET);
                ExprResult index_res = interpret_expression(interpreter);
                Value current_loop_index = index_res.value;
                bool current_loop_index_is_fresh = index_res.is_freshly_created_container;

                if (interpreter->exception_is_active) {
                    if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                    free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                    return STATEMENT_PROPAGATE_FLAG;
                } 
                if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
                    if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                    free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                    return STATEMENT_YIELDED_AWAIT;
                }
                interpreter_eat(interpreter, TOKEN_RBRACKET);

                if (final_index_is_fresh) free_value_contents(final_index_for_assignment); // Free previous loop's final_index if it was fresh
                final_index_for_assignment = current_loop_index; // This becomes the final index if loop breaks
                final_index_is_fresh = current_loop_index_is_fresh;

                if (interpreter->current_token->type == TOKEN_ASSIGN) {
                    break; // End of LHS
                }

                // Descend
                if (parent_container_for_final_assignment->type == VAL_ARRAY) {
                    if (current_loop_index.type != VAL_INT) {
                        if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                        free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                        report_error("Runtime", "Array index must be an integer.", target_name_token_for_error);
                    }
                    long idx = current_loop_index.as.integer;
                    long effective_idx = idx >= 0 ? idx : parent_container_for_final_assignment->as.array_val->count + idx;

                    if (effective_idx < 0 || effective_idx >= parent_container_for_final_assignment->as.array_val->count) {
                        char err_msg[150];
                        sprintf(err_msg, "Array index %ld out of bounds for array attribute '%s' (size %d).", idx, attr_name_str, parent_container_for_final_assignment->as.array_val->count);
                        if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                        free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                        report_error("Runtime", err_msg, target_name_token_for_error);
                    }
                    container_to_modify = &parent_container_for_final_assignment->as.array_val->elements[effective_idx];
                    if (current_loop_index_is_fresh) free_value_contents(current_loop_index); // Traversal index freed
                    final_index_is_fresh = false; // Mark as consumed for next iteration or if loop ends
                } else if (parent_container_for_final_assignment->type == VAL_DICT) {
                    if (current_loop_index.type != VAL_STRING) {
                        if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                        free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                        report_error("Runtime", "Dictionary key must be a string.", target_name_token_for_error);
                    }
                    
                    Value* next_container_ptr = dictionary_try_get_value_ptr(parent_container_for_final_assignment->as.dict_val, current_loop_index.as.string_val);
                    if (!next_container_ptr) {
                        char err_msg[200];
                        sprintf(err_msg, "Key '%s' not found in dictionary attribute '%s' during chained assignment.", current_loop_index.as.string_val, attr_name_str);
                        if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                        free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                        report_error("Runtime", err_msg, target_name_token_for_error);
                    }
                    container_to_modify = next_container_ptr; // This is a pointer to the Value inside the dictionary.
                    if (current_loop_index_is_fresh) free_value_contents(current_loop_index); // Traversal index freed
                    final_index_is_fresh = false; // Mark as consumed
                } else {
                    // This is the new final else block for all other unsupported types.
                    if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                    free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                    report_error("Runtime", "Chained indexed assignment is only supported for nested arrays and dictionaries.", target_name_token_for_error);
                }
            }

            interpreter_eat(interpreter, TOKEN_ASSIGN);
            DEBUG_PRINTF("LET_STMT (self.attr[...]): About to parse RHS. Current token: %s ('%s')",
                         token_type_to_string(interpreter->current_token->type),
                         interpreter->current_token->value ? interpreter->current_token->value : "N/A");
            ExprResult rhs_res = interpret_expression(interpreter);
            Value val_to_set = rhs_res.value;
            
            if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
                DEBUG_PRINTF("LET_STMT (self.attr[...]): RHS yielded. var_name='%s', attr_name='%s'. Pending.", var_name_str, attr_name_str);
                status = STATEMENT_YIELDED_AWAIT;
                if(final_index_is_fresh) free_value_contents(final_index_for_assignment);
                if(rhs_res.is_freshly_created_container) free_value_contents(val_to_set); // RHS value will be re-evaluated
                free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                return status;
            }
            if (interpreter->exception_is_active) {
                if(final_index_is_fresh) free_value_contents(final_index_for_assignment);
                if(rhs_res.is_freshly_created_container) free_value_contents(val_to_set);
                free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                return STATEMENT_PROPAGATE_FLAG;
            }

            if (interpreter->is_dummy_resume_value) {
                interpreter->is_dummy_resume_value = false; // Consume flag
                if (final_index_is_fresh) free_value_contents(final_index_for_assignment); // Clean up index
            } else {
                if (!interpreter->prevent_side_effects) {
                    perform_indexed_assignment(parent_container_for_final_assignment, final_index_for_assignment, val_to_set, target_name_token_for_error, attr_name_str);
                } else if (final_index_is_fresh) {
                    // If skipping assignment, we must still free the fresh index value.
                    free_value_contents(final_index_for_assignment);
                }
            }

            // Always free the RHS value if it was a temporary, as it was either consumed by assignment or unused.
            if (rhs_res.is_freshly_created_container) free_value_contents(val_to_set); // perform_indexed_assignment copies or consumes
        } else if (interpreter->current_token->type == TOKEN_ASSIGN) { // self.attribute = value
            interpreter_eat(interpreter, TOKEN_ASSIGN); 
            DEBUG_PRINTF("LET_STMT (self.attr): About to parse RHS. Current token: %s ('%s')",
                         token_type_to_string(interpreter->current_token->type),
                         interpreter->current_token->value ? interpreter->current_token->value : "N/A");
            ExprResult val_expr_res = interpret_expression(interpreter);
            Value val_to_assign = val_expr_res.value;
            if (interpreter->exception_is_active) {
                free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error); if(val_expr_res.is_freshly_created_container) free_value_contents(val_to_assign);
                return STATEMENT_PROPAGATE_FLAG;
            }
            if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
                DEBUG_PRINTF("LET_STMT (self.attr): RHS yielded for await. var_name='%s', attr_name='%s'. Pending assignment.",
                             var_name_str, attr_name_str); // Log action
                status = STATEMENT_YIELDED_AWAIT;
                if(val_expr_res.is_freshly_created_container) free_value_contents(val_to_assign); // RHS value will be re-evaluated
                free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
                return status;
            } else if (interpreter->is_dummy_resume_value) {
                interpreter->is_dummy_resume_value = false; // Consume flag
                if (val_expr_res.is_freshly_created_container) free_value_contents(val_to_assign);
            } else {
                if (!interpreter->prevent_side_effects) {
                    DEBUG_PRINTF("LET_STMT (self.attr): Assigning to self attribute '%s'. Value type: %d", attr_name_str, val_to_assign.type);
                    symbol_table_define(interpreter->current_self_object->instance_attributes, attr_name_str, val_to_assign);
                }
                if (val_expr_res.is_freshly_created_container) free_value_contents(val_to_assign); // symbol_table_set made a deep copy
            }
        } else {
            free(var_name_str); free(attr_name_str); free_token(target_name_token_for_error);
            report_error_unexpected_token(interpreter, "'[' for indexed assignment or '=' for attribute assignment after 'self.attribute'");
        }
        free(attr_name_str); free(var_name_str);
        free_token(target_name_token_for_error); // Free the copied token
        interpreter_eat(interpreter, TOKEN_COLON); 
        if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
            return STATEMENT_YIELDED_AWAIT;
        }
        return STATEMENT_EXECUTED_OK;
    }
    Value* current_val_ptr = symbol_table_get(interpreter->current_scope, var_name_str);

    // This block replaces the old logic for indexed assignment, including the problematic `while` loop.
    if (interpreter->current_token->type == TOKEN_LBRACKET) { // Indexed assignment var[idx1][idx2]... = value
        if (!current_val_ptr) {
            char err_msg[150];
            sprintf(err_msg, "Variable '%s' must be an existing collection for indexed assignment with 'let:'.", var_name_str);
            free(var_name_str); free_token(target_name_token_for_error);
            report_error("Runtime", err_msg, target_name_token_for_error);
        }

        Value* container_to_modify = current_val_ptr;
        Value* parent_container_for_final_assignment = NULL;

        Value final_index_for_assignment;
        bool final_index_is_fresh = false;

        while (interpreter->current_token->type == TOKEN_LBRACKET) {
            parent_container_for_final_assignment = container_to_modify;

            interpreter_eat(interpreter, TOKEN_LBRACKET);
            ExprResult index_res = interpret_expression(interpreter);
            Value current_loop_index = index_res.value;
            bool current_loop_index_is_fresh = index_res.is_freshly_created_container;

            if (interpreter->exception_is_active) {
                if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                free(var_name_str); free_token(target_name_token_for_error);
                return STATEMENT_PROPAGATE_FLAG;
            } 
            if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
                if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                free(var_name_str); free_token(target_name_token_for_error);
                return STATEMENT_YIELDED_AWAIT;
            }
            interpreter_eat(interpreter, TOKEN_RBRACKET);

            if (final_index_is_fresh) free_value_contents(final_index_for_assignment);
            final_index_for_assignment = current_loop_index;
            final_index_is_fresh = current_loop_index_is_fresh;

            if (interpreter->current_token->type == TOKEN_ASSIGN) {
                break; // End of LHS
            }

            // Descend
            if (parent_container_for_final_assignment->type == VAL_ARRAY) {
                if (current_loop_index.type != VAL_INT) {
                    if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                    free(var_name_str); free_token(target_name_token_for_error);
                    report_error("Runtime", "Array index must be an integer.", target_name_token_for_error);
                }
                long idx = current_loop_index.as.integer;
                long effective_idx = idx >= 0 ? idx : parent_container_for_final_assignment->as.array_val->count + idx;

                if (effective_idx < 0 || effective_idx >= parent_container_for_final_assignment->as.array_val->count) {
                    char err_msg[150];
                    snprintf(err_msg, sizeof(err_msg), "Array index %ld out of bounds for array '%s' (size %d).", idx, var_name_str, parent_container_for_final_assignment->as.array_val->count);
                    if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                    free(var_name_str); free_token(target_name_token_for_error);
                    report_error("Runtime", err_msg, target_name_token_for_error);
                }
                container_to_modify = &parent_container_for_final_assignment->as.array_val->elements[effective_idx];
                if (current_loop_index_is_fresh) free_value_contents(current_loop_index); // Traversal index freed
                final_index_is_fresh = false; 
            } else if (parent_container_for_final_assignment->type == VAL_DICT) {
                if (current_loop_index.type != VAL_STRING) {
                    if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                    free(var_name_str); free_token(target_name_token_for_error);
                    report_error("Runtime", "Dictionary key must be a string.", target_name_token_for_error);
                }
                
                Value* next_container_ptr = dictionary_try_get_value_ptr(parent_container_for_final_assignment->as.dict_val, current_loop_index.as.string_val);
                if (!next_container_ptr) {
                    char err_msg[200];
                    sprintf(err_msg, "Key '%s' not found in dictionary variable '%s' during chained assignment.", current_loop_index.as.string_val, var_name_str);
                    if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                    free(var_name_str); free_token(target_name_token_for_error);
                    report_error("Runtime", err_msg, target_name_token_for_error);
                }
                container_to_modify = next_container_ptr;
                if (current_loop_index_is_fresh) free_value_contents(current_loop_index);
                final_index_is_fresh = false;
            } else {
                if(current_loop_index_is_fresh) free_value_contents(current_loop_index);
                free(var_name_str); free_token(target_name_token_for_error);
                report_error("Runtime", "Chained indexed assignment is only supported for nested arrays and dictionaries.", target_name_token_for_error);
            }
        }

        interpreter_eat(interpreter, TOKEN_ASSIGN);
        ExprResult new_val_res = interpret_expression(interpreter);
        Value new_value_to_assign = new_val_res.value;

        if (interpreter->exception_is_active) {
            if(final_index_is_fresh) free_value_contents(final_index_for_assignment);
            if(new_val_res.is_freshly_created_container) free_value_contents(new_value_to_assign);
            free(var_name_str); free_token(target_name_token_for_error);
            return STATEMENT_PROPAGATE_FLAG;
        } 
        if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
            DEBUG_PRINTF("LET_STMT (indexed): RHS yielded for await. var_name='%s'. Pending assignment.", var_name_str);
            status = STATEMENT_YIELDED_AWAIT;
            if(final_index_is_fresh) free_value_contents(final_index_for_assignment);
            if(new_val_res.is_freshly_created_container) free_value_contents(new_value_to_assign); // RHS value will be re-evaluated
            free(var_name_str);
            free_token(target_name_token_for_error);
            return status;
        }
        if (interpreter->is_dummy_resume_value) {
            interpreter->is_dummy_resume_value = false; // Consume flag
            if (final_index_is_fresh) free_value_contents(final_index_for_assignment);
        } else {
            if (!interpreter->prevent_side_effects) {
                perform_indexed_assignment(parent_container_for_final_assignment, final_index_for_assignment, new_value_to_assign, target_name_token_for_error, var_name_str);
            } else if (final_index_is_fresh) {
                // If skipping assignment, we must still free the fresh index value.
                free_value_contents(final_index_for_assignment);
            }
        }
        // Always free the RHS value if it was temporary
        if (new_val_res.is_freshly_created_container) free_value_contents(new_value_to_assign);
    } else if (interpreter->current_token->type == TOKEN_ASSIGN) { // Simple assignment
        interpreter_eat(interpreter, TOKEN_ASSIGN);

        DEBUG_PRINTF("LET_STMT_BEFORE_EXPR: Current token before interpret_expression: %s ('%s')",
                     token_type_to_string(interpreter->current_token->type),
                     interpreter->current_token->value ? interpreter->current_token->value : "N/A");

        ExprResult val_expr_res = interpret_expression(interpreter);
        // Changed from interpret_ternary_expr to interpret_expression
        Value val_to_assign = val_expr_res.value;
        DEBUG_PRINTF("LET_STMT (simple) AFTER_EXPR_VAL_ASSIGN: var_name='%s', coro_state=%d, val_type=%d, val_is_fresh=%d",
                     var_name_str, interpreter->current_executing_coroutine ? (int)interpreter->current_executing_coroutine->state : -1,
                     val_to_assign.type, val_expr_res.is_freshly_created_container);
        DEBUG_PRINTF("LET_STMT (simple) AFTER_EXPR: Current token after interpret_expression: %s ('%s')",
                    token_type_to_string(interpreter->current_token->type),
                    interpreter->current_token->value ? interpreter->current_token->value : "N/A");
        
        char* val_to_assign_str = value_to_string_representation(val_to_assign, interpreter, target_name_token_for_error);
        DEBUG_PRINTF("LET_STMT (simple) AFTER_EXPR: val_to_assign (type %d, fresh: %d): %s. coro_state: %d",
                     val_to_assign.type, val_expr_res.is_freshly_created_container, val_to_assign_str ? val_to_assign_str : "NULL_REPR", interpreter->current_executing_coroutine ? (int)interpreter->current_executing_coroutine->state : -1);
        free(val_to_assign_str);

        if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
            // --- YIELD PATH ---
            // The expression on the RHS is yielding right now. We must suspend this statement.
            DEBUG_PRINTF("LET_STMT (simple): RHS yielded for await. var_name='%s'. Pending assignment.", var_name_str);
            status = STATEMENT_YIELDED_AWAIT;
            // The RHS value will be re-evaluated on resume, so free any temporary value now.
            if(val_expr_res.is_freshly_created_container) free_value_contents(val_to_assign);
            free(var_name_str);
            free_token(target_name_token_for_error);
            return status;
        } else {
            // --- NORMAL SYNC PATH ---
            // The expression on the RHS was synchronous and did not yield.
            if (interpreter->exception_is_active) { // Check for exception only in sync path after expr eval
                free(var_name_str); free_token(target_name_token_for_error);
                if(val_expr_res.is_freshly_created_container) free_value_contents(val_to_assign);
                return STATEMENT_PROPAGATE_FLAG;
            }
            if (interpreter->is_dummy_resume_value) {
                interpreter->is_dummy_resume_value = false; // Consume flag
                if (val_expr_res.is_freshly_created_container) free_value_contents(val_to_assign);
            } else {
                if (!interpreter->prevent_side_effects) {
                    DEBUG_PRINTF("LET_STMT (simple) SYNC/RESUME: var_name='%s'. Assigning.", var_name_str);
                    symbol_table_set(interpreter->current_scope, var_name_str, val_to_assign);
                }
                if (val_expr_res.is_freshly_created_container) {
                    free_value_contents(val_to_assign); // symbol_table_set made a deep copy
                }
            }
        }

    } else {
        char err_msg[200];
        snprintf(err_msg, sizeof(err_msg), "Expected '[' for indexed assignment or '=' for simple assignment after variable name '%s', but got %s.",
                var_name_str, token_type_to_string(interpreter->current_token->type));
        free(var_name_str); free_token(target_name_token_for_error);
        report_error("Syntax", err_msg, interpreter->current_token);
    }

    free(var_name_str);
    // Always free target_name_token_for_error as it's local to this call.
    free_token(target_name_token_for_error);
    DEBUG_PRINTF("LET_STMT_BEFORE_FINAL_COLON: Current token: %s ('%s')",
                 token_type_to_string(interpreter->current_token->type),
                 interpreter->current_token->value ? interpreter->current_token->value : "N/A");

    interpreter_eat(interpreter, TOKEN_COLON);
    return status;
}

static StatementExecStatus interpret_block_statement(Interpreter* interpreter) {
    StatementExecStatus status = STATEMENT_EXECUTED_OK;
    Token* lbrace_token = interpreter->current_token;
    int lbrace_col = lbrace_token->col;
    interpreter_eat(interpreter, TOKEN_LBRACE);
    enter_scope(interpreter);

    while (interpreter->current_token->type != TOKEN_RBRACE &&
           interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->col != lbrace_col + 4) {
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg),
                     "Statement in block '{...}' has incorrect indentation. Expected column %d, got column %d.",
                     lbrace_col + 4, interpreter->current_token->col);
            report_error("Syntax", err_msg, interpreter->current_token);
        }
        status = interpret_statement(interpreter);
        if (status != STATEMENT_EXECUTED_OK) {
            // If a statement yields or propagates, exit scope and return status
            exit_scope(interpreter);
            return status;
        }
    }

    if (interpreter->current_token->type == TOKEN_RBRACE) {
        Token* rbrace_token = interpreter->current_token;
        if (rbrace_token->col != lbrace_col) {
            char err_msg[250];
            snprintf(err_msg, sizeof(err_msg),
                     "'}' (closing brace at column %d) is not aligned with the opening '{' (at column %d).",
                     rbrace_token->col, lbrace_col);
            report_error("Syntax", err_msg, rbrace_token);
        }
        interpreter_eat(interpreter, TOKEN_RBRACE);
    } else {
        report_error("Syntax", "Expected '}' to close block", interpreter->current_token);
    }
    exit_scope(interpreter);
    return status; // Should be STATEMENT_EXECUTED_OK if reached here
}

// New implementation for skip_statements_in_branch
static void skip_statements_in_branch(Interpreter* interpreter, int start_col) {

    while (interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->col <= start_col) {
            return; // Stop skipping, leave the token for the caller.
        }
        Token* token_to_skip = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(token_to_skip);
    }
}

static void skip_to_loop_end(Interpreter* interpreter, int target_loop_col) {
    Token* last_token_before_eof_skip = interpreter->current_token;

    while(interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->col <= target_loop_col) {
            return; // Found end of block by dedentation
        }
        last_token_before_eof_skip = interpreter->current_token;
        Token* old_token = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(old_token);
    }
    report_error("Syntax", "Unexpected EOF while skipping to loop end. Missing block terminator?", last_token_before_eof_skip);
}

static StatementExecStatus execute_statements_in_controlled_block(Interpreter* interpreter, int start_col, const char* block_type_for_error, TokenType terminator1, TokenType terminator2, TokenType terminator3) {
    Token* last_token_before_terminator_or_eof = NULL; // Initialize to NULL
    int expected_body_indent = start_col + 4; // Body is indented 4 spaces more

    // The top-level coroutine execution handles resuming from a yielded state by resetting the lexer.

    while (1) {
        if (last_token_before_terminator_or_eof) { // Free the old token if exists
            free_token(last_token_before_terminator_or_eof);
        }
        last_token_before_terminator_or_eof = token_deep_copy(interpreter->current_token);

        TokenType current_type = interpreter->current_token->type;
        int current_col = interpreter->current_token->col;

        if (current_type == TOKEN_EOF) {
            break;
        }

        // If indentation drops, the block is over. If it's at the same level,
        // it must be a valid terminator for this block type.
        if (current_col < start_col || (current_col == start_col && (current_type == terminator1 || current_type == terminator2 || current_type == terminator3))) {
            break;
        }
        if (current_col == start_col) { // Dedent to same level, but not a terminator
            break;
        }

        // If we are here, we are inside the block. Check for correct indentation.
        if (current_col != expected_body_indent) { // This check is now correct.
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg),
                     "Statement in '%s' block has incorrect indentation. Expected column %d, got column %d.",
                     block_type_for_error, expected_body_indent, current_col);
            report_error("Syntax", err_msg, interpreter->current_token);
        }

        StatementExecStatus status = interpret_statement(interpreter);

        if (status == STATEMENT_YIELDED_AWAIT) {
            // If a statement yields, just propagate the status up.
            // Do NOT skip the rest of the block.
            if (last_token_before_terminator_or_eof) free_token(last_token_before_terminator_or_eof);
            return status;
        } else if (status != STATEMENT_EXECUTED_OK) {
            // For break, continue, return, exception, we DO skip the rest of the block.
            skip_statements_in_branch(interpreter, start_col);
            if (last_token_before_terminator_or_eof) {
                free_token(last_token_before_terminator_or_eof);
            }
            return status; 
        }
    }

    if (interpreter->current_token->type == TOKEN_EOF) {
        bool eof_is_valid_terminator = (terminator1 == TOKEN_EOF || terminator2 == TOKEN_EOF || terminator3 == TOKEN_EOF);
        if (!eof_is_valid_terminator) {
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg), "Unexpected EOF in '%s' block. Missing '%s', '%s', or '%s' to terminate? Last processed token was near line %d, col %d.",
                 block_type_for_error,
                 token_type_to_string(terminator1),
                 token_type_to_string(terminator2),
                 token_type_to_string(terminator3),
                 last_token_before_terminator_or_eof->line, 
                 last_token_before_terminator_or_eof->col);
            Token temp_token_for_error = *last_token_before_terminator_or_eof;
            temp_token_for_error.value = NULL; // Prevent use-after-free of the string value
            free_token(last_token_before_terminator_or_eof);
            last_token_before_terminator_or_eof = NULL; // Avoid double-free at the end of the function
            report_error("Syntax", err_msg, &temp_token_for_error);
        }
    }

    if (last_token_before_terminator_or_eof) {
        free_token(last_token_before_terminator_or_eof);
    }

    return STATEMENT_EXECUTED_OK;
}

static StatementExecStatus execute_loop_body_iteration(Interpreter* interpreter, int loop_start_col, int expected_body_indent_col, const char* loop_type_for_error) {
    Token* last_token_in_block = NULL;

    // Top-level coroutine execution handles resuming from a yielded state by resetting the lexer.
    // This function now just executes statements in the current iteration.

    // Loop as long as the current token is part of the loop body
    while (interpreter->current_token->type != TOKEN_EOF && 
           !(interpreter->current_token->col <= loop_start_col) && 
           interpreter->current_token->col >= expected_body_indent_col) { 
        
        if (last_token_in_block) free_token(last_token_in_block); // Free previous copy
        last_token_in_block = token_deep_copy(interpreter->current_token); // Make a new copy
        // Check if the current token signifies the end of the loop structure itself.
        if (interpreter->current_token->col != expected_body_indent_col) {
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg),
                     "Statement in '%s' loop body has incorrect indentation. Expected column %d, got column %d.",
                     loop_type_for_error, expected_body_indent_col, interpreter->current_token->col);
            report_error("Syntax", err_msg, interpreter->current_token);
        }
 
        StatementExecStatus status = interpret_statement(interpreter);

        // If the statement yielded, or caused a break/continue/return/exception,
        // we must stop executing this block and propagate that status up.
        if (status != STATEMENT_EXECUTED_OK) {
            if (last_token_in_block) free_token(last_token_in_block);
            return status;
        }
    }

    // The check for EOF was flawed. If the loop terminates due to EOF, it's a valid end of the program.
    // The while loop condition correctly handles termination by dedentation.
    if (last_token_in_block) {
        free_token(last_token_in_block);
    }
    return STATEMENT_EXECUTED_OK; // Return OK if the block completes normally
}

static void skip_all_elif_and_else_branches(Interpreter* interpreter, int if_col) {
    // This function is called after a branch has been taken and has yielded or propagated a control flow flag.
    // It ensures the parser skips past the rest of the if-elif-else structure.
    while (interpreter->current_token->type == TOKEN_ELIF && interpreter->current_token->col == if_col) {
        interpreter_eat(interpreter, TOKEN_ELIF);
        interpreter_eat(interpreter, TOKEN_COLON);
        // We must skip the expression to avoid side effects and advance the lexer correctly.
        interpreter->prevent_side_effects = true;
        ExprResult dummy_res = interpret_expression(interpreter);
        interpreter->prevent_side_effects = false;
        if (dummy_res.is_freshly_created_container) free_value_contents(dummy_res.value);
        interpreter_eat(interpreter, TOKEN_COLON);
        skip_statements_in_branch(interpreter, if_col);
    }
    if (interpreter->current_token->type == TOKEN_ELSE && interpreter->current_token->col == if_col) {
        interpreter_eat(interpreter, TOKEN_ELSE);
        interpreter_eat(interpreter, TOKEN_COLON);
        skip_statements_in_branch(interpreter, if_col);
    }
}

static StatementExecStatus interpret_if_statement(Interpreter* interpreter) { // New, robust implementation
    Token* if_keyword_token_for_context = token_deep_copy(interpreter->current_token);
    int if_col = if_keyword_token_for_context->col;
    StatementExecStatus status = STATEMENT_EXECUTED_OK;
    bool branch_taken = false;

    // --- IF ---
    interpreter_eat(interpreter, TOKEN_IF);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'if'
    ExprResult cond_res = interpret_expression(interpreter);

    if (interpreter->exception_is_active) {
        if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);
        free_token(if_keyword_token_for_context);
        return STATEMENT_PROPAGATE_FLAG;
    }
    if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
        if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);
        free_token(if_keyword_token_for_context);
        return STATEMENT_YIELDED_AWAIT;
    }

    int condition_line = interpreter->current_token->line;
    interpreter_eat(interpreter, TOKEN_COLON);
    if (interpreter->current_token->line == condition_line && interpreter->current_token->type != TOKEN_EOF) {
        report_error("Syntax", "Unexpected token on the same line after 'if' condition. Expected a newline and an indented block.", interpreter->current_token);
    }

    if (interpreter->current_token->col <= if_col) {
        free_token(if_keyword_token_for_context);
        if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);
        report_error("Syntax", "Expected an indented block after 'if' statement.", interpreter->current_token);
    }

    if (value_is_truthy(cond_res.value)) {
        branch_taken = true;
        status = execute_statements_in_controlled_block(interpreter, if_col, "if", TOKEN_ELIF, TOKEN_ELSE, TOKEN_EOF);
		if (status == STATEMENT_YIELDED_AWAIT) {
			free_token(if_keyword_token_for_context);
			if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);
			return STATEMENT_YIELDED_AWAIT;
		}
    } else {
        skip_statements_in_branch(interpreter, if_col);
    }
    if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);

    // --- ELIFs ---
    while (interpreter->current_token->type == TOKEN_ELIF && interpreter->current_token->col == if_col) {
        if (branch_taken) { // A previous branch was taken, so just skip this entire clause
            interpreter_eat(interpreter, TOKEN_ELIF); interpreter_eat(interpreter, TOKEN_COLON);
            interpreter->prevent_side_effects = true;
            ExprResult dummy = interpret_expression(interpreter);
            interpreter->prevent_side_effects = false;
            if (dummy.is_freshly_created_container) free_value_contents(dummy.value);
            interpreter_eat(interpreter, TOKEN_COLON);
            skip_statements_in_branch(interpreter, if_col);
            continue;
        }
        // This is the first branch to be considered after the initial 'if' failed.
        Token* elif_token_for_error = token_deep_copy(interpreter->current_token);
        interpreter_eat(interpreter, TOKEN_ELIF);
        interpreter_eat(interpreter, TOKEN_COLON);
        ExprResult elif_cond_res = interpret_expression(interpreter);
        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            if (elif_cond_res.is_freshly_created_container) free_value_contents(elif_cond_res.value);
            free_token(elif_token_for_error); free_token(if_keyword_token_for_context);
            return interpreter->exception_is_active ? STATEMENT_PROPAGATE_FLAG : STATEMENT_YIELDED_AWAIT;
        }
        int elif_cond_line = interpreter->current_token->line;
        interpreter_eat(interpreter, TOKEN_COLON);
        if (interpreter->current_token->line == elif_cond_line && interpreter->current_token->type != TOKEN_EOF) {
            report_error("Syntax", "Unexpected token on the same line after 'elif' condition. Expected a newline and an indented block.", interpreter->current_token);
        }
        if (interpreter->current_token->col <= if_col) {
            free_token(if_keyword_token_for_context); free_token(elif_token_for_error);
            report_error("Syntax", "Expected an indented block after 'elif' statement.", interpreter->current_token);
        }

        if (value_is_truthy(elif_cond_res.value)) {
            branch_taken = true;
            status = execute_statements_in_controlled_block(interpreter, if_col, "elif", TOKEN_ELIF, TOKEN_ELSE, TOKEN_EOF);
			if (status == STATEMENT_YIELDED_AWAIT) {
				if (elif_cond_res.is_freshly_created_container) free_value_contents(elif_cond_res.value);
				free_token(elif_token_for_error);
				free_token(if_keyword_token_for_context);
				return STATEMENT_YIELDED_AWAIT;
			}
        } else {
            skip_statements_in_branch(interpreter, if_col);
        }
        if (elif_cond_res.is_freshly_created_container) free_value_contents(elif_cond_res.value);
        free_token(elif_token_for_error);

        if (branch_taken) break; // Exit the while loop once a branch is taken
    }

    // --- ELSE ---
    if (!branch_taken && interpreter->current_token->type == TOKEN_ELSE && interpreter->current_token->col == if_col) {
        Token* else_token_for_error = token_deep_copy(interpreter->current_token);
        int else_line = else_token_for_error->line;
        interpreter_eat(interpreter, TOKEN_ELSE);
        interpreter_eat(interpreter, TOKEN_COLON);
        if (interpreter->current_token->line == else_line && interpreter->current_token->type != TOKEN_EOF)
            report_error("Syntax", "Unexpected token on the same line after 'else:'. Expected a newline and an indented block.", interpreter->current_token);
        if (interpreter->current_token->col <= if_col) {
            free_token(else_token_for_error); free_token(if_keyword_token_for_context);
            report_error("Syntax", "Expected an indented block after 'else' statement.", else_token_for_error);
        }
        status = execute_statements_in_controlled_block(interpreter, if_col, "else", TOKEN_EOF, TOKEN_EOF, TOKEN_EOF);
		if (status == STATEMENT_YIELDED_AWAIT) {
			free_token(else_token_for_error);
			free_token(if_keyword_token_for_context);
			return STATEMENT_YIELDED_AWAIT;
		}
        free_token(else_token_for_error);
    }

    // After the correct branch has been executed (or skipped), we must skip any remaining parts of the structure.
    skip_all_elif_and_else_branches(interpreter, if_col);

    free_token(if_keyword_token_for_context);
    return status;
}

static void interpret_break_statement(Interpreter* interpreter) { // This function itself doesn't yield
    Token* break_token = interpreter->current_token;
    interpreter_eat(interpreter, TOKEN_BREAK);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'break'

    if (interpreter->loop_depth == 0) {
        report_error("Syntax", "'break:' statement found outside of a loop.", break_token);
    }
    interpreter->break_flag = 1;
}

static void interpret_skip_statement(Interpreter* interpreter) {
    // This is a no-op statement, like 'pass' in Python.
    // It's useful for empty blocks where syntax requires a statement.
    interpreter_eat(interpreter, TOKEN_SKIP);
    interpreter_eat(interpreter, TOKEN_COLON); // All simple statements end with a colon.
}

static void interpret_continue_statement(Interpreter* interpreter) { // This function itself doesn't yield too
    Token* continue_token = interpreter->current_token;
    interpreter_eat(interpreter, TOKEN_CONTINUE);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'continue'

    if (interpreter->loop_depth == 0) {
        report_error("Syntax", "'continue:' statement found outside of a loop.", continue_token);
    }
    interpreter->continue_flag = 1;
}

static void interpret_funct_statement(Interpreter* interpreter, int statement_start_col, bool is_async_param) {
    Token* funct_token_original_ref = token_deep_copy(interpreter->current_token);
    int funct_def_col = statement_start_col; // Use the passed start column
    interpreter_eat(interpreter, TOKEN_FUNCT);
    interpreter_eat(interpreter, TOKEN_COLON);
    if (interpreter->current_token->type != TOKEN_ID) {
        report_error("Syntax", "Expected function name after 'funct:'.", interpreter->current_token);
    }
    char* func_name_str = strdup(interpreter->current_token->value);
    if (!func_name_str) {
        report_error("System", "Failed to strdup function name.", funct_token_original_ref);
    }
    interpreter_eat(interpreter, TOKEN_ID);

    Function* new_func = calloc(1, sizeof(Function));
    if (!new_func) {
        free(func_name_str);
        report_error("System", "Failed to allocate memory for Function struct.", funct_token_original_ref);
    }

    new_func->name = func_name_str;
    new_func->definition_col = funct_def_col;
    new_func->definition_line = funct_token_original_ref->line;
    new_func->params = NULL;
    new_func->param_count = 0;
    new_func->is_async = is_async_param;
    new_func->definition_scope = interpreter->current_scope; // Lexical scope
    // Point to the lexer's current text; value_deep_copy will strdup it for the stored version.
    new_func->source_text_owned_copy = (char*)interpreter->lexer->text; // Cast away const for temporary assignment
    new_func->source_text_length = interpreter->lexer->text_length;
    new_func->is_source_owner = false; // This temporary Function struct does not own the source text yet.
    new_func->body_end_token_original_line = -1; // Initialize to -1, meaning not pre-scanned
    new_func->body_end_token_original_col = -1; // Initialize to -1

    interpreter_eat(interpreter, TOKEN_LPAREN);
    int param_capacity = 4;
    if (interpreter->current_token->type != TOKEN_RPAREN) {
        new_func->params = malloc(param_capacity * sizeof(Parameter));
        if (!new_func->params && param_capacity > 0) {
            free(new_func->name);
            // new_func->source_text_owned_copy is not owned here
            free(new_func);
            report_error("System", "Failed to alloc params for func def.", funct_token_original_ref);
        }

        while (1) {
            if (new_func->param_count >= param_capacity) {
                param_capacity *= 2;
                Parameter* new_params_ptr = realloc(new_func->params, param_capacity * sizeof(Parameter));
                if (!new_params_ptr) {
                    // Proper cleanup of existing new_func->params elements would be needed here
                    free(new_func->name);
                    if (new_func->params) { // Free existing params if realloc fails
                        for(int k=0; k<new_func->param_count; ++k) {
                            free(new_func->params[k].name);
                            if (new_func->params[k].default_value) {
                                free_value_contents(*(new_func->params[k].default_value));
                                free(new_func->params[k].default_value);
                            }
                        }
                        free(new_func->params);
                    }
                    // new_func->source_text_owned_copy is not owned here
                    free(new_func);
                    report_error("System", "Failed to realloc params for function definition.", interpreter->current_token);
                }
                new_func->params = new_params_ptr;
            }
            if (interpreter->current_token->type != TOKEN_ID) {
                report_error("Syntax", "Expected parameter name.", interpreter->current_token);
            }
            new_func->params[new_func->param_count].name = strdup(interpreter->current_token->value);
            if (!new_func->params[new_func->param_count].name) {
                // Full cleanup
                for(int k=0; k<new_func->param_count; ++k) { // Free previously allocated param names
                    free(new_func->params[k].name);
                    if (new_func->params[k].default_value) { // And their default values
                        free_value_contents(*(new_func->params[k].default_value));
                        free(new_func->params[k].default_value);
                    }
                }
                free(new_func->params);
                free(new_func->name);
                    // new_func->source_text_owned_copy is not owned here
                free(new_func);
                report_error("System", "Failed to strdup parameter name.", interpreter->current_token);
            }

            new_func->params[new_func->param_count].default_value = NULL;
            interpreter_eat(interpreter, TOKEN_ID);

            if (interpreter->current_token->type == TOKEN_ASSIGN) { // Default value
                interpreter_eat(interpreter, TOKEN_ASSIGN);
                new_func->params[new_func->param_count].default_value = malloc(sizeof(Value));
                if (!new_func->params[new_func->param_count].default_value) {
                    // Full cleanup for current and previous params
                    free(new_func->params[new_func->param_count].name);
                    for(int k=0; k<new_func->param_count; ++k) { free(new_func->params[k].name); /* free default_value if set */ }
                    free(new_func->params);
                    free(new_func->name);
                    // new_func->source_text_owned_copy is not owned here
                    free(new_func);
                    report_error("System", "Failed to alloc memory for default param value.", interpreter->current_token);
                }
                ExprResult default_val_expr_res = interpret_expression(interpreter); // Default value is an expression
                *(new_func->params[new_func->param_count].default_value) = default_val_expr_res.value;
                if (interpreter->exception_is_active) { // Exception during default value parsing                    
                    report_error("Internal", "Exception during default param parsing. Further cleanup may be needed.", interpreter->current_token);
                }
            }
            new_func->param_count++;

            if (interpreter->current_token->type == TOKEN_RPAREN) break;
            interpreter_eat(interpreter, TOKEN_COMMA);
        }
    }
    interpreter_eat(interpreter, TOKEN_RPAREN);

    // --- START: Stricter Syntax Check ---
    int funct_header_line = interpreter->current_token->line;
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after parameters
    if (interpreter->current_token->line == funct_header_line && interpreter->current_token->type != TOKEN_EOF) {
        report_error("Syntax", "Unexpected token on the same line after function signature. Expected a newline and an indented block.", interpreter->current_token);
    }
    // --- END: Stricter Syntax Check ---

    // After the 'funct ... ():', the next token MUST be indented.
    // If it's not, it's a syntax error (missing body).
    if (interpreter->current_token->col <= funct_def_col) {
        // Cleanup before error reporting
        free(new_func->name);
        if (new_func->params) {
            for (int i = 0; i < new_func->param_count; ++i) {
                free(new_func->params[i].name);
                if (new_func->params[i].default_value) {
                    free_value_contents(*(new_func->params[i].default_value));
                    free(new_func->params[i].default_value);
                }
            }
            free(new_func->params);
        }
        free(new_func);
        free_token(funct_token_original_ref);
        report_error("Syntax", "Expected an indented block after function definition.", interpreter->current_token);
    }

    // Capture body start state
    // Ensure the first token of the body (if any) is correctly indented. // TOKEN_END removed
    // The body itself starts on the next line, indented by 4 spaces relative to `funct_def_col`.
    if (interpreter->current_token->col > funct_def_col && interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->col != funct_def_col + 4) {
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg),
                     "First statement in function '%s' body has incorrect indentation. Expected column %d, got column %d.",
                     new_func->name, funct_def_col + 4, interpreter->current_token->col);
            report_error("Syntax", err_msg, interpreter->current_token);
        }    
        LexerState absolute_body_start_state = get_lexer_state_for_token_start(interpreter->lexer,
                                                                 interpreter->current_token->line,
                                                                 interpreter->current_token->col,
                                                                 interpreter->current_token);
        new_func->body_start_state = absolute_body_start_state; // Store absolute for now, adjust when source_text_owned_copy is made
    } else { // Empty function body
        new_func->body_start_state = get_lexer_state_for_token_start(interpreter->lexer, interpreter->current_token->line, interpreter->current_token->col, interpreter->current_token); // Points to 'end'
    }

    // The new logic: skip tokens until indentation is less than or equal to funct_def_col
    // This assumes the function body is always indented by 4 spaces.
    int expected_body_indent = funct_def_col + 4;
    while (interpreter->current_token->type != TOKEN_EOF &&
           interpreter->current_token->col >= expected_body_indent) {
        // If the current token is at the same indentation level as the function definition,
        // and it's not the start of the function body, then the function body has ended.
        // This is a heuristic and might need refinement for complex cases.
        Token* temp_tok_to_free = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(temp_tok_to_free);
    }

    // The current_token is now the first token *after* the function body, at an indentation
    // less than or equal to the function definition's column.
    Value func_val; func_val.type = VAL_FUNCTION; func_val.as.function_val = new_func;
    symbol_table_define(interpreter->current_scope, new_func->name, func_val); // This makes a deep copy for the symbol table.

    // Now, free the temporary `new_func` and its contents using the standard function.
    free_value_contents(func_val);
    free_token(funct_token_original_ref);
}

static StatementExecStatus interpret_return_statement(Interpreter* interpreter) {
    StatementExecStatus status = STATEMENT_EXECUTED_OK;
    Token* return_keyword_token = token_deep_copy(interpreter->current_token);
    int start_line = return_keyword_token->line;

    if (interpreter->function_nesting_level == 0 && !interpreter->current_executing_coroutine) {
        report_error("Syntax", "'return:' statement found outside of a function.", return_keyword_token);
    }
    interpreter_eat(interpreter, TOKEN_RETURN);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'return'

    // Case 1: Empty return (e.g., "return:" followed by a newline)
    if (interpreter->current_token->line != start_line || interpreter->current_token->type == TOKEN_EOF) {
        free_value_contents(interpreter->current_function_return_value);
        interpreter->current_function_return_value = create_null_value();
        interpreter->return_flag = 1;
        free_token(return_keyword_token);
        return STATEMENT_PROPAGATE_FLAG;
    }

    // Case 2: One or more comma-separated expressions
    #define MAX_RETURN_VALUES 16
    ExprResult results[MAX_RETURN_VALUES];
    int result_count = 0;

    do {
        if (result_count >= MAX_RETURN_VALUES) {
            report_error("Syntax", "Exceeded maximum number of return values (16).", interpreter->current_token);
        }
        results[result_count] = interpret_expression(interpreter);

        if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            for (int i = 0; i <= result_count; ++i) {
                if (results[i].is_freshly_created_container) free_value_contents(results[i].value);
            }
            free_token(return_keyword_token);
            return interpreter->exception_is_active ? STATEMENT_PROPAGATE_FLAG : STATEMENT_YIELDED_AWAIT;
        }
        result_count++;

        if (interpreter->current_token->type == TOKEN_COMMA) {
            interpreter_eat(interpreter, TOKEN_COMMA);
        } else {
            break;
        }
    } while (1);

    Value final_return_value;
    bool final_return_value_is_fresh = false;

    if (result_count == 1) {
        final_return_value = results[0].value;
        final_return_value_is_fresh = results[0].is_freshly_created_container;
    } else {
        Tuple* tuple = malloc(sizeof(Tuple));
        if (!tuple) report_error("System", "Failed to allocate memory for return tuple.", return_keyword_token);
        tuple->count = result_count;
        tuple->elements = malloc(result_count * sizeof(Value));
        if (!tuple->elements) { free(tuple); report_error("System", "Failed to allocate memory for return tuple elements.", return_keyword_token); }

        for (int i = 0; i < result_count; ++i) {
            tuple->elements[i] = value_deep_copy(results[i].value);
        }
        final_return_value.type = VAL_TUPLE;
        final_return_value.as.tuple_val = tuple;
        final_return_value_is_fresh = true;
    }

    interpreter_eat(interpreter, TOKEN_COLON);

    if (interpreter->current_token->line == start_line && interpreter->current_token->type != TOKEN_EOF) {
        report_error("Syntax", "Unexpected token on the same line after return statement.", interpreter->current_token);
    }

    free_value_contents(interpreter->current_function_return_value);
    interpreter->current_function_return_value = value_deep_copy(final_return_value);
    interpreter->return_flag = 1;
    status = STATEMENT_PROPAGATE_FLAG; // Return is a propagation flag

    for (int i = 0; i < result_count; ++i) {
        if (results[i].is_freshly_created_container) free_value_contents(results[i].value);
    }
    // If multiple values were returned, a new tuple was created for final_return_value, which must also be freed.
    // If only one value was returned, final_return_value was a shallow copy and was freed by the loop above.
    if (result_count > 1 && final_return_value_is_fresh) {
        free_value_contents(final_return_value);
    }
    free_token(return_keyword_token);
    return status;
}

static StatementExecStatus interpret_loop_statement(Interpreter* interpreter) {
    StatementExecStatus status = STATEMENT_EXECUTED_OK;
    Token* loop_keyword_token_for_context = token_deep_copy(interpreter->current_token);
    // Save the start state of the 'loop:' statement to potentially rewind to on async resume.
    LexerState loop_statement_start_state = get_lexer_state_for_token_start(
        interpreter->lexer, loop_keyword_token_for_context->line, loop_keyword_token_for_context->col, loop_keyword_token_for_context
    );
    (void)loop_statement_start_state; // Suppress unused variable warning

    int loop_col = loop_keyword_token_for_context->col;
    int loop_line = loop_keyword_token_for_context->line; // Keep loop_line for error context
    interpreter_eat(interpreter, TOKEN_LOOP);
    interpreter_eat(interpreter, TOKEN_COLON);

    if (interpreter->current_token->type == TOKEN_WHILE) {
        // This is the new control loop for 'while'
        interpreter_eat(interpreter, TOKEN_WHILE);

        // Capture the start of the condition expression ONCE.
        LexerState condition_start_state = get_lexer_state_for_token_start(
            interpreter->lexer,
            interpreter->current_token->line,
            interpreter->current_token->col,
            interpreter->current_token
        );

        bool just_resumed = (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_RESUMING);

        interpreter->loop_depth++;
        bool is_first_pass_of_c_loop = true;

        while (1) { // The new C control loop
            // On the first pass of this C loop (whether it's the first time the statement
            // is run, or the first time after an async resume), the lexer is already
            // positioned at the start of the condition. On subsequent synchronous iterations,
            // we must rewind to re-evaluate the condition.
            if (is_first_pass_of_c_loop) {
                is_first_pass_of_c_loop = false;
                if (just_resumed) {
                    just_resumed = false; // Consume the flag for the first pass
                }
                // The flag will be consumed by the await expression itself. We just check it here.
            } else {
                rewind_lexer_and_token(interpreter, condition_start_state, NULL);
            }
            Token* condition_token_for_error = token_deep_copy(interpreter->current_token);

            // --- Part 1: Evaluate Condition ---
            ExprResult cond_res = interpret_expression(interpreter);

            if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
                free_token(condition_token_for_error);
                if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);
                return STATEMENT_YIELDED_AWAIT;
            }
            if (interpreter->exception_is_active) {
                free_token(condition_token_for_error);
                if (cond_res.is_freshly_created_container) free_value_contents(cond_res.value);
                free_token(loop_keyword_token_for_context);
                return STATEMENT_PROPAGATE_FLAG;
            }

            Value condition = cond_res.value;
            // --- START: Stricter Syntax Check ---
            int while_cond_line = interpreter->current_token->line;
            interpreter_eat(interpreter, TOKEN_COLON);
            if (interpreter->current_token->line == while_cond_line && interpreter->current_token->type != TOKEN_EOF) {
                report_error("Syntax", "Unexpected token on the same line after 'while' condition. Expected a newline and an indented block.", interpreter->current_token);
            }
            // --- END: Stricter Syntax Check ---

            // After the 'while condition:', the next token MUST be indented.
            if (interpreter->current_token->col <= loop_col) {
                free_token(loop_keyword_token_for_context);
                report_error("Syntax", "Expected an indented block after 'while' loop condition.", condition_token_for_error);
            }

            if (condition.type != VAL_BOOL) {
                if (cond_res.is_freshly_created_container) free_value_contents(condition);
                report_error("Runtime", "Condition for 'while' loop must be a boolean.", condition_token_for_error);
                free_token(condition_token_for_error); // report_error exits
                return STATEMENT_PROPAGATE_FLAG; 
            }
            free_token(condition_token_for_error);

            bool should_continue_loop = condition.as.bool_val;
            // VAL_BOOL has no complex contents to free via free_value_contents(condition)

            if (!should_continue_loop) {
                skip_to_loop_end(interpreter, loop_col);
                if (interpreter->exception_is_active) { // Exception during skip
                    free_token(loop_keyword_token_for_context);
                    return STATEMENT_PROPAGATE_FLAG;
                }
                break; // Exit the C while(1) loop
            }

            // --- Part 2: Execute Body ---
            // Check indentation of the first statement in the loop body, if one exists.
            if (interpreter->current_token->col > loop_col && interpreter->current_token->type != TOKEN_EOF) {
                if (interpreter->current_token->col != loop_col + 4) {
                    char err_msg[300];
                    snprintf(err_msg, sizeof(err_msg),
                             "First statement in 'while' loop body has incorrect indentation. Expected column %d, got column %d.",
                             loop_col + 4, interpreter->current_token->col);
                    report_error("Syntax", err_msg, interpreter->current_token);
                }
            }            StatementExecStatus body_status = execute_loop_body_iteration(interpreter, loop_col, loop_col + 4, "while");

            if (body_status == STATEMENT_YIELDED_AWAIT) {
                free_token(loop_keyword_token_for_context);
                return STATEMENT_YIELDED_AWAIT;
            }

            if (interpreter->break_flag) {
                interpreter->break_flag = 0;
                skip_to_loop_end(interpreter, loop_col);
                if (interpreter->exception_is_active) { return STATEMENT_PROPAGATE_FLAG; }
                break; 
            }

            if (interpreter->continue_flag) {
                interpreter->continue_flag = 0;
                continue; 
            }

            if (interpreter->return_flag || interpreter->exception_is_active) {
                skip_to_loop_end(interpreter, loop_col);
                free_token(loop_keyword_token_for_context);
                return STATEMENT_PROPAGATE_FLAG;
            }
        } // End of the new C while(1) loop

        status = STATEMENT_EXECUTED_OK; // If we finished the loop normally.

        interpreter->loop_depth--;
    } else if (interpreter->current_token->type == TOKEN_FOR) {
        status = interpret_for_loop(interpreter, loop_col, loop_line, loop_keyword_token_for_context);
        // If interpret_for_loop yielded, it will return STATEMENT_YIELDED_AWAIT.
        // The interpret_for_loop function handles its own loop_depth decrement.
    } else {
        char err_msg[200];
        sprintf(err_msg, "Expected 'while' or 'for' after 'loop:', but got %s.",
                token_type_to_string(interpreter->current_token->type));
        free_token(loop_keyword_token_for_context);
        report_error("Syntax", err_msg, interpreter->current_token); // Error at current token, loop_keyword_token_for_context is eaten
    } // The loop implicitly ends when indentation drops.

    free_token(loop_keyword_token_for_context);
    return status;
}

static StatementExecStatus interpret_for_loop(Interpreter* interpreter, int loop_col, int loop_line, Token* loop_keyword_token_for_context) {
    StatementExecStatus status = STATEMENT_EXECUTED_OK;
    (void)loop_line; // Mark as unused
    (void)loop_keyword_token_for_context; // Mark as unused for now
    interpreter_eat(interpreter, TOKEN_FOR);
    // Changed from interpret_ternary_expr to interpret_expression
    Token* var_name_token = token_deep_copy(interpreter->current_token); // For error reporting
    if (var_name_token->type != TOKEN_ID) report_error("Syntax", "Expected identifier for loop variable after 'for'.", var_name_token);
    char* var_name_str = strdup(var_name_token->value); // This is the public loop variable name
    if (!var_name_str) report_error("System", "Failed to allocate memory for loop variable name.", var_name_token);
    interpreter_eat(interpreter, TOKEN_ID);

    // 1. Create a single, persistent scope for the entire 'for' loop.
    enter_scope(interpreter);
    interpreter->loop_depth++;

    int body_indent = loop_col + 4; // Body indented 4 spaces relative to 'loop:'

    if (interpreter->current_token->type == TOKEN_FROM) { // Range loop
        // --- START: New async-safe for...from...to implementation ---
        interpreter_eat(interpreter, TOKEN_FROM);
        
        // On first entry, evaluate start/end/step and store them.
        // On resume, these expressions are NOT re-evaluated. The loop continues from where it was.
        // This requires storing the loop state (current value, end, step) in the loop's scope.
        
        // Create hidden variable names
        char end_var_name[256], step_var_name[256];
        snprintf(end_var_name, sizeof(end_var_name), "__%s_end", var_name_str);
        snprintf(step_var_name, sizeof(step_var_name), "__%s_step", var_name_str);

        bool just_resumed = (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_RESUMING);

        Value* loop_var_ptr = symbol_table_get_local(interpreter->current_scope, var_name_str);

        if (loop_var_ptr == NULL) { // First time entering this loop
            ExprResult start_res = interpret_expression(interpreter);
            if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
                if (start_res.is_freshly_created_container) free_value_contents(start_res.value);
                status = STATEMENT_YIELDED_AWAIT;
                goto cleanup_for_loop;
            }
            if (interpreter->exception_is_active) { if (start_res.is_freshly_created_container) free_value_contents(start_res.value); status = STATEMENT_PROPAGATE_FLAG; goto cleanup_for_loop; }
            interpreter_eat(interpreter, TOKEN_TO);
            ExprResult end_res = interpret_expression(interpreter);
            if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
                if (start_res.is_freshly_created_container) free_value_contents(start_res.value);
                if (end_res.is_freshly_created_container) free_value_contents(end_res.value);
                status = STATEMENT_YIELDED_AWAIT;
                goto cleanup_for_loop;
            }
            if (interpreter->exception_is_active) { if (start_res.is_freshly_created_container) free_value_contents(start_res.value); if (end_res.is_freshly_created_container) free_value_contents(end_res.value); status = STATEMENT_PROPAGATE_FLAG; goto cleanup_for_loop; }
            
            Value step_val; step_val.type = VAL_INT; step_val.as.integer = 1;
            bool step_is_fresh = false;
            if (interpreter->current_token->type == TOKEN_STEP) {
                interpreter_eat(interpreter, TOKEN_STEP);
                ExprResult step_res = interpret_expression(interpreter);
                if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
                    if (start_res.is_freshly_created_container) free_value_contents(start_res.value);
                    if (end_res.is_freshly_created_container) free_value_contents(end_res.value);
                    if (step_res.is_freshly_created_container) free_value_contents(step_res.value);
                    status = STATEMENT_YIELDED_AWAIT;
                    goto cleanup_for_loop;
                }
                if (interpreter->exception_is_active) { if (start_res.is_freshly_created_container) free_value_contents(start_res.value); if (end_res.is_freshly_created_container) free_value_contents(end_res.value); if (step_res.is_freshly_created_container) free_value_contents(step_res.value); status = STATEMENT_PROPAGATE_FLAG; goto cleanup_for_loop; }
                step_val = step_res.value;
                step_is_fresh = step_res.is_freshly_created_container;
            }

            // --- START: Stricter Syntax Check ---
            int for_header_line = interpreter->current_token->line;
            interpreter_eat(interpreter, TOKEN_COLON);
            if (interpreter->current_token->line == for_header_line && interpreter->current_token->type != TOKEN_EOF) {
                report_error("Syntax", "Unexpected token on the same line after 'for...from...to' header. Expected a newline and an indented block.", interpreter->current_token);
            }
            // --- END: Stricter Syntax Check ---

            if (!((start_res.value.type == VAL_INT || start_res.value.type == VAL_FLOAT) && (end_res.value.type == VAL_INT || end_res.value.type == VAL_FLOAT) && (step_val.type == VAL_INT || step_val.type == VAL_FLOAT))) {
                report_error("Runtime", "Start, end, and step values for 'for...from...to' loop must be numbers.", var_name_token);
            }

            // Store loop parameters in hidden variables within the loop's scope
            symbol_table_define(interpreter->current_scope, end_var_name, end_res.value);
            symbol_table_define(interpreter->current_scope, step_var_name, step_val);
            // Initialize the public loop variable
            symbol_table_define(interpreter->current_scope, var_name_str, start_res.value);

            if (start_res.is_freshly_created_container) free_value_contents(start_res.value);
            if (end_res.is_freshly_created_container) free_value_contents(end_res.value);
            if (step_is_fresh) free_value_contents(step_val);
        }

        if (interpreter->current_token->col <= loop_col) report_error("Syntax", "Expected an indented block after 'for...from...to' statement.", loop_keyword_token_for_context);
        LexerState loop_body_start_lexer_state = get_lexer_state_for_token_start(interpreter->lexer,
                                                                                  interpreter->current_token->line,
                                                                                  interpreter->current_token->col,
                                                                                  interpreter->current_token);


        
        while(1) { // Async-safe loop
            // Get current loop state from scope
            Value* i_val = symbol_table_get_local(interpreter->current_scope, var_name_str);
            Value* end_val = symbol_table_get_local(interpreter->current_scope, end_var_name);
            Value* step_val = symbol_table_get_local(interpreter->current_scope, step_var_name);

            if (!i_val || !end_val || !step_val) report_error("Internal", "Loop state variables missing in for...from...to loop.", var_name_token);

            double i_d = (i_val->type == VAL_INT) ? i_val->as.integer : i_val->as.floating;
            double end_d = (end_val->type == VAL_INT) ? end_val->as.integer : end_val->as.floating;
            double step_d = (step_val->type == VAL_INT) ? step_val->as.integer : step_val->as.floating;

            // Check termination condition
            if ((step_d > 0 && i_d > end_d) || (step_d < 0 && i_d < end_d)) {
                break; // Exit while(1)
            }

            // Execute body
            if (just_resumed) {
                DEBUG_PRINTF("FOR_LOOP (Range): Resuming from await, not rewinding body lexer.%s", "");
                just_resumed = false; // Consume the resume signal
            } else {
                rewind_lexer_and_token(interpreter, loop_body_start_lexer_state, NULL);
            }
            StatementExecStatus body_status = execute_loop_body_iteration(interpreter, loop_col, body_indent, "for...from...to");

            if (body_status == STATEMENT_YIELDED_AWAIT) {
                status = STATEMENT_YIELDED_AWAIT;
                goto cleanup_for_loop;
            }
            if (interpreter->return_flag || interpreter->exception_is_active) {
                status = STATEMENT_PROPAGATE_FLAG;
                skip_to_loop_end(interpreter, loop_col);
                goto cleanup_for_loop;
            }
            if (interpreter->break_flag) {
                interpreter->break_flag = 0;
                skip_to_loop_end(interpreter, loop_col);
                break; // Exit while(1)
            }
            if (interpreter->continue_flag) {
                interpreter->continue_flag = 0;
                // Fall through to increment step
            }

            // Increment and update loop variable
            i_d += step_d;
            if (i_val->type == VAL_INT && step_val->type == VAL_INT) {
                i_val->as.integer = (long)i_d;
            } else {
                i_val->type = VAL_FLOAT;
                i_val->as.floating = i_d;
            }
        }

    } else if (interpreter->current_token->type == TOKEN_IN) { // Collection loop
        interpreter_eat(interpreter, TOKEN_IN);
        ExprResult coll_res = interpret_expression(interpreter);
        if (interpreter->exception_is_active) {
            if (coll_res.is_freshly_created_container) free_value_contents(coll_res.value);
            status = STATEMENT_PROPAGATE_FLAG;
            goto cleanup_for_loop;
        }
        if (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT) {
            if (coll_res.is_freshly_created_container) free_value_contents(coll_res.value);
            status = STATEMENT_YIELDED_AWAIT;
            goto cleanup_for_loop;
        }
        Value collection_val = coll_res.value; // Keep this for freeing if fresh

        // --- START: Stricter Syntax Check ---
        int for_in_header_line = interpreter->current_token->line;
        interpreter_eat(interpreter, TOKEN_COLON);
        if (interpreter->current_token->line == for_in_header_line && interpreter->current_token->type != TOKEN_EOF) {
            report_error("Syntax", "Unexpected token on the same line after 'for...in' header. Expected a newline and an indented block.", interpreter->current_token);
        }
        // --- END: Stricter Syntax Check ---

        // current_token is now first token of body.
        if (interpreter->current_token->col <= loop_col) report_error("Syntax", "Expected an indented block after 'for...in' statement.", loop_keyword_token_for_context);

        LexerState loop_body_start_lexer_state = get_lexer_state_for_token_start(interpreter->lexer,
                                                                                 interpreter->current_token->line,
                                                                                 interpreter->current_token->col,
                                                                                 interpreter->current_token);

        if (collection_val.type != VAL_ARRAY && collection_val.type != VAL_STRING && collection_val.type != VAL_DICT) report_error("Runtime", "Collection in 'for...in' loop must be an array, string, or dictionary.", var_name_token);

        // Store the collection itself in a hidden variable to persist it across awaits.
        char coll_var_name[256];
        snprintf(coll_var_name, sizeof(coll_var_name), "__%s_coll", var_name_str);
        bool just_resumed = (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_RESUMING);

        symbol_table_define(interpreter->current_scope, coll_var_name, collection_val);
        if (coll_res.is_freshly_created_container) free_value_contents(collection_val);

        // Use a hidden index variable.
        char idx_var_name[256];
        snprintf(idx_var_name, sizeof(idx_var_name), "__%s_idx", var_name_str);
        Value initial_idx_val = {.type = VAL_INT, .as.integer = 0};
        symbol_table_define(interpreter->current_scope, idx_var_name, initial_idx_val);

        while(1) { // Async-safe loop
            Value* idx_ptr = symbol_table_get_local(interpreter->current_scope, idx_var_name);
            Value* coll_ptr = symbol_table_get_local(interpreter->current_scope, coll_var_name);
            if (!idx_ptr || !coll_ptr) report_error("Internal", "Loop state variables missing in for...in loop.", var_name_token);

            if (just_resumed) {
                DEBUG_PRINTF("FOR_LOOP (In): Resuming from await, not rewinding body lexer.%s", "");
                just_resumed = false; // Consume the resume signal
            }

            long current_idx = idx_ptr->as.integer;
            Value current_item;
            bool has_more_items = false;

            if (coll_ptr->type == VAL_ARRAY) {
                if (current_idx < coll_ptr->as.array_val->count) {
                    current_item = coll_ptr->as.array_val->elements[current_idx];
                    has_more_items = true;
                }
            } else if (coll_ptr->type == VAL_STRING) {
                if ((size_t)current_idx < strlen(coll_ptr->as.string_val)) {
                    char* char_str = malloc(2);
                    char_str[0] = coll_ptr->as.string_val[current_idx];
                    char_str[1] = '\0';
                    current_item.type = VAL_STRING;
                    current_item.as.string_val = char_str;
                    has_more_items = true;
                }
            } else if (coll_ptr->type == VAL_DICT) {
                Dictionary* dict = coll_ptr->as.dict_val;
                if (current_idx < dict->count) {
                    long items_seen = 0;
                    for (int i = 0; i < dict->num_buckets; ++i) {
                        for (DictEntry* entry = dict->buckets[i]; entry != NULL; entry = entry->next) {
                            if (items_seen == current_idx) {
                                current_item.type = VAL_STRING;
                                current_item.as.string_val = strdup(entry->key);
                                has_more_items = true;
                                goto found_dict_key;
                            }
                            items_seen++;
                        }
                    }
                }
                found_dict_key:;
            }

            if (!has_more_items) {
                break; // Exit while(1)
            }

            // Update the loop variable in the single loop scope.
            symbol_table_set(interpreter->current_scope, var_name_str, current_item);
            if (coll_ptr->type == VAL_STRING || coll_ptr->type == VAL_DICT) {
                // The string/key was freshly allocated for this iteration
                free_value_contents(current_item);
            }

            // Execute body
            if (!just_resumed) { // The just_resumed flag has been consumed by now
                rewind_lexer_and_token(interpreter, loop_body_start_lexer_state, NULL);
            }
            StatementExecStatus body_status = execute_loop_body_iteration(interpreter, loop_col, body_indent, "for...in");


            if (body_status == STATEMENT_YIELDED_AWAIT) {
                status = STATEMENT_YIELDED_AWAIT;
                goto cleanup_for_loop;
            }
            if (interpreter->return_flag || interpreter->exception_is_active) {
                status = STATEMENT_PROPAGATE_FLAG;
                skip_to_loop_end(interpreter, loop_col);
                goto cleanup_for_loop;
            }
            if (interpreter->break_flag) {
                interpreter->break_flag = 0;
                skip_to_loop_end(interpreter, loop_col);
                break; // Exit while(1)
            }
            if (interpreter->continue_flag) {
                interpreter->continue_flag = 0;
                // Fall through to increment step
            }

            // Increment index for next iteration
            idx_ptr->as.integer++;
        }
    } else {
        report_error("Syntax", "Expected 'from' or 'in' after 'for <variable>'.", interpreter->current_token);
    }

cleanup_for_loop:
    free(var_name_str);
    free_token(var_name_token);
    interpreter->loop_depth--;
    exit_scope(interpreter);
    if (interpreter->loop_depth == 0) {
        // break_flag is reset when handled. continue_flag is reset per iteration.
        // No need to reset them here again.
    }
    return status; // Return the overall status (could be OK or from a return)
}

static void interpret_raise_statement(Interpreter* interpreter) {
    Token* raise_token = token_deep_copy(interpreter->current_token);
    interpreter_eat(interpreter, TOKEN_RAISE);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'raise'

    ExprResult err_val_res = interpret_expression(interpreter);
    Value error_val = err_val_res.value;
    if (interpreter->exception_is_active) { // If expression itself had an error
        free_value_contents(error_val); // Free partially evaluated error_val
        free_token(raise_token);
        return; // Propagate the original error
    }

    interpreter_eat(interpreter, TOKEN_COLON);

    if (error_val.type != VAL_STRING) {
        char* val_str_repr = value_to_string_representation(error_val, interpreter, raise_token);
        char err_msg[300]; // Pass interpreter to value_to_string_representation if op_str is involved
        snprintf(err_msg, sizeof(err_msg), "Can only raise a string value as an exception. Got type for value '%s'.", val_str_repr);
        free(val_str_repr);
        if (err_val_res.is_freshly_created_container) free_value_contents(error_val);
        report_error("Runtime", err_msg, raise_token); // This exits
    }

    // Free any existing exception before setting the new one
    free_value_contents(interpreter->current_exception);
    interpreter->current_exception = value_deep_copy(error_val); // error_val is now owned by current_exception
    if (interpreter->error_token) free_token(interpreter->error_token);
    interpreter->error_token = token_deep_copy(raise_token);

    interpreter->exception_is_active = 1;

    if (err_val_res.is_freshly_created_container) free_value_contents(error_val); // Free the original expression result as it's copied
    DEBUG_PRINTF("RAISED EXCEPTION: %s", interpreter->current_exception.as.string_val);
    free_token(raise_token);
}

static StatementExecStatus interpret_try_statement(Interpreter* interpreter) {
    StatementExecStatus status = STATEMENT_EXECUTED_OK;
    Token* try_keyword_token = token_deep_copy(interpreter->current_token);
    bool yielded_in_block = false;
    // Save the start state of the 'try' statement to potentially rewind to on async resume.
    LexerState try_statement_start_state = get_lexer_state_for_token_start(
        interpreter->lexer, try_keyword_token->line, try_keyword_token->col, try_keyword_token
    );
    (void)try_statement_start_state; // Suppress unused variable warning
    
    int try_col = try_keyword_token->col;
    interpreter_eat(interpreter, TOKEN_TRY); // Consume 'try'

    // --- START: Stricter Syntax Check ---
    int try_line = try_keyword_token->line;
    interpreter_eat(interpreter, TOKEN_COLON); // Consume ':'
    if (interpreter->current_token->line == try_line && interpreter->current_token->type != TOKEN_EOF) {
        report_error("Syntax", "Unexpected token on the same line after 'try:'. Expected a newline and an indented block.", interpreter->current_token);
    }
    // --- END: Stricter Syntax Check ---

    // After 'try:', the next token MUST be indented.
    if (interpreter->current_token->col <= try_col) {
        free_token(try_keyword_token);
        report_error("Syntax", "Expected an indented block after 'try:' clause.", interpreter->current_token);
    }

    TryCatchFrame* frame = malloc(sizeof(TryCatchFrame));
    if (!frame) {
        free_token(try_keyword_token);
        report_error("System", "Failed to allocate memory for TryCatchFrame.", try_keyword_token);
    }

    frame->catch_clause = NULL;
    frame->finally_present = 0;
    frame->pending_exception_after_finally = create_null_value();
    frame->pending_exception_active_after_finally = 0;
    frame->prev = interpreter->try_catch_stack_top;
    interpreter->try_catch_stack_top = frame;
    StatementExecStatus try_block_status = STATEMENT_EXECUTED_OK;

    // 1. Execute TRY block    
    try_block_status = execute_statements_in_controlled_block(interpreter, try_col, "try", TOKEN_CATCH, TOKEN_FINALLY, TOKEN_EOF);

    if (try_block_status == STATEMENT_YIELDED_AWAIT) {
        free_token(try_keyword_token);
        return STATEMENT_YIELDED_AWAIT;
    }

    int exception_occurred_in_try = interpreter->exception_is_active;
    Value exception_from_try = create_null_value();

    if (exception_occurred_in_try) {
        exception_from_try = value_deep_copy(interpreter->current_exception);
    }

    bool has_catch_or_finally = false;

    // 2. Execute CATCH block (if an exception occurred and catch exists)
    if (interpreter->current_token->type == TOKEN_CATCH && interpreter->current_token->col == try_col) {
        has_catch_or_finally = true;
        // Parse catch clause header
        interpreter_eat(interpreter, TOKEN_CATCH);
        frame->catch_clause = malloc(sizeof(CatchClauseInfo));
        frame->catch_clause->variable_name = NULL;
        frame->catch_clause->variable_name_present = 0;
        if (interpreter->current_token->type == TOKEN_AS) {
            interpreter_eat(interpreter, TOKEN_AS);
            if (interpreter->current_token->type != TOKEN_ID) report_error("Syntax", "Expected identifier after 'catch as'", interpreter->current_token);
            frame->catch_clause->variable_name = strdup(interpreter->current_token->value);
            frame->catch_clause->variable_name_present = 1;
            interpreter_eat(interpreter, TOKEN_ID);
        }

        // --- START: Stricter Syntax Check ---
        int catch_header_line = interpreter->current_token->line;
        interpreter_eat(interpreter, TOKEN_COLON);
        if (interpreter->current_token->line == catch_header_line && interpreter->current_token->type != TOKEN_EOF) {
            report_error("Syntax", "Unexpected token on the same line after 'catch' clause. Expected a newline and an indented block.", interpreter->current_token);
        }
        // --- END: Stricter Syntax Check ---

        // After 'catch...:', the next token MUST be indented.
        if (interpreter->current_token->col <= try_col) {
            free_value_contents(exception_from_try);
            free_token(try_keyword_token);
            report_error("Syntax", "Expected an indented block after 'catch' clause.", interpreter->current_token);
        }

        if (exception_occurred_in_try && !yielded_in_block) { // Only execute catch body
            interpreter->exception_is_active = 0; // Exception is "caught" for now
            free_value_contents(interpreter->current_exception);
            interpreter->current_exception = create_null_value();

            enter_scope(interpreter); // Scope for catch block
            if (frame->catch_clause->variable_name_present) {
                symbol_table_set(interpreter->current_scope, frame->catch_clause->variable_name, exception_from_try);
            }

            StatementExecStatus catch_block_status = execute_statements_in_controlled_block(interpreter, try_col, "catch", TOKEN_FINALLY, TOKEN_EOF, TOKEN_EOF);
            if (catch_block_status == STATEMENT_YIELDED_AWAIT) { // A yield happened inside the catch block
                yielded_in_block = true;
            }

            exit_scope(interpreter); // Exit catch block scope
            // If catch block raised a new exception, interpreter->exception_is_active will be true.
            // The original exception_from_try is considered handled by this catch.
            exception_occurred_in_try = interpreter->exception_is_active; // Update based on catch block's outcome
            if (exception_occurred_in_try) { // If catch re-raised or raised new
                free_value_contents(exception_from_try); // Free the original try exception
                exception_from_try = value_deep_copy(interpreter->current_exception); // Store the new one
                } else { // Original try exception was handled
                    free_value_contents(exception_from_try);
                    exception_from_try = create_null_value();
                }
        } else { // No exception occurred or we yielded, so skip the catch block body
            skip_statements_in_branch(interpreter, try_col);
        }
    } else if (exception_occurred_in_try) {
        // Exception occurred in try, but no catch clause. It remains active.
        // exception_from_try already holds it.
    }

    // 3. Execute FINALLY block (if present)
    if (interpreter->current_token->type == TOKEN_FINALLY && interpreter->current_token->col == try_col) {
        has_catch_or_finally = true;
        int finally_line = interpreter->current_token->line;
        interpreter_eat(interpreter, TOKEN_FINALLY);
        interpreter_eat(interpreter, TOKEN_COLON);

        // --- START: Stricter Syntax Check ---
        if (interpreter->current_token->line == finally_line && interpreter->current_token->type != TOKEN_EOF) {
            report_error("Syntax", "Unexpected token on the same line after 'finally:'. Expected a newline and an indented block.", interpreter->current_token);
        }
        // --- END: Stricter Syntax Check ---

        // After 'finally:', the next token MUST be indented.
        if (interpreter->current_token->col <= try_col) {
            report_error("Syntax", "Expected an indented block after 'finally:' clause.", try_keyword_token);
        }

        // FIX 2: Save ALL pending control flow states
        Value pending_exception = value_deep_copy(exception_from_try);
        int pending_exception_is_active = exception_occurred_in_try;

        Value pending_return_value = create_null_value();
        int pending_return_flag = interpreter->return_flag;
        if (pending_return_flag) {
            pending_return_value = value_deep_copy(interpreter->current_function_return_value);
        }

        int pending_break_flag = interpreter->break_flag;
        int pending_continue_flag = interpreter->continue_flag;

        // Temporarily clear flags for finally execution
        interpreter->exception_is_active = 0; // Temporarily clear for finally execution
        free_value_contents(interpreter->current_exception);
        interpreter->current_exception = create_null_value();
        interpreter->return_flag = 0;
        free_value_contents(interpreter->current_function_return_value);
        interpreter->current_function_return_value = create_null_value();
        interpreter->break_flag = 0;
        interpreter->continue_flag = 0;
        
        StatementExecStatus finally_block_status = execute_statements_in_controlled_block(interpreter, try_col, "finally", TOKEN_EOF, TOKEN_EOF, TOKEN_EOF);
        if (finally_block_status == STATEMENT_YIELDED_AWAIT) { // A yield happened inside the finally block
            yielded_in_block = true;
        }
        
        if (interpreter->exception_is_active || interpreter->return_flag || interpreter->break_flag || interpreter->continue_flag) { // If finally raised its own exception or control flow change
            free_value_contents(pending_exception); // Original/catch exception is superseded
            free_value_contents(pending_return_value); // Any pending return is also superseded
            // The new exception from finally is already in interpreter->current_exception and active.
        } else { // Finally completed without new exception, restore pending one
            interpreter->current_exception = value_deep_copy(pending_exception);
            interpreter->exception_is_active = pending_exception_is_active;
            free_value_contents(pending_exception);

            interpreter->current_function_return_value = value_deep_copy(pending_return_value);
            interpreter->return_flag = pending_return_flag;
            free_value_contents(pending_return_value);

            interpreter->break_flag = pending_break_flag;
            interpreter->continue_flag = pending_continue_flag;
        }
    } else if (exception_occurred_in_try) { // No finally, but an exception is still active
        free_value_contents(interpreter->current_exception);
        interpreter->current_exception = value_deep_copy(exception_from_try);
        interpreter->exception_is_active = 1;
    }
    free_value_contents(exception_from_try);

    if (!has_catch_or_finally) {
        report_error("Syntax", "'try' statement must be followed by at least one 'catch' or 'finally' clause.", try_keyword_token);
    }
    // Pop the frame
    interpreter->try_catch_stack_top = frame->prev;
    if (frame->catch_clause) {
        if (frame->catch_clause->variable_name) free(frame->catch_clause->variable_name);
        free(frame->catch_clause);
    }
    free(frame);
    free_token(try_keyword_token);

    if (yielded_in_block) {
        return STATEMENT_YIELDED_AWAIT;
    }

    if (interpreter->exception_is_active || interpreter->return_flag) { // If exception or return is active after try-catch-finally
        return STATEMENT_PROPAGATE_FLAG;
    }
    return status;
}

static bool is_builtin_module(const char* module_name) {
    if (!module_name) return false;
    // For now, only 'weaver' is built-in. This can be expanded later.
    if (strcmp(module_name, "weaver") == 0) {
        return true;
    }
    return false;
}

static void interpret_blueprint_statement(Interpreter* interpreter) {
    Token* blueprint_keyword_token = token_deep_copy(interpreter->current_token);
    int blueprint_def_col = blueprint_keyword_token->col; // This is now safe
    interpreter_eat(interpreter, TOKEN_BLUEPRINT); // Consume 'blueprint'
    interpreter_eat(interpreter, TOKEN_COLON); // Consume ':'

    if (interpreter->current_token->type != TOKEN_ID) {
        report_error("Syntax", "Expected blueprint name after 'blueprint:'.", interpreter->current_token);
    }
    char* bp_name_str = strdup(interpreter->current_token->value);
    if (!bp_name_str) {
        // bp_name_token not yet created, blueprint_keyword_token is the context
        report_error("System", "Failed to strdup blueprint name.", blueprint_keyword_token);
    }

    Token* bp_name_token = token_deep_copy(interpreter->current_token);
    interpreter_eat(interpreter, TOKEN_ID);

    Blueprint* new_bp = malloc(sizeof(Blueprint));
    if (!new_bp) { free(bp_name_str); free_token(bp_name_token); report_error("System", "Failed to allocate memory for Blueprint struct.", blueprint_keyword_token); }
    new_bp->name = bp_name_str;
    new_bp->parent_blueprint = NULL;
    new_bp->init_method_cache = NULL;

    new_bp->definition_col = blueprint_def_col; // Use the saved int, which is safer
    new_bp->class_attributes_and_methods = malloc(sizeof(Scope));
    if (!new_bp->class_attributes_and_methods) {
        free(new_bp->name); free(new_bp); free_token(bp_name_token);
        report_error("System", "Failed to allocate memory for blueprint scope.", blueprint_keyword_token);
    }
    new_bp->class_attributes_and_methods->symbols = NULL;
    new_bp->class_attributes_and_methods->outer = interpreter->current_scope; // Class scope can see outer scope

    // Handle inheritance
    if (interpreter->current_token->type == TOKEN_INHERITS) {
        interpreter_eat(interpreter, TOKEN_INHERITS);
        if (interpreter->current_token->type != TOKEN_ID) {
            report_error("Syntax", "Expected parent blueprint name after 'inherits'.", interpreter->current_token);
        }
        char* parent_name_str = strdup(interpreter->current_token->value);
        if (!parent_name_str) {
            free(new_bp->name); free(new_bp->class_attributes_and_methods); free(new_bp); free_token(bp_name_token);
            report_error("System", "Failed to strdup parent blueprint name.", interpreter->current_token);
        }

        Value* parent_val_ptr = symbol_table_get(interpreter->current_scope, parent_name_str);
        if (!parent_val_ptr || parent_val_ptr->type != VAL_BLUEPRINT) {
            char err_msg[200];
            sprintf(err_msg, "Parent blueprint '%s' not found or not a blueprint.", parent_name_str);
            free(parent_name_str); /* cleanup new_bp */ report_error("Runtime", err_msg, interpreter->current_token);
        }
        new_bp->parent_blueprint = parent_val_ptr->as.blueprint_val;
        free(parent_name_str);
        interpreter_eat(interpreter, TOKEN_ID);
    }

    // --- START: Stricter Syntax Check ---
    int blueprint_header_line = interpreter->current_token->line;
    interpreter_eat(interpreter, TOKEN_COLON);
    if (interpreter->current_token->line == blueprint_header_line && interpreter->current_token->type != TOKEN_EOF) {
        report_error("Syntax", "Unexpected token on the same line after blueprint signature. Expected a newline and an indented block.", interpreter->current_token);
    }
    if (interpreter->current_token->col <= blueprint_def_col) {
        report_error("Syntax", "Expected an indented block after 'blueprint' signature.", interpreter->current_token);
    }
    // --- END: Stricter Syntax Check ---

    // Process blueprint body (class attributes and methods)
    Scope* old_scope = interpreter->current_scope;
    interpreter->current_scope = new_bp->class_attributes_and_methods; // Set scope for 'let' and 'funct' inside blueprint

    while (interpreter->current_token->type != TOKEN_EOF && interpreter->current_token->col > blueprint_def_col) {

        if (interpreter->current_token->col != blueprint_def_col + 4 && interpreter->current_token->type != TOKEN_EOF) {
             char err_msg[300]; snprintf(err_msg, sizeof(err_msg), "Statement in blueprint '%s' has incorrect indentation. Expected col %d, got %d.", new_bp->name, blueprint_def_col + 4, interpreter->current_token->col);
             report_error("Syntax", err_msg, interpreter->current_token);
        }

        if (interpreter->current_token->type == TOKEN_LET) {
            interpret_let_statement(interpreter); // Will store in new_bp->class_attributes_and_methods
        } else if (interpreter->current_token->type == TOKEN_FUNCT) {
            int funct_stmt_start_col = interpreter->current_token->col; // Capture column of 'funct'
            interpret_funct_statement(interpreter, funct_stmt_start_col, false); // Pass column and false for async
            // Cache init_method if this was it
            Value* func_val = symbol_table_get_local(new_bp->class_attributes_and_methods, "init");
            if (func_val && func_val->type == VAL_FUNCTION) {
                new_bp->init_method_cache = func_val->as.function_val;
            }
        } else {
            report_error_unexpected_token(interpreter, "'let:' for class attribute or 'funct:' for method");
        }
        if (interpreter->exception_is_active) { interpreter->current_scope = old_scope; /* cleanup new_bp */ return; }
    }
    interpreter->current_scope = old_scope; // Restore original scope

    Value bp_val; bp_val.type = VAL_BLUEPRINT; bp_val.as.blueprint_val = new_bp;
    symbol_table_set(interpreter->current_scope, new_bp->name, bp_val); // Store blueprint in the outer scope

    // Add to global list of blueprints for cleanup
    BlueprintListNode* bp_list_node = malloc(sizeof(BlueprintListNode));
    if (!bp_list_node) { /* TODO: proper error handling and cleanup of new_bp if this fails */ report_error("System", "Failed to allocate BlueprintListNode.", bp_name_token); }
    bp_list_node->blueprint = new_bp;
    bp_list_node->next = interpreter->all_blueprints_head;
    interpreter->all_blueprints_head = bp_list_node;

    // bp_val (and new_bp) is now owned by the symbol table.
    free_token(bp_name_token);
    free_token(blueprint_keyword_token); // Free the deep copy we made
}

static void interpret_load_statement(Interpreter* interpreter) {
    // Token* load_keyword_token = interpreter->current_token; // Unused variable
    interpreter_eat(interpreter, TOKEN_LOAD);
    interpreter_eat(interpreter, TOKEN_COLON); // Consume ':'

    do {
        // These variables must be local to each iteration of the load statement
        char* module_source_identifier_str = NULL;
        bool explicit_as_keyword_used = false;
        char* alias_str = NULL;

        if (interpreter->current_token->type == TOKEN_ID || interpreter->current_token->type == TOKEN_STRING) { // Form 1: load: module [as alias]
            module_source_identifier_str = strdup(interpreter->current_token->value);
            Token* module_source_token = token_deep_copy(interpreter->current_token);
            interpreter_eat(interpreter, interpreter->current_token->type); // Eat ID or STRING

            if (interpreter->current_token->type == TOKEN_AS) {
                explicit_as_keyword_used = true;
                interpreter_eat(interpreter, TOKEN_AS);
                if (interpreter->current_token->type != TOKEN_ID) {
                    free(module_source_identifier_str);
                    free_token(module_source_token);
                    report_error("Syntax", "Expected alias name after 'as' in load statement.", interpreter->current_token);
                }
                alias_str = strdup(interpreter->current_token->value);
                interpreter_eat(interpreter, TOKEN_ID);
            }
            
            Value module_namespace_dict;
            bool is_builtin = is_builtin_module(module_source_identifier_str);
            if (is_builtin) {
                module_namespace_dict = get_or_create_builtin_module(interpreter, module_source_identifier_str, module_source_token);
            } else {
                char* abs_path = resolve_module_path(interpreter, module_source_identifier_str, module_source_token);
                module_namespace_dict = load_module_from_path(interpreter, abs_path, module_source_token);
                free(abs_path);
            }

            if (explicit_as_keyword_used) {
                symbol_table_set(interpreter->current_scope, alias_str, module_namespace_dict);
            } else {
                // If no 'as' alias is provided, use the module's own name as the variable name.
                symbol_table_set(interpreter->current_scope, module_source_identifier_str, module_namespace_dict);
            }

            // Cleanup for this iteration
            free_value_contents(module_namespace_dict);
            free(module_source_identifier_str);
            free_token(module_source_token);
            if (alias_str) free(alias_str);

        } else if (interpreter->current_token->type == TOKEN_LPAREN) { // Form 2: load: (items) from module
            interpreter_eat(interpreter, TOKEN_LPAREN);
            // For simplicity, let's assume a fixed max number of items to import for now
            #define MAX_LOAD_ITEMS 10
            char* item_names[MAX_LOAD_ITEMS];
            char* item_aliases[MAX_LOAD_ITEMS];
            int item_count = 0;

            while (interpreter->current_token->type != TOKEN_RPAREN && item_count < MAX_LOAD_ITEMS) {
                if (interpreter->current_token->type != TOKEN_ID) {
                    report_error("Syntax", "Expected item name in 'load from' list.", interpreter->current_token);
                }
                item_names[item_count] = strdup(interpreter->current_token->value);
                item_aliases[item_count] = item_names[item_count]; // Default alias
                interpreter_eat(interpreter, TOKEN_ID);

                if (interpreter->current_token->type == TOKEN_AS) {
                    interpreter_eat(interpreter, TOKEN_AS);
                    if (interpreter->current_token->type != TOKEN_ID) {
                        report_error("Syntax", "Expected alias for item in 'load from' list.", interpreter->current_token);
                    }
                    if (item_aliases[item_count] != item_names[item_count]) free(item_aliases[item_count]);
                    item_aliases[item_count] = strdup(interpreter->current_token->value);
                    interpreter_eat(interpreter, TOKEN_ID);
                }
                item_count++;
                if (interpreter->current_token->type == TOKEN_COMMA) interpreter_eat(interpreter, TOKEN_COMMA);
                else if (interpreter->current_token->type != TOKEN_RPAREN) {
                    report_error("Syntax", "Expected ',' or ')' in 'load from' item list.", interpreter->current_token);
                }
            }
            // After parsing items, current_token is RPAREN. Eat it.
            interpreter_eat(interpreter, TOKEN_RPAREN);
            // Next token must be FROM. Eat it.
            interpreter_eat(interpreter, TOKEN_FROM);
            char* module_name_or_path_str = NULL;
            Token* module_origin_token = token_deep_copy(interpreter->current_token); // For error reporting context

            if (interpreter->current_token->type == TOKEN_ID) {
                module_name_or_path_str = strdup(interpreter->current_token->value);
                interpreter_eat(interpreter, TOKEN_ID);
            } else if (interpreter->current_token->type == TOKEN_STRING) {
                module_name_or_path_str = strdup(interpreter->current_token->value); // Lexer handles unquoting
                interpreter_eat(interpreter, TOKEN_STRING);
            } else {
                free_token(module_origin_token);
                report_error("Syntax", "Expected module name (identifier or string path) after 'from' in load statement.", interpreter->current_token);
            }

            Value module_namespace;
            if (is_builtin_module(module_name_or_path_str)) {
                module_namespace = get_or_create_builtin_module(interpreter, module_name_or_path_str, module_origin_token);
            } else {
                char* abs_path = resolve_module_path(interpreter, module_name_or_path_str, module_origin_token);
                module_namespace = load_module_from_path(interpreter, abs_path, module_origin_token);
                free(abs_path);
            }

            for (int i = 0; i < item_count; ++i) {
                Value item_val = dictionary_get(module_namespace.as.dict_val, item_names[i], module_origin_token);
                symbol_table_set(interpreter->current_scope, item_aliases[i], item_val);
                free_value_contents(item_val);
            }
            free_value_contents(module_namespace);

            for (int i = 0; i < item_count; ++i) {
                if (item_aliases[i] != item_names[i])
                    free(item_aliases[i]);
                free(item_names[i]);
            }
            free(module_name_or_path_str);
            free_token(module_origin_token);
        } else {
            report_error_unexpected_token(interpreter, "a module name or '(' for item import list");
        }
        if (interpreter->current_token->type == TOKEN_COMMA)
            interpreter_eat(interpreter, TOKEN_COMMA);
        else
            break;
    } while (interpreter->current_token->type != TOKEN_COLON && interpreter->current_token->type != TOKEN_EOF);

    interpreter_eat(interpreter, TOKEN_COLON); // Final colon for the load statement
}

// Executes the body of an EchoC-defined coroutine.
// This function is called by the event loop when a coroutine is scheduled to run.
StatementExecStatus interpret_coroutine_body(Interpreter* interpreter, Coroutine* coro_to_run) {
    Scope* old_scope = interpreter->current_scope;

    Object* old_self_obj_ctx = interpreter->current_self_object;
    LexerState old_lexer_state = get_lexer_state(interpreter->lexer);
    Token* old_current_token = token_deep_copy(interpreter->current_token);
    
    // --- START of changes for coroutine-specific try-catch stack ---
    TryCatchFrame* old_try_catch_stack = interpreter->try_catch_stack_top;
    interpreter->try_catch_stack_top = coro_to_run->try_catch_stack_top;
    // --- END of changes for coroutine-specific try-catch stack ---

    // Set up interpreter context for this coroutine
    interpreter->current_scope = coro_to_run->execution_scope;
    // If resuming from an await, we use the more precise post_await_resume_state.
    // Otherwise (e.g., first run, or after a different kind of yield if introduced later),
    // we use the general statement_resume_state.
    if (coro_to_run->has_yielding_await_state) {
        interpreter->prevent_side_effects = true;
        DEBUG_PRINTF("CORO_BODY_EXEC: Resuming coro '%s'. Fast-forward mode ENABLED.", coro_to_run->name ? coro_to_run->name : "unnamed");
    }

    DEBUG_PRINTF("CORO_BODY_EXEC: Resuming coro '%s'. ALWAYS resuming from STATEMENT_RESUME state: Pos=%d, Line=%d, Col=%d", coro_to_run->name ? coro_to_run->name : "unnamed", coro_to_run->statement_resume_state.pos, coro_to_run->statement_resume_state.line, coro_to_run->statement_resume_state.col);
    // ALWAYS resume from the start of the statement that yielded.
    // The 'await' expression handler will detect it's a resume and fast-forward the expression parsing.
    set_lexer_state(interpreter->lexer, coro_to_run->statement_resume_state); 

    interpreter->current_self_object = NULL; // Coroutines are not methods in the OOP sense here

    interpreter->function_nesting_level++; // Increment nesting level for the coroutine's execution

    free_token(interpreter->current_token); 
    interpreter->current_token = get_next_token(interpreter->lexer); 

    interpreter->current_executing_coroutine = coro_to_run; // Set context AFTER setting up lexer
    interpreter->return_flag = 0; // Reset for this coroutine execution slice

    DEBUG_PRINTF("CORO_BODY_EXEC: Starting/Resuming coro %s (%p). Scope: %p. Lexer state: Pos=%d, Line=%d, Col=%d. Token: %s ('%s')",
                 coro_to_run->name ? coro_to_run->name : "unnamed", (void*)coro_to_run,
                 (void*)interpreter->current_scope,
                 interpreter->lexer->pos, interpreter->lexer->line, interpreter->lexer->col,
                 token_type_to_string(interpreter->current_token->type),
                 interpreter->current_token->value ? interpreter->current_token->value : "N/A");

    if (!coro_to_run->function_def) {
        report_error("Internal", "interpret_coroutine_body called on coroutine with no function_def.", interpreter->current_token);
        goto cleanup_and_return_coro_body; // Use goto for cleanup path
    }

    bool returned = false;

    while (true) {
        // Check for function end condition BEFORE executing a statement
        if ((interpreter->current_token->col <= coro_to_run->function_def->definition_col &&
             interpreter->current_token->type != TOKEN_EOF) ||
            interpreter->current_token->type == TOKEN_EOF)
        {
            // Natural completion of the coroutine body.
            coro_to_run->state = CORO_DONE;
            free_value_contents(coro_to_run->result_value);
            coro_to_run->result_value = create_null_value(); // Implicit return of null.
            break; // Exit the while loop
        }

		// Save resume state BEFORE executing the statement.
		if (coro_to_run) {
			coro_to_run->statement_resume_state = get_lexer_state_for_token_start(interpreter->lexer, interpreter->current_token->line, interpreter->current_token->col, interpreter->current_token);
		}

        DEBUG_PRINTF("CORO_BODY_EXEC: About to interpret statement. Token: %s ('%s') at L%d C%d",
             token_type_to_string(interpreter->current_token->type),
             interpreter->current_token->value ? interpreter->current_token->value : "N/A",
             interpreter->current_token->line, interpreter->current_token->col);
        StatementExecStatus status = interpret_statement(interpreter);

        if (status == STATEMENT_YIELDED_AWAIT) {
            // The coroutine yielded. Its state is set to SUSPENDED by await.
            // We need to break out of this execution loop. The resume state is already saved.
            break;
        } else if (status == STATEMENT_PROPAGATE_FLAG) {
            // A 'return' or 'raise' occurred.
            if (interpreter->return_flag) {
                returned = true;
                coro_to_run->state = CORO_DONE;
                free_value_contents(coro_to_run->result_value);
                coro_to_run->result_value = value_deep_copy(interpreter->current_function_return_value);
                interpreter->return_flag = 0;
            } else if (interpreter->exception_is_active) {
                coro_to_run->state = CORO_DONE;
                coro_to_run->has_exception = 1;
                free_value_contents(coro_to_run->exception_value);
                coro_to_run->exception_value = value_deep_copy(interpreter->current_exception);
                interpreter->exception_is_active = 0;
                free_value_contents(interpreter->current_exception);
                interpreter->current_exception = create_null_value();
            }
            break; // Exit the while loop
        }
    }

    // After the loop, if it exited due to a return, check for unreachable code.
    if (returned) {
        if (interpreter->current_token->type != TOKEN_EOF && interpreter->current_token->col > coro_to_run->function_def->definition_col) {
            report_error("Syntax", "Unreachable code after 'return:' statement.", interpreter->current_token);
        }
    }

cleanup_and_return_coro_body:
    DEBUG_PRINTF("CORO_BODY_CLEANUP: Coro %s (%p). Final state: %d. Overall status for event loop: %d",
                 coro_to_run->name ? coro_to_run->name : "unnamed", (void*)coro_to_run,
                 coro_to_run->state, coro_to_run->state == CORO_SUSPENDED_AWAIT ? STATEMENT_YIELDED_AWAIT : STATEMENT_EXECUTED_OK);

    // --- START of changes for coroutine-specific try-catch stack (cleanup) ---
    // Save the (potentially modified) stack top back to the coroutine
    coro_to_run->try_catch_stack_top = interpreter->try_catch_stack_top;
    // Restore the interpreter's original stack top
    interpreter->try_catch_stack_top = old_try_catch_stack;
    // --- END of changes for coroutine-specific try-catch stack (cleanup) ---
    interpreter->function_nesting_level--; // Decrement nesting level
    interpreter->current_executing_coroutine = NULL; // Unset context
    interpreter->current_scope = old_scope;
    interpreter->current_self_object = old_self_obj_ctx;
    set_lexer_state(interpreter->lexer, old_lexer_state);
    free_token(interpreter->current_token);
    interpreter->current_token = old_current_token;

    return (coro_to_run->state == CORO_SUSPENDED_AWAIT) ? STATEMENT_YIELDED_AWAIT : STATEMENT_EXECUTED_OK;
}