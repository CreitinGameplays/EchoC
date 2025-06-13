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

// Forward declarations for static functions within this file
static void interpret_show_statement(Interpreter* interpreter);
static void interpret_let_statement(Interpreter* interpreter);
static void interpret_if_statement(Interpreter* interpreter);
static void interpret_block_statement(Interpreter* interpreter);
static void skip_statements_in_branch(Interpreter* interpreter);
static void interpret_loop_statement(Interpreter* interpreter); // Keep this
static void interpret_while_loop(Interpreter* interpreter, int loop_col, int loop_line); // Pass loop_line
static void interpret_for_loop(Interpreter* interpreter, int loop_col, int loop_line);
static void interpret_funct_statement(Interpreter* interpreter, int statement_start_col, bool is_async_param);
static void interpret_break_statement(Interpreter* interpreter);
static void interpret_continue_statement(Interpreter* interpreter);
static void interpret_return_statement(Interpreter* interpreter);
static void interpret_expression_statement(Interpreter* interpreter); // Added for function call statements
static void interpret_raise_statement(Interpreter* interpreter); // New
static void interpret_try_statement(Interpreter* interpreter);   // New
static void execute_statements_in_controlled_block(Interpreter* interpreter, int expected_indent_col, const char* block_type_for_error, TokenType terminator1, TokenType terminator2, TokenType terminator3);
static void execute_loop_body_iteration(Interpreter* interpreter, int loop_start_col, int expected_body_indent_col, const char* loop_type_for_error);
static void interpret_load_statement(Interpreter* interpreter); // For module loading
static void interpret_blueprint_statement(Interpreter* interpreter); // For OOP
static void interpret_run_statement(Interpreter* interpreter); // Forward declaration

// Modified execute_statements_in_branch to use the more generic execute_statements_in_controlled_block
extern void add_to_ready_queue(Interpreter* interpreter, Coroutine* coro); // From interpreter.c or async_manager.c
extern void run_event_loop(Interpreter* interpreter); // From interpreter.c or async_manager.c
static void execute_statements_in_branch(Interpreter* interpreter, int expected_indent_col, const char* branch_type_for_error) {
    execute_statements_in_controlled_block(interpreter, expected_indent_col, branch_type_for_error, TOKEN_ELIF, TOKEN_ELSE, TOKEN_END);
}

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
    } else if (interpreter->current_token->type == TOKEN_ASYNC) { 
        Token* async_token_ref = interpreter->current_token;
        int start_col_for_async_funct = async_token_ref->col;
        interpreter_eat(interpreter, TOKEN_ASYNC);
        if (interpreter->current_token->type != TOKEN_FUNCT) report_error_unexpected_token(interpreter, "'funct' after 'async'");
        interpret_funct_statement(interpreter, start_col_for_async_funct, true);
    } else if (interpreter->current_token->type == TOKEN_FUNCT) {
        interpret_funct_statement(interpreter, interpreter->current_token->col, false);
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

static void interpret_expression_statement(Interpreter* interpreter) {
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
    if (first_token_in_expr->type == TOKEN_ID && expr_res.is_standalone_primary_id) {
        // An identifier was parsed, but no operations (call, access, arithmetic etc.) were applied to it.
        // This is not a valid statement on its own.
        free_value_contents(expr_res.value); // Free the value of the standalone ID
        report_error("Syntax", "An identifier by itself is not a valid statement. Must be a function/method call or part of an assignment.", first_token_in_expr);
    }
    free_token(first_token_in_expr);

    Value result = expr_res.value;

    if (interpreter->exception_is_active) { // If expression evaluation raised an exception
        // If fresh, free it. If not, it's owned elsewhere.
        if (expr_res.is_freshly_created_container) free_value_contents(result);
        return;
    }

    // The result of an expression statement is discarded.
    if (expr_res.is_freshly_created_container) { // Only free if it was a fresh container
        free_value_contents(result);
    }

    // interpret_expression_statement's interpret_ternary_expr might leave current_token on COLON
    if (interpreter->current_token->type == TOKEN_COLON) {
        interpreter_eat(interpreter, TOKEN_COLON);
    }
}

static void interpret_show_statement(Interpreter* interpreter) {
    interpreter_eat(interpreter, TOKEN_SHOW);
    interpreter_eat(interpreter, TOKEN_COLON);
    ExprResult expr_res = interpret_expression(interpreter);
    // Changed from interpret_ternary_expr to interpret_expression
    Value result = expr_res.value;

    if (interpreter->exception_is_active) {
        free_value_contents(result);
        return;
    }
    
    char* str_repr = value_to_string_representation(result, interpreter, interpreter->current_token);
    printf("%s\n", str_repr);
    free(str_repr);
    if (expr_res.is_freshly_created_container) { // Only free if it was a fresh container
        free_value_contents(result);
    }

    interpreter_eat(interpreter, TOKEN_COLON);
}

// Helper function to perform the actual indexed assignment
// target_container: The direct container (array or dict) to modify
// final_index: The index/key to use on target_container
// value_to_set: The value to assign
// error_token: For error reporting context
// base_var_name: Name of the original variable for error messages
static void perform_indexed_assignment(Value* target_container, Value final_index, Value value_to_set, Token* error_token, const char* base_var_name) {
    if (target_container->type == VAL_ARRAY) {
        if (final_index.type != VAL_INT) {
            free_value_contents(final_index); free_value_contents(value_to_set);
            report_error("Runtime", "Array index for assignment must be an integer.", error_token);
        }
        Array* arr = target_container->as.array_val;
        long idx = final_index.as.integer;
        long effective_idx = idx;
        if (effective_idx < 0) effective_idx += arr->count;

        if (effective_idx < 0 || effective_idx >= arr->count) {
            char err_msg[150];
            sprintf(err_msg, "Array assignment index %ld out of bounds for array '%s' (size %d).", idx, base_var_name, arr->count);
            free_value_contents(final_index); free_value_contents(value_to_set);
            report_error("Runtime", err_msg, error_token);
        }
        free_value_contents(arr->elements[effective_idx]); // Free old element
        arr->elements[effective_idx] = value_deep_copy(value_to_set);
    } else if (target_container->type == VAL_DICT) {
        if (final_index.type != VAL_STRING) {
            free_value_contents(final_index); free_value_contents(value_to_set);
            report_error("Runtime", "Dictionary key for assignment must be a string.", error_token);
        }
        dictionary_set(target_container->as.dict_val, final_index.as.string_val, value_to_set, error_token);
    } else if (target_container->type == VAL_TUPLE) {
        free_value_contents(final_index); free_value_contents(value_to_set);
        report_error("Runtime", "Tuples are immutable and cannot be modified.", error_token);
    } else {
        char err_msg[150];
        sprintf(err_msg, "Cannot apply indexed assignment to variable '%s' of type %d.", base_var_name, target_container->type);
        free_value_contents(final_index); free_value_contents(value_to_set);
        report_error("Runtime", err_msg, error_token);
    }
    free_value_contents(final_index); // final_index is consumed
    // value_to_set is consumed by deep_copy or dictionary_set which makes its own copy
}

static void interpret_let_statement(Interpreter* interpreter) {
    interpreter_eat(interpreter, TOKEN_LET);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'let'

    Token* var_name_token = interpreter->current_token;
    if (var_name_token->type != TOKEN_ID) {
        report_error("Syntax", "Expected variable name after 'let:'", var_name_token);
    }
    char* var_name_str = strdup(var_name_token->value);
    interpreter_eat(interpreter, TOKEN_ID);

    // Handle 'let: self.attribute = value'
    if (strcmp(var_name_str, "self") == 0 && interpreter->current_token->type == TOKEN_DOT) { // Starts with self.
        if (!interpreter->current_self_object) {
            free(var_name_str);
            report_error("Runtime", "'self' can only be used within an instance method.", var_name_token);
        }
        interpreter_eat(interpreter, TOKEN_DOT); // Eat '.'
        Token* attr_name_token = interpreter->current_token;
        if (attr_name_token->type != TOKEN_ID) {
            free(var_name_str);
            report_error("Syntax", "Expected attribute name after 'self.'.", attr_name_token);
        }
        char* attr_name_str = strdup(attr_name_token->value);
        interpreter_eat(interpreter, TOKEN_ID); // Eat attribute name

        if (interpreter->current_token->type == TOKEN_LBRACKET) { // self.attribute[index] = value
            Object* self_obj = interpreter->current_self_object;
            Value* container_val_ptr = symbol_table_get_local(self_obj->instance_attributes, attr_name_str);
            if (!container_val_ptr) {
                // Could also check blueprint attributes if self.CLASS_ATTR[idx] was allowed (not currently supported this way)
                char err_msg[200]; sprintf(err_msg, "Attribute '%s' not found on 'self' for indexed assignment.", attr_name_str);
                free(var_name_str); free(attr_name_str); report_error("Runtime", err_msg, attr_name_token);
            }

            Value final_index_val;
            interpreter_eat(interpreter, TOKEN_LBRACKET);
            ExprResult index_res = interpret_expression(interpreter);
            // Changed from interpret_ternary_expr to interpret_expression
            final_index_val = index_res.value; // This is an R-value, needs freeing if complex
            interpreter_eat(interpreter, TOKEN_RBRACKET);

            interpreter_eat(interpreter, TOKEN_ASSIGN);
            ExprResult rhs_res = interpret_expression(interpreter);
            // Changed from interpret_ternary_expr to interpret_expression
            Value val_to_set = rhs_res.value;
            if (interpreter->exception_is_active) { free(var_name_str); free(attr_name_str); free_value_contents(final_index_val); free_value_contents(val_to_set); return; }

            perform_indexed_assignment(container_val_ptr, final_index_val, val_to_set, attr_name_token, attr_name_str);
            // perform_indexed_assignment frees final_index_val.
            // val_to_set is deep_copied by perform_indexed_assignment. If rhs_res.is_freshly_created_container, free it.
            if (rhs_res.is_freshly_created_container) free_value_contents(val_to_set);
            
        } else if (interpreter->current_token->type == TOKEN_ASSIGN) { // self.attribute = value
            interpreter_eat(interpreter, TOKEN_ASSIGN); 
            ExprResult val_expr_res = interpret_expression(interpreter);
            // Changed from interpret_ternary_expr to interpret_expression
            Value val_to_assign = val_expr_res.value;
            if (interpreter->exception_is_active) { free(var_name_str); free(attr_name_str); free_value_contents(val_to_assign); return; }
            symbol_table_set(interpreter->current_self_object->instance_attributes, attr_name_str, val_to_assign);
            if (val_expr_res.is_freshly_created_container) free_value_contents(val_to_assign); // symbol_table_set made a deep copy
        } else {
            free(var_name_str); free(attr_name_str);
            report_error_unexpected_token(interpreter, "'[' for indexed assignment or '=' for attribute assignment after 'self.attribute'");
        }
        free(attr_name_str); free(var_name_str);
        interpreter_eat(interpreter, TOKEN_COLON); return;
    }
    Value* current_val_ptr = symbol_table_get(interpreter->current_scope, var_name_str);

    if (interpreter->current_token->type == TOKEN_LBRACKET) { // Indexed assignment
        if (!current_val_ptr) {
            char err_msg[150];
            sprintf(err_msg, "Variable '%s' must be an existing collection for indexed assignment with 'let:'.", var_name_str);
            free(var_name_str);
            report_error("Runtime", err_msg, var_name_token);
        }

        Value* target_container_ptr = current_val_ptr;
        Value final_index_val;
        int has_final_index = 0;
        // Changed from interpret_ternary_expr to interpret_expression

        while (interpreter->current_token->type == TOKEN_LBRACKET) {
            if (has_final_index) { // Navigating to a deeper level
                if (target_container_ptr->type == VAL_ARRAY) {
                    if (final_index_val.type != VAL_INT) { free(var_name_str); free_value_contents(final_index_val); report_error("Runtime", "Array index must be an integer for multi-level assignment.", var_name_token); }
                    Array* arr = target_container_ptr->as.array_val;
                    long idx = final_index_val.as.integer;
                    if (idx < 0) idx += arr->count;
                    if (idx < 0 || idx >= arr->count) { char err_msg[100]; sprintf(err_msg, "Index out of bounds for array '%s'", var_name_str); free(var_name_str); free_value_contents(final_index_val); report_error("Runtime", err_msg, var_name_token); }
                    target_container_ptr = &arr->elements[idx];
                } else if (target_container_ptr->type == VAL_DICT) {
                    char err_msg[250]; // Increased buffer size
                    sprintf(err_msg, "Multi-level dictionary assignment like dict[k1][k2]=v is not directly supported for '%s'. Assign to dict[k1] first if it's a sub-dictionary.", var_name_str);
                    free(var_name_str); free_value_contents(final_index_val);
                    report_error("Runtime", err_msg, var_name_token);
                } else { free(var_name_str); free_value_contents(final_index_val); report_error("Runtime", "Cannot apply further indexing, intermediate value is not a collection.", var_name_token); }
                free_value_contents(final_index_val);
                has_final_index = 0;
            }

            interpreter_eat(interpreter, TOKEN_LBRACKET);
            ExprResult final_idx_res = interpret_expression(interpreter);
            final_index_val = final_idx_res.value;
            if (interpreter->exception_is_active) { free(var_name_str); if(has_final_index) free_value_contents(final_index_val); return; }
            interpreter_eat(interpreter, TOKEN_RBRACKET);
            has_final_index = 1;

            if (interpreter->current_token->type != TOKEN_ASSIGN && interpreter->current_token->type != TOKEN_LBRACKET) {
                char err_msg[200];
                sprintf(err_msg, "Expected '=' or '[' for further indexing after ']', but got %s.", token_type_to_string(interpreter->current_token->type));
                free(var_name_str); free_value_contents(final_index_val);
                report_error("Syntax", err_msg, interpreter->current_token);
            }
            if (interpreter->current_token->type == TOKEN_ASSIGN) break;
        }

        if (!has_final_index) { report_error("Internal", "Logic error in indexed assignment parsing for 'let:'.", var_name_token); }

        interpreter_eat(interpreter, TOKEN_ASSIGN); // Expect '='
        ExprResult new_val_res = interpret_expression(interpreter);
        // Changed from interpret_ternary_expr to interpret_expression
        Value new_value_to_assign = new_val_res.value;

        if (interpreter->exception_is_active) {
            free(var_name_str); free_value_contents(final_index_val); free_value_contents(new_value_to_assign); return;
        }

        // perform_indexed_assignment will free final_index_val.
        // new_value_to_assign is deep_copied by perform_indexed_assignment.
        perform_indexed_assignment(target_container_ptr, final_index_val, new_value_to_assign, var_name_token, var_name_str); 
        if (new_val_res.is_freshly_created_container) {
            free_value_contents(new_value_to_assign);
        }
        // final_index_val is freed by perform_indexed_assignment

    } else if (interpreter->current_token->type == TOKEN_ASSIGN) { // Simple assignment
        interpreter_eat(interpreter, TOKEN_ASSIGN);
        ExprResult val_expr_res = interpret_expression(interpreter);
        // Changed from interpret_ternary_expr to interpret_expression
        Value val_to_assign = val_expr_res.value;
        if (interpreter->exception_is_active) {
            free(var_name_str); free_value_contents(val_to_assign);
            return;
        }

        // New logic for 'let: var = value': update if exists in any scope, else create in current.
        VarScopeInfo var_info = get_variable_definition_scope_and_value(interpreter->current_scope, var_name_str);
        if (var_info.value_ptr) { // Variable exists in current or an outer scope
            DEBUG_PRINTF("LET_STATEMENT: Updating existing variable '%s' in its definition scope %p.", var_name_str, (void*)var_info.definition_scope);
            free_value_contents(*(var_info.value_ptr));       // Free old value's contents
            *(var_info.value_ptr) = value_deep_copy(val_to_assign); // Assign new deep-copied value
        } else { // Variable does not exist, create in current scope
            DEBUG_PRINTF("LET_STATEMENT: Creating new variable '%s' in current scope %p.", var_name_str, (void*)interpreter->current_scope);
            symbol_table_set(interpreter->current_scope, var_name_str, val_to_assign);
        }

        if (val_expr_res.is_freshly_created_container) {
            free_value_contents(val_to_assign); // symbol_table_set made a deep copy
        }

    } else {
        char err_msg[200];
        sprintf(err_msg, "Expected '[' for indexed assignment or '=' for simple assignment after variable name '%s', but got %s.",
                var_name_str, token_type_to_string(interpreter->current_token->type));
        free(var_name_str);
        report_error("Syntax", err_msg, interpreter->current_token);
    }

    free(var_name_str);
    interpreter_eat(interpreter, TOKEN_COLON);
}

static void interpret_block_statement(Interpreter* interpreter) {
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
        if (interpreter->exception_is_active) { // If statement inside block raised
            exit_scope(interpreter); return; // Still need to exit scope
        }
        interpret_statement(interpreter);
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
}

static void skip_statements_in_branch(Interpreter* interpreter) {
    // This function is primarily for skipping 'if'/'elif'/'else' branches.
    // For skipping to a loop's 'end:', a more targeted skip is needed.
    int nesting_level = 0; // General nesting for if/loop within the branch
    Token* last_token_before_eof_skip = interpreter->current_token;

    while (interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->type == TOKEN_IF || interpreter->current_token->type == TOKEN_LOOP) {
            nesting_level++;
        } else if (interpreter->current_token->type == TOKEN_END) {
            if (nesting_level == 0) return; 
            nesting_level--;
        } else if (nesting_level == 0 &&
                   (interpreter->current_token->type == TOKEN_ELIF ||
                    interpreter->current_token->type == TOKEN_ELSE)) {
            if (interpreter->exception_is_active) { // Exception during skip? Unlikely but check.
                return;
            }
            return; 
        }
        last_token_before_eof_skip = interpreter->current_token;
        Token* old_token = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(old_token);
    }
    if (interpreter->exception_is_active) { // Exception during skip?
        return;
    }
    if (nesting_level >= 0 && interpreter->current_token->type == TOKEN_EOF) {
        char err_msg[300];
        snprintf(err_msg, sizeof(err_msg), 
                 "Unexpected EOF while skipping statements. Missing 'end:' for an 'if' or 'loop' structure that started near line %d, col %d?",
                 last_token_before_eof_skip->line, last_token_before_eof_skip->col);
        report_error("Syntax", err_msg, last_token_before_eof_skip);
    }
}

static void skip_to_loop_end(Interpreter* interpreter, int target_loop_col) {
    int loop_nesting_level = 0; // Relative to the current loop being skipped
    // Corrected to handle all block types that use 'end:'
    Token* last_token_before_eof_skip = interpreter->current_token;

    while(interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->type == TOKEN_IF ||
            interpreter->current_token->type == TOKEN_LOOP ||
            interpreter->current_token->type == TOKEN_FUNCT ||
            interpreter->current_token->type == TOKEN_TRY ||
            interpreter->current_token->type == TOKEN_BLUEPRINT) {
            loop_nesting_level++;
        } else if (interpreter->current_token->type == TOKEN_END) {
            if (loop_nesting_level == 0 && interpreter->current_token->col == target_loop_col) {
                return; // Found the matching 'end:'
            }
            if (loop_nesting_level > 0) {
                loop_nesting_level--;
            }
        }
        last_token_before_eof_skip = interpreter->current_token;
        Token* old_token = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(old_token);
    }
    if (interpreter->exception_is_active) { // Exception during skip?
        return;
    }
    report_error("Syntax", "Unexpected EOF while skipping to loop 'end:'. Missing 'end:'?", last_token_before_eof_skip);
}

static void execute_statements_in_controlled_block(Interpreter* interpreter, int expected_indent_col, const char* block_type_for_error, TokenType terminator1, TokenType terminator2, TokenType terminator3) {
    Token* last_token_in_block = interpreter->current_token;
    while (interpreter->current_token->type != terminator1 &&
           interpreter->current_token->type != terminator2 &&
           interpreter->current_token->type != terminator3 &&
           interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->col != expected_indent_col && interpreter->current_token->type != TOKEN_EOF) { // EOF col doesn't matter
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg),
                     "Statement in '%s' block has incorrect indentation. Expected column %d, got column %d.",
                     block_type_for_error, expected_indent_col, interpreter->current_token->col);
            report_error("Syntax", err_msg, interpreter->current_token);
        }
        last_token_in_block = interpreter->current_token;
        interpret_statement(interpreter);
        // If interpret_statement processed a break, continue, return, or raised an exception, stop executing this block
        if (interpreter->break_flag || interpreter->continue_flag || interpreter->return_flag || interpreter->exception_is_active) {
            return;
        }
    }
    if (interpreter->current_token->type == TOKEN_EOF &&
        interpreter->current_token->type != terminator1 &&
        interpreter->current_token->type != terminator2 &&
        interpreter->current_token->type != terminator3) {
        char err_msg[300];
         snprintf(err_msg, sizeof(err_msg), "Unexpected EOF in '%s' block. Missing '%s', '%s', or '%s' to terminate?",
                 block_type_for_error,
                 token_type_to_string(terminator1),
                 token_type_to_string(terminator2),
                 token_type_to_string(terminator3));
        report_error("Syntax", err_msg, last_token_in_block);
    }
}

static void execute_loop_body_iteration(Interpreter* interpreter, int loop_start_col, int expected_body_indent_col, const char* loop_type_for_error) {
    Token* last_token_in_block = interpreter->current_token;
    (void)last_token_in_block; // Potentially unused if loop body is empty or errors early

    // Loop as long as the current token is part of the loop body
    while (interpreter->current_token->type != TOKEN_EOF &&
           !(interpreter->current_token->type == TOKEN_END && interpreter->current_token->col == loop_start_col) && // Not the loop's own end
           interpreter->current_token->col >= expected_body_indent_col) { // Still indented as part of the body
        // Check if the current token signifies the end of the loop structure itself.

        if (interpreter->current_token->col != expected_body_indent_col) {
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg),
                     "Statement in '%s' loop body has incorrect indentation. Expected column %d, got column %d.",
                     loop_type_for_error, expected_body_indent_col, interpreter->current_token->col);
            report_error("Syntax", err_msg, interpreter->current_token);
        }

        // last_token_in_block = interpreter->current_token; // Update before each statement
        interpret_statement(interpreter);

        if (interpreter->break_flag || interpreter->continue_flag || interpreter->return_flag || interpreter->exception_is_active) {
            // Stop this iteration if break, continue, return, or an exception was encountered/raised.
            // The calling loop construct (e.g., interpret_for_loop) will then handle this flag.
            return; 
        }
    }

    if (interpreter->current_token->type == TOKEN_EOF &&
        !(interpreter->current_token->type == TOKEN_END && interpreter->current_token->col == loop_start_col)) {
        // This means EOF was hit before the loop's 'end:' token.
        report_error("Syntax", "Unexpected EOF in loop body. Missing 'end:' to close loop?", interpreter->current_token); // Use current_token (EOF) for context
    }
}

static void interpret_if_statement(Interpreter* interpreter) {
    Token* if_keyword_token_for_context = interpreter->current_token;
    int if_col = if_keyword_token_for_context->col; // Column of the 'if' keyword
    int if_line = if_keyword_token_for_context->line; // Line of the 'if' keyword
    interpreter_eat(interpreter, TOKEN_IF);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'if'
    ExprResult cond_res = interpret_expression(interpreter);
    // Changed from interpret_ternary_expr to interpret_expression
    Value condition = cond_res.value;
    if (interpreter->exception_is_active) {
        free_value_contents(condition);
        return;
    }

    interpreter_eat(interpreter, TOKEN_COLON);
        // Changed from interpret_ternary_expr to interpret_expression

    // Check indentation of the first statement in the 'if' body, if one exists.
    // A branch is "empty" if the next token is elif/else/end/eof.
    if (interpreter->current_token->type != TOKEN_ELIF &&
        interpreter->current_token->type != TOKEN_ELSE &&
        interpreter->current_token->type != TOKEN_END &&
        interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->col != if_col + 4) {
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg),
                     "First statement in 'if' branch has incorrect indentation. Expected column %d, got column %d.",
                     if_col + 4, interpreter->current_token->col);
            report_error("Syntax", err_msg, interpreter->current_token);
        }
    }

    if (condition.type != VAL_BOOL) {
        free_value_contents(condition);
        report_error("Runtime", "Condition for 'if' statement must be a boolean.", if_keyword_token_for_context); // Use the token before it's eaten if possible, or pass line/col
    }
    int condition_is_true = condition.as.bool_val;
    free_value_contents(condition); // Condition value is now used or discarded

    int executed_a_branch = 0;

    if (condition_is_true) {
        executed_a_branch = 1;
        execute_statements_in_branch(interpreter, if_col + 4, "if");
        if (interpreter->exception_is_active) { // Exception in if branch
            // The exception will propagate. We need to ensure 'end:' is still processed or skipped.
            // For now, skip_statements_in_branch will handle finding the next elif/else/end.
            skip_statements_in_branch(interpreter); // Skip to next structural token
        }
    } else {
        skip_statements_in_branch(interpreter);
        if (interpreter->exception_is_active) return; // Propagate if skip itself had an issue (unlikely)
    }

    while (interpreter->current_token->type == TOKEN_ELIF) {
        Token* elif_token = interpreter->current_token;
        if (elif_token->col != if_col) {
            char err_msg[200];
            snprintf(err_msg, sizeof(err_msg),
                     "'elif:' keyword (column %d) is not aligned with the preceding 'if:' (column %d).",
                     elif_token->col, if_col);
            report_error("Syntax", err_msg, elif_token);
        }

        interpreter_eat(interpreter, TOKEN_ELIF);
        interpreter_eat(interpreter, TOKEN_COLON);
        ExprResult elif_cond_res = interpret_expression(interpreter);
        Value elif_condition = elif_cond_res.value;
        if (interpreter->exception_is_active) {
            free_value_contents(elif_condition);
            return; // Propagate
        }

        interpreter_eat(interpreter, TOKEN_COLON);

        // Check indentation of the first statement in the 'elif' body, if one exists.
        if (interpreter->current_token->type != TOKEN_ELIF && // Not another elif
            interpreter->current_token->type != TOKEN_ELSE &&
            interpreter->current_token->type != TOKEN_END &&
            interpreter->current_token->type != TOKEN_EOF) {
            if (interpreter->current_token->col != if_col + 4) { // Still relative to the original if_col
                char err_msg[300];
                snprintf(err_msg, sizeof(err_msg),
                         "First statement in 'elif' branch has incorrect indentation. Expected column %d, got column %d.",
                         if_col + 4, interpreter->current_token->col);
                report_error("Syntax", err_msg, interpreter->current_token);
            }
        }

        if (executed_a_branch) { // If a previous branch (if or elif) was true
            free_value_contents(elif_condition); // Still need to parse and free condition
            skip_statements_in_branch(interpreter);
            if (interpreter->exception_is_active) return;
        } else {
            // If an exception occurred in a previous skip_statements_in_branch, it would have returned.
            // So, if we are here, no exception is active from prior skips.
            if (elif_condition.type != VAL_BOOL) {
                free_value_contents(elif_condition);
                report_error("Runtime", "Condition for 'elif' statement must be a boolean.", elif_token);
            }
            int elif_is_true = elif_condition.as.bool_val;
            free_value_contents(elif_condition);

            if (elif_is_true) {
                executed_a_branch = 1;
                execute_statements_in_branch(interpreter, if_col + 4, "elif"); // elif body also indented relative to if_col
                if (interpreter->exception_is_active) { // Exception in elif branch
                    skip_statements_in_branch(interpreter); // Skip to next structural token
                }
            } else {
                skip_statements_in_branch(interpreter);
                if (interpreter->exception_is_active) return;

            }
        }
    }

    if (interpreter->current_token->type == TOKEN_ELSE) {
        Token* else_token = interpreter->current_token;
        if (else_token->col != if_col) {
            char err_msg[200];
            snprintf(err_msg, sizeof(err_msg),
                     "'else:' keyword (column %d) is not aligned with the preceding 'if:' (column %d).",
                     else_token->col, if_col);
            report_error("Syntax", err_msg, else_token);
        }

        interpreter_eat(interpreter, TOKEN_ELSE);
        interpreter_eat(interpreter, TOKEN_COLON);
        // Check indentation of the first statement in the 'else' body, if one exists.
        if (interpreter->current_token->type != TOKEN_END && // Not the end
            interpreter->current_token->type != TOKEN_EOF) {
            if (interpreter->current_token->col != if_col + 4) { // Still relative to the original if_col
                char err_msg[300];
                snprintf(err_msg, sizeof(err_msg),
                         "First statement in 'else' branch has incorrect indentation. Expected column %d, got column %d.",
                         if_col + 4, interpreter->current_token->col);
                report_error("Syntax", err_msg, interpreter->current_token);
            }
        }

        if (executed_a_branch) {
            skip_statements_in_branch(interpreter);
            if (interpreter->exception_is_active) return;
        } else {
            execute_statements_in_branch(interpreter, if_col + 4, "else"); // else body also indented relative to if_col
            if (interpreter->exception_is_active) { // Exception in else branch
                // No further branches to skip, 'end:' is next or error.
            }
        }
    }

    if (interpreter->current_token->type == TOKEN_END) {
        Token* end_token = interpreter->current_token;
        if (end_token->col != if_col) {
            char err_msg[250];
            snprintf(err_msg, sizeof(err_msg),
                     "'end:' keyword (column %d) is not aligned with the 'if:' (column %d) it closes.",
                     end_token->col, if_col);
            report_error("Syntax", err_msg, end_token);
        }
        interpreter_eat(interpreter, TOKEN_END);
        interpreter_eat(interpreter, TOKEN_COLON);
    } else {
        if (interpreter->exception_is_active) { // If an exception is already propagating, don't report new syntax error
            return;
        }
        char err_msg[256];
        sprintf(err_msg, "Expected 'end:' (aligned with 'if:' at column %d) to close 'if/elif/else' structure that started near line %d, col %d. Found %s ('%s') at column %d.", 
                if_col,
                if_line, if_col,
                token_type_to_string(interpreter->current_token->type),
                interpreter->current_token->value ? interpreter->current_token->value : "",
                (int)interpreter->current_token->col);
        report_error("Syntax", err_msg, interpreter->current_token);
    }
}

static void interpret_break_statement(Interpreter* interpreter) {
    Token* break_token = interpreter->current_token;
    interpreter_eat(interpreter, TOKEN_BREAK);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'break'

    if (interpreter->loop_depth == 0) {
        report_error("Syntax", "'break:' statement found outside of a loop.", break_token);
    }
    interpreter->break_flag = 1;
}

static void interpret_continue_statement(Interpreter* interpreter) {
    Token* continue_token = interpreter->current_token;
    interpreter_eat(interpreter, TOKEN_CONTINUE);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'continue'

    if (interpreter->loop_depth == 0) {
        report_error("Syntax", "'continue:' statement found outside of a loop.", continue_token);
    }
    interpreter->continue_flag = 1;
}

static void interpret_funct_statement(Interpreter* interpreter, int statement_start_col, bool is_async_param) {
    Token* funct_token_original_ref = interpreter->current_token;
    int funct_def_col = statement_start_col; // Use the passed start column
    interpreter_eat(interpreter, TOKEN_FUNCT);
    interpreter_eat(interpreter, TOKEN_COLON);
    if (interpreter->current_token->type != TOKEN_ID) {
        report_error("Syntax", "Expected function name after 'funct:'.", interpreter->current_token);
    }
    char* func_name_str = strdup(interpreter->current_token->value);
    if (!func_name_str) {
        report_error("System", "Failed to strdup function name.", interpreter->current_token);
    }
    interpreter_eat(interpreter, TOKEN_ID);

    Function* new_func = malloc(sizeof(Function));
    if (!new_func) {
        free(func_name_str);
        report_error("System", "Failed to allocate memory for Function struct.", funct_token_original_ref);
    }

    new_func->name = func_name_str;
    new_func->params = NULL;
    new_func->param_count = 0;
    new_func->is_async = is_async_param;
    new_func->definition_scope = interpreter->current_scope; // Lexical scope
    // Point to the lexer's current text; value_deep_copy will strdup it for the stored version.
    new_func->source_text_owned_copy = (char*)interpreter->lexer->text; // Cast away const for temporary assignment
    new_func->source_text_length = interpreter->lexer->text_length;
    new_func->is_source_owner = false; // This temporary Function struct does not own the source text yet.

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
                ExprResult default_val_expr_res = interpret_ternary_expr(interpreter); // Default value is an expression
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
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after parameters

    // Capture body start state
    // Ensure the first token of the body (if any) is correctly indented.
    // The body itself starts on the next line, indented by 4 spaces relative to `funct_def_col`.
    if (interpreter->current_token->type != TOKEN_END && // Not an empty function ending immediately
        interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->col != funct_def_col + 4) {
            char err_msg[300];
            snprintf(err_msg, sizeof(err_msg),
                     "First statement in function '%s' body has incorrect indentation. Expected column %d, got column %d.",
                     new_func->name, funct_def_col + 4, interpreter->current_token->col);
            report_error("Syntax", err_msg, interpreter->current_token);
        }
    }    
    // Capture body start state for the *current_token* which is the first token of the body.
    new_func->body_start_state = get_lexer_state_for_token_start(interpreter->lexer,
                                                                 interpreter->current_token->line,
                                                                 interpreter->current_token->col,
                                                                 interpreter->current_token);

    // Pre-scan to find the matching 'end:' token for this function
    int nesting_level = 0;
    Token* found_end_token_info = NULL; // Store info of the found end token
    LexerState state_before_prescan = get_lexer_state(interpreter->lexer);
    Token* token_to_restore_after_prescan = token_deep_copy(interpreter->current_token); // Current token is LPAREN or first param
    Token* last_token_of_scan = NULL;

    // Pre-scan loop using interpreter->current_token directly 
    while(interpreter->current_token->type != TOKEN_EOF) {
        DEBUG_PRINTF("Function Pre-scan for '%s': Token %s ('%s') at L%d C%d, Nesting: %d, TargetCol: %d",
            new_func->name ? new_func->name : "<NULL_FUNC_NAME>",
            token_type_to_string(interpreter->current_token->type),
            interpreter->current_token->value ? interpreter->current_token->value : "",
            interpreter->current_token->line, interpreter->current_token->col,
            nesting_level, funct_def_col);        
        if ( // interpreter->current_token->type == TOKEN_FUNCT || // Functions are not nested
            interpreter->current_token->type == TOKEN_IF ||
            interpreter->current_token->type == TOKEN_LOOP ||
            interpreter->current_token->type == TOKEN_TRY) { // try also has an end
            // For these, we only increment nesting if they are at a deeper or equal indent than the current function's body statements would be
            nesting_level++;
        } else if (interpreter->current_token->type == TOKEN_END) {
            if (nesting_level == 0 && interpreter->current_token->type == TOKEN_END && interpreter->current_token->col == funct_def_col) {
                found_end_token_info = token_deep_copy(interpreter->current_token);
                last_token_of_scan = interpreter->current_token; // END token itself
                break;
            }
            if (nesting_level > 0) nesting_level--;
        } // Add other block-like structures if necessary

        // Ensure the pre-scan respects indentation for finding the correct 'end:'
        // This simple nesting count might not be robust enough for all cases if 'end:'
        // can be used for different structures at different indentations.
        Token* temp_tok_to_free = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(temp_tok_to_free);
    }
    if (!found_end_token_info) { // Loop terminated by EOF
        last_token_of_scan = interpreter->current_token; // EOF token
    }

    // Restore lexer and current_token to their state *before* the pre-scan.
    set_lexer_state(interpreter->lexer, state_before_prescan);
    // Free the token that was live at the end of the scan (END or EOF).
    // token_to_restore_after_prescan is a distinct deep copy.
    if (last_token_of_scan) {
        free_token(last_token_of_scan);
    }
    interpreter->current_token = token_to_restore_after_prescan; // Assign the deep copied token back.

    if (!found_end_token_info) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Unclosed function definition for '%s'. Missing 'end:' aligned at column %d?",
                 new_func->name, funct_def_col); 
        // Cleanup before error:
        free(new_func->name);
        if (new_func->params) {
            for (int i = 0; i < new_func->param_count; ++i) {
                free(new_func->params[i].name);
                if (new_func->params[i].default_value) {
                    free_value_contents(*(new_func->params[i].default_value));
                    free(new_func->params[i].default_value);
                }
            }
            free(new_func->params);        }
        // interpreter->current_token (restored token_to_restore_after_prescan) will be used for error context
        // and freed by the main loop or caller of interpret_statement.
        report_error("Syntax", err_msg, interpreter->current_token);
    }


    new_func->body_end_token_original_line = found_end_token_info->line;
    new_func->body_end_token_original_col = found_end_token_info->col;
    free_token(found_end_token_info);

    // Now, consume (skip over) the body tokens and the final 'end:' for this definition
    while(!(interpreter->current_token->type == TOKEN_END &&
            interpreter->current_token->line == new_func->body_end_token_original_line &&
            interpreter->current_token->col == new_func->body_end_token_original_col) &&
          interpreter->current_token->type != TOKEN_EOF) {
        Token* temp_tok_to_free = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(temp_tok_to_free);
    }

    if (interpreter->current_token->type == TOKEN_END &&
        interpreter->current_token->line == new_func->body_end_token_original_line &&
        interpreter->current_token->col == new_func->body_end_token_original_col) {
        interpreter_eat(interpreter, TOKEN_END);
        interpreter_eat(interpreter, TOKEN_COLON);
    } else {
        // This case should ideally not be reached if pre-scan was correct and robust
        // and the main loop of skipping tokens also worked.
        report_error("Internal", "Failed to correctly skip to function's end token during definition.", funct_token_original_ref);
    }

    // After consuming the function's 'end:', 'interpreter->current_token' is now on the token *after* 'end:'.
    // No further advancement is needed here as interpreter_eat handles it.
    // The main interpret loop will pick up from this new current_token.
    Value func_val; func_val.type = VAL_FUNCTION; func_val.as.function_val = new_func;
    symbol_table_set(interpreter->current_scope, new_func->name, func_val);
    // Since symbol_table_set performs a deep copy (including allocating a new Function struct
    // and copying its contents), the original 'new_func' created in this function
    // and its dynamically allocated members are now orphaned if not explicitly freed.
    
    free(new_func->name); 
    new_func->name = NULL;
    if (new_func->params) {
        for (int i = 0; i < new_func->param_count; ++i) {
            free(new_func->params[i].name);
            new_func->params[i].name = NULL;
            if (new_func->params[i].default_value) {
                free_value_contents(*(new_func->params[i].default_value));
                free(new_func->params[i].default_value);
                new_func->params[i].default_value = NULL;
            }
        }
        free(new_func->params);
        new_func->params = NULL;
    }
    free(new_func); // Free the original Function struct itself
}

static void interpret_return_statement(Interpreter* interpreter) {
    Token* return_keyword_token = interpreter->current_token;
    interpreter_eat(interpreter, TOKEN_RETURN);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'return'

    if (interpreter->function_nesting_level == 0) {
        report_error("Syntax", "'return:' statement found outside of a function.", return_keyword_token);
    }

    ExprResult ret_expr_res = interpret_expression(interpreter);
    Value return_expr_val = ret_expr_res.value;
    if (interpreter->exception_is_active) {
        free_value_contents(return_expr_val);
        return;
    }

    interpreter_eat(interpreter, TOKEN_COLON);

    free_value_contents(interpreter->current_function_return_value); // Free previous (e.g. default null)
    interpreter->current_function_return_value = value_deep_copy(return_expr_val);
    interpreter->return_flag = 1;

    if (ret_expr_res.is_freshly_created_container) {
        free_value_contents(return_expr_val); // return_expr_val was copied by value_deep_copy
    }
}

static void interpret_loop_statement(Interpreter* interpreter) {
    Token* loop_keyword_token_for_context = interpreter->current_token;
    int loop_col = loop_keyword_token_for_context->col;
    int loop_line = loop_keyword_token_for_context->line; // Keep loop_line for error context
    interpreter_eat(interpreter, TOKEN_LOOP);
    interpreter_eat(interpreter, TOKEN_COLON);

    if (interpreter->current_token->type == TOKEN_WHILE) {
        interpret_while_loop(interpreter, loop_col, loop_line);
    } else if (interpreter->current_token->type == TOKEN_FOR) {
        interpret_for_loop(interpreter, loop_col, loop_line);
    } else {
        char err_msg[200];
        sprintf(err_msg, "Expected 'while' or 'for' after 'loop:', but got %s.",
                token_type_to_string(interpreter->current_token->type));
        report_error("Syntax", err_msg, interpreter->current_token); // Error at current token, loop_keyword_token_for_context is eaten
    }

    // Ensure 'end:' closes the loop structure
    if (interpreter->current_token->type == TOKEN_END) {
        Token* end_token = interpreter->current_token;
        if (end_token->col != loop_col) {
            char err_msg[250];
            snprintf(err_msg, sizeof(err_msg),
                    "'end:' keyword (column %d) is not aligned with the 'loop:' (column %d) it closes.",
                    end_token->col, loop_col);
            report_error("Syntax", err_msg, end_token);
        }
        interpreter_eat(interpreter, TOKEN_END);
        interpreter_eat(interpreter, TOKEN_COLON);
    } else {
        if (interpreter->exception_is_active) { // If an exception is already propagating
            return;
        }
        // If 'end:' is not found immediately, it's a structural error.
        // The loop body execution should have positioned the token correctly,
        // or skip_statements_in_branch (if break/continue occurred) should have.
        // If we are here, it means the loop body didn't end where expected or a break/continue path is flawed.
        char err_msg[256];
        sprintf(err_msg, "Expected 'end:' (aligned with 'loop:' at column %d) to close 'loop' structure that started near line %d, col %d. Found %s ('%s') at column %d.",
                loop_col,
                loop_line, loop_col, // Use saved line and column of 'loop:'
                token_type_to_string(interpreter->current_token->type),
                interpreter->current_token->value ? interpreter->current_token->value : "",
                (int)interpreter->current_token->col);
        report_error("Syntax", err_msg, interpreter->current_token);
    }
}

static void interpret_while_loop(Interpreter* interpreter, int loop_col, int loop_line) {
    // Token* while_keyword_token = interpreter->current_token; // Was unused
    interpreter_eat(interpreter, TOKEN_WHILE); // Consume 'while'
    // After eating TOKEN_WHILE, interpreter->current_token is the first token of the condition.
    // We need to capture the LexerState that corresponds to the *start* of this token.
    LexerState condition_start_lexer_state = get_lexer_state_for_token_start(interpreter->lexer, 
                                                                             interpreter->current_token->line,
                                                                             interpreter->current_token->col,
                                                                             interpreter->current_token); // Pass current token for error context
    (void)loop_line;
    interpreter->loop_depth++;
    while(1) {
        Token* condition_start_token = interpreter->current_token;
        ExprResult cond_res = interpret_expression(interpreter);
        // Changed from interpret_ternary_expr to interpret_expression
        Value condition = cond_res.value;
        if (interpreter->exception_is_active) {
            free_value_contents(condition);
            interpreter->loop_depth--; return;
        }

        interpreter_eat(interpreter, TOKEN_COLON); // Expect colon after condition

        if (condition.type != VAL_BOOL) {
            free_value_contents(condition);
            interpreter->loop_depth--; // Decrement before erroring out
            report_error("Runtime", "Condition for 'while' loop must be a boolean.", condition_start_token);
        }

        if (!condition.as.bool_val) {
            free_value_contents(condition);
            skip_to_loop_end(interpreter, loop_col);
            if (interpreter->exception_is_active) { interpreter->loop_depth--; return; }
            break; // Exit the C while(1) loop
        }
        free_value_contents(condition);

        // Check indentation of the first statement in the loop body, if one exists.
        int body_actual_indent = loop_col + 4; // Body indented 4 spaces relative to 'loop:'
        if (interpreter->current_token->type != TOKEN_END && interpreter->current_token->type != TOKEN_EOF) {
            if (interpreter->current_token->col != body_actual_indent) {
                char err_msg[300];
                snprintf(err_msg, sizeof(err_msg),
                         "First statement in 'while' loop body has incorrect indentation. Expected column %d, got column %d.",
                         body_actual_indent, interpreter->current_token->col);
                report_error("Syntax", err_msg, interpreter->current_token);
            }
        } else if (interpreter->current_token->type == TOKEN_END && interpreter->current_token->col != loop_col) {
             char err_msg[250];
            snprintf(err_msg, sizeof(err_msg),
                     "'end:' keyword (column %d) for 'while' loop is not aligned with the 'loop:' (column %d). Loop body might be empty or missing.",
                     interpreter->current_token->col, loop_col);
            report_error("Syntax", err_msg, interpreter->current_token);
        } // Pass loop_col for the 'loop:' keyword's column, and body_actual_indent for the body statements
        execute_loop_body_iteration(interpreter, loop_col, body_actual_indent, "while");

        if (interpreter->break_flag) {
            interpreter->break_flag = 0;
            skip_to_loop_end(interpreter, loop_col);
            if (interpreter->exception_is_active) { interpreter->loop_depth--; return; }
            break; // Exit C while(1)
        }
        if (interpreter->exception_is_active) { interpreter->loop_depth--; return; } // Exception from body
        if (interpreter->continue_flag) {
            interpreter->continue_flag = 0; // Reset for this loop level
            // Body has been executed or skipped by execute_loop_body_iteration.
            // Rewind to re-evaluate the condition for the next iteration.
            // The body has already been skipped by execute_loop_body_iteration returning.
            rewind_lexer_and_token(interpreter, condition_start_lexer_state, NULL); 
            continue; // Continue the C while(1) loop to re-evaluate condition
        }

        // If execute_loop_body_iteration returned because it saw TOKEN_END at loop_col,
        // current_token will be that TOKEN_END. The outer C while loop condition will handle termination.
        // If it returned due to dedent, current_token is the dedented token.
        // Rewind to re-evaluate condition for the next iteration
        if (interpreter->return_flag) { // Check for return_flag after body execution
            interpreter->loop_depth--;
            // Do not reset return_flag here.
            skip_to_loop_end(interpreter, loop_col); // Ensure parser is past the loop
            return; // Propagate the return
        }

        if (interpreter->exception_is_active) { // Exception from body
            // Ensure we skip to the loop's end before propagating the exception fully
            skip_to_loop_end(interpreter, loop_col);
            interpreter->loop_depth--; return;
        }
        rewind_lexer_and_token(interpreter, condition_start_lexer_state, NULL);
    }
    interpreter->loop_depth--;
    // Clear flags if this was the outermost loop being exited by break/natural end
    if (interpreter->loop_depth == 0) {
        interpreter->break_flag = 0;
        interpreter->continue_flag = 0;
    }
}

static void interpret_for_loop(Interpreter* interpreter, int loop_col, int loop_line) {
    (void)loop_line; // Mark as unused
    interpreter_eat(interpreter, TOKEN_FOR);
    // Changed from interpret_ternary_expr to interpret_expression
    Token* var_name_token = interpreter->current_token;
    if (var_name_token->type != TOKEN_ID) {
        report_error("Syntax", "Expected identifier for loop variable after 'for'.", var_name_token);
    }
    char* var_name_str = strdup(var_name_token->value);
    interpreter_eat(interpreter, TOKEN_ID);

    interpreter->loop_depth++;
    int body_indent = loop_col + 4; // Body indented 4 spaces relative to 'loop:'

    if (interpreter->current_token->type == TOKEN_FROM) { // Range loop
        interpreter_eat(interpreter, TOKEN_FROM);
        ExprResult start_val_expr_res = interpret_expression(interpreter);
        Value start_val_expr = start_val_expr_res.value;
        if (interpreter->exception_is_active) { free(var_name_str); free_value_contents(start_val_expr); interpreter->loop_depth--; return; }

        interpreter_eat(interpreter, TOKEN_TO);
        ExprResult end_val_expr_res = interpret_expression(interpreter);
        Value end_val_expr = end_val_expr_res.value;
        if (interpreter->exception_is_active) { free(var_name_str); free_value_contents(start_val_expr); free_value_contents(end_val_expr); interpreter->loop_depth--; return; }

        Value step_val_expr; step_val_expr.type = VAL_INT; step_val_expr.as.integer = 1; // Default

        if (interpreter->current_token->type == TOKEN_STEP) {
            interpreter_eat(interpreter, TOKEN_STEP);
            ExprResult step_val_expr_res = interpret_expression(interpreter);
            step_val_expr = step_val_expr_res.value; // Overwrite default if step provided
            if (interpreter->exception_is_active) {
                free(var_name_str); free_value_contents(start_val_expr); free_value_contents(end_val_expr); free_value_contents(step_val_expr);
                interpreter->loop_depth--; return;
            }
        }

        interpreter_eat(interpreter, TOKEN_COLON); // Consumes COLON. current_token is now first token of body.
        // current_token is now the first token of the loop body.
        // We need the LexerState corresponding to the *start* of this token.
        LexerState loop_body_start_lexer_state = get_lexer_state_for_token_start(interpreter->lexer,
                                                                                 interpreter->current_token->line,
                                                                                 interpreter->current_token->col,
                                                                                 interpreter->current_token);

        if (!((start_val_expr.type == VAL_INT || start_val_expr.type == VAL_FLOAT) &&
              (end_val_expr.type == VAL_INT || end_val_expr.type == VAL_FLOAT) &&
              (step_val_expr.type == VAL_INT || step_val_expr.type == VAL_FLOAT))) {
            free(var_name_str); free_value_contents(start_val_expr); free_value_contents(end_val_expr); if(interpreter->current_token->type != TOKEN_COLON) free_value_contents(step_val_expr);
            interpreter->loop_depth--; report_error("Runtime", "Start, end, and step values for 'for...from...to' loop must be numbers.", var_name_token);
        }
        double start_d = (start_val_expr.type == VAL_INT) ? start_val_expr.as.integer : start_val_expr.as.floating;
        double end_d = (end_val_expr.type == VAL_INT) ? end_val_expr.as.integer : end_val_expr.as.floating;
        double step_d = (step_val_expr.type == VAL_INT) ? step_val_expr.as.integer : step_val_expr.as.floating;
        free_value_contents(start_val_expr); free_value_contents(end_val_expr); if(interpreter->current_token->type != TOKEN_COLON && step_val_expr.type != VAL_INT) free_value_contents(step_val_expr);

        if (step_d == 0) { free(var_name_str); interpreter->loop_depth--; report_error("Runtime", "Step value in 'for...from...to' loop cannot be zero.", var_name_token); }

        // Check for no-iteration condition before starting the loop
        int no_iterations = 0;
        if (step_d > 0 && start_d > end_d) {
            no_iterations = 1;
        } else if (step_d < 0 && start_d < end_d) {
            no_iterations = 1;
        }

        if (no_iterations) {
            skip_to_loop_end(interpreter, loop_col);
            if (interpreter->exception_is_active) { interpreter->loop_depth--; free(var_name_str); return; }
        } else {
            for (double i = start_d; (step_d > 0) ? (i <= end_d) : (i >= end_d); i += step_d) {
                DEBUG_PRINTF("FOR_LOOP (Range): Iteration start. C var i = %f. Current scope before enter_scope: %p", i, (void*)interpreter->current_scope);
                enter_scope(interpreter);
                DEBUG_PRINTF("FOR_LOOP (Range): After enter_scope. New current scope: %p", (void*)interpreter->current_scope);
                Value loop_var_val;
                if (fmod(i, 1.0) == 0.0) { loop_var_val.type = VAL_INT; loop_var_val.as.integer = (long)i; }
                else { loop_var_val.type = VAL_FLOAT; loop_var_val.as.floating = i; }

                if (strcmp(var_name_str, "i") == 0) { // Specific log for the 'i' variable from the test, C var is also 'i'
                    DEBUG_PRINTF("  Preparing to set EchoC var '%s'. C var i = %f.", var_name_str, i);
                    if (loop_var_val.type == VAL_INT) DEBUG_PRINTF("    loop_var_val: Type: VAL_INT, Value: %ld", loop_var_val.as.integer);
                    else DEBUG_PRINTF("    loop_var_val: Type: VAL_FLOAT, Value: %f", loop_var_val.as.floating);
                }

                symbol_table_set(interpreter->current_scope, var_name_str, loop_var_val); // loop_var_val is copied

                // Rewind to the start of the loop body for this iteration
                rewind_lexer_and_token(interpreter, loop_body_start_lexer_state, NULL);

                if (interpreter->current_token->type != TOKEN_END && interpreter->current_token->type != TOKEN_EOF && interpreter->current_token->col != body_indent) {
                    char err_msg[300];
                    snprintf(err_msg, sizeof(err_msg),
                             "First statement in 'for...from...to' loop body has incorrect indentation. Expected column %d, got column %d.",
                             body_indent, interpreter->current_token->col);
                    report_error("Syntax", err_msg, interpreter->current_token);
                }
                DEBUG_PRINTF("FOR_LOOP (Range): Executing loop body. Current scope: %p", (void*)interpreter->current_scope);
                execute_loop_body_iteration(interpreter, loop_col, body_indent, "for...from...to");
                // Check for return_flag after body execution, before exit_scope for this iteration
                if (interpreter->return_flag) {
                    exit_scope(interpreter); // Still need to exit the iteration's scope
                    interpreter->loop_depth--;
                    skip_to_loop_end(interpreter, loop_col); // Ensure parser is past the loop
                    goto end_for_range_loop_label; // Use goto to break out of C for-loop and then handle cleanup
                }
                exit_scope(interpreter);
                if (interpreter->exception_is_active) { interpreter->loop_depth--; free(var_name_str); return; }
                if (interpreter->break_flag) { 
                    interpreter->break_flag = 0; 
                    // skip_to_loop_end will be called after the C for-loop
                    if (interpreter->exception_is_active) { interpreter->loop_depth--; free(var_name_str); return; }
                    goto end_for_range_loop_label; // Use goto to break out of the C for-loop
                }
                if (interpreter->continue_flag) { 
                    interpreter->continue_flag = 0; 
                    // C for loop will naturally continue to next `i`.
                    // execute_loop_body_iteration would have handled the current iteration's statements.
                    // If an exception occurred during continue's processing within the body,
                    // it should have been caught by the exception_is_active check above.
                    // No explicit rewind needed here as the C for loop handles iteration.
                }
            }
            end_for_range_loop_label:; // Label for break to jump to
            if (interpreter->return_flag) { // If loop exited due to return_flag
                free(var_name_str); // var_name_str was allocated in this function
                return; // Propagate the return (loop_depth already decremented)
            }        }
        // After the C for-loop (natural finish, break, or no iterations for range loop),
        // ensure the parser is positioned at the EchoC loop's 'end:' token.
        if (!interpreter->exception_is_active) { // Don't skip if an exception is already propagating
            skip_to_loop_end(interpreter, loop_col);
        }

    } else if (interpreter->current_token->type == TOKEN_IN) { // Collection loop
        interpreter_eat(interpreter, TOKEN_IN);
        ExprResult coll_res = interpret_expression(interpreter);
        Value collection_val = coll_res.value;
        if (interpreter->exception_is_active) { free(var_name_str); free_value_contents(collection_val); interpreter->loop_depth--; return; }

        interpreter_eat(interpreter, TOKEN_COLON); // Consumes COLON. current_token is now first token of body.
        // current_token is now the first token of the loop body.
        // We need the LexerState corresponding to the *start* of this token.
        LexerState loop_body_start_lexer_state = get_lexer_state_for_token_start(interpreter->lexer,
                                                                                 interpreter->current_token->line,
                                                                                 interpreter->current_token->col,
                                                                                 interpreter->current_token);

        if (collection_val.type != VAL_ARRAY && collection_val.type != VAL_STRING && collection_val.type != VAL_DICT) {
            free(var_name_str); free_value_contents(collection_val); interpreter->loop_depth--;
            report_error("Runtime", "Collection in 'for...in' loop must be an array, string, or dictionary.", var_name_token);
        }
        
        int break_all_collection_loops = 0; // Use int for boolean flag
        if (collection_val.type == VAL_ARRAY) {
            Array* arr = collection_val.as.array_val;
            if (arr->count == 0) {
                skip_to_loop_end(interpreter, loop_col);
                if (interpreter->exception_is_active) { interpreter->loop_depth--; free(var_name_str); free_value_contents(collection_val); return; }
            } else {
                for (int i = 0; i < arr->count && !break_all_collection_loops; ++i) {
                    enter_scope(interpreter); 
                    symbol_table_set(interpreter->current_scope, var_name_str, arr->elements[i]); 
                    rewind_lexer_and_token(interpreter, loop_body_start_lexer_state, NULL);
                    if (interpreter->current_token->type != TOKEN_END && interpreter->current_token->type != TOKEN_EOF && interpreter->current_token->col != body_indent) {
                        char err_msg[300];
                        snprintf(err_msg, sizeof(err_msg),
                                 "First statement in 'for...in array' loop body has incorrect indentation. Expected column %d, got column %d.",
                                 body_indent, interpreter->current_token->col);
                        report_error("Syntax", err_msg, interpreter->current_token);
                    }
                    execute_loop_body_iteration(interpreter, loop_col, body_indent, "for...in array");
                    if (interpreter->return_flag) { // Check for return_flag
                        exit_scope(interpreter);
                        interpreter->loop_depth--;
                        skip_to_loop_end(interpreter, loop_col);
                        break_all_collection_loops = 1; // Signal outer C loop to terminate
                        // No need to free var_name_str or collection_val yet, handled after loop
                        // but we need to ensure they are freed if we return early.
                        goto end_for_collection_loop_label;
                    }                    exit_scope(interpreter);
                    if (interpreter->exception_is_active) { break_all_collection_loops = 1; /* Will be handled below */ }
                    if (interpreter->break_flag) {
                        // break_flag is set, the C for loop (`for (int i = 0;...`) will terminate due to `break_all_collection_loops`.
                        // skip_to_loop_end will be called after the C for-loop.
                        interpreter->break_flag = 0; skip_to_loop_end(interpreter, loop_col); if (interpreter->exception_is_active) {break_all_collection_loops=1;} break_all_collection_loops = 1; 
                    }
                    if (interpreter->continue_flag) { 
                        interpreter->continue_flag = 0; 
                    }
                }
            }
        } else if (collection_val.type == VAL_STRING) {
            char* str = collection_val.as.string_val;
            if (str[0] == '\0') {
                skip_to_loop_end(interpreter, loop_col);
                if (interpreter->exception_is_active) { interpreter->loop_depth--; free(var_name_str); free_value_contents(collection_val); return; }
            } else {
                for (int i = 0; str[i] != '\0' && !break_all_collection_loops; ++i) {
                    enter_scope(interpreter); Value char_val; char_val.type = VAL_STRING; char_val.as.string_val = malloc(2); char_val.as.string_val[0] = str[i]; char_val.as.string_val[1] = '\0';
                    symbol_table_set(interpreter->current_scope, var_name_str, char_val); free_value_contents(char_val); // char_val copied, then original freed
                    rewind_lexer_and_token(interpreter, loop_body_start_lexer_state, NULL);
                    if (interpreter->current_token->type != TOKEN_END && interpreter->current_token->type != TOKEN_EOF && interpreter->current_token->col != body_indent) {
                        char err_msg[300];
                        snprintf(err_msg, sizeof(err_msg),
                                 "First statement in 'for...in string' loop body has incorrect indentation. Expected column %d, got column %d.",
                                 body_indent, interpreter->current_token->col);
                        report_error("Syntax", err_msg, interpreter->current_token);
                    }
                    execute_loop_body_iteration(interpreter, loop_col, body_indent, "for...in string");
                    if (interpreter->return_flag) {
                        exit_scope(interpreter);
                        interpreter->loop_depth--;
                        skip_to_loop_end(interpreter, loop_col);
                        break_all_collection_loops = 1;
                        goto end_for_collection_loop_label;

                    }
                    exit_scope(interpreter);
                    if (interpreter->exception_is_active) { break_all_collection_loops = 1; }
                    if (interpreter->break_flag) { 
                        interpreter->break_flag = 0; skip_to_loop_end(interpreter, loop_col); if (interpreter->exception_is_active) {break_all_collection_loops=1;} break_all_collection_loops = 1; 
                    }
                    if (interpreter->continue_flag) { 
                        interpreter->continue_flag = 0; 
                    }
                }
            }
        } else if (collection_val.type == VAL_DICT) {
            // Note: Dictionary iteration order is not guaranteed.
            Dictionary* dict = collection_val.as.dict_val;
            if (dict->count == 0) {
                skip_to_loop_end(interpreter, loop_col);
                if (interpreter->exception_is_active) { interpreter->loop_depth--; free(var_name_str); free_value_contents(collection_val); return; }
            } else {
                for (int bucket_idx = 0; bucket_idx < dict->num_buckets && !break_all_collection_loops; ++bucket_idx) {
                    DictEntry* entry = dict->buckets[bucket_idx];
                    while (entry) {
                        enter_scope(interpreter); Value key_val; key_val.type = VAL_STRING; key_val.as.string_val = strdup(entry->key);
                        symbol_table_set(interpreter->current_scope, var_name_str, key_val); free_value_contents(key_val); // key_val copied, then original freed
                        rewind_lexer_and_token(interpreter, loop_body_start_lexer_state, NULL);
                        if (interpreter->current_token->type != TOKEN_END && interpreter->current_token->type != TOKEN_EOF && interpreter->current_token->col != body_indent) {
                            char err_msg[300];
                            snprintf(err_msg, sizeof(err_msg),
                                     "First statement in 'for...in dictionary' loop body has incorrect indentation. Expected column %d, got column %d.",
                                     body_indent, interpreter->current_token->col);
                            report_error("Syntax", err_msg, interpreter->current_token);
                        }
                        execute_loop_body_iteration(interpreter, loop_col, body_indent, "for...in dictionary");
                        if (interpreter->return_flag) {
                            exit_scope(interpreter);
                            interpreter->loop_depth--;
                            skip_to_loop_end(interpreter, loop_col);
                            break_all_collection_loops = 1;
                            entry = NULL; // Ensure inner while loop terminates
                            goto end_for_collection_loop_label; // Break outer for loop as well
                        }                        exit_scope(interpreter);
                        if (interpreter->exception_is_active) { break_all_collection_loops = 1; break; }
                        if (interpreter->break_flag) { 
                            interpreter->break_flag = 0; skip_to_loop_end(interpreter, loop_col); if (interpreter->exception_is_active) {break_all_collection_loops=1; break;} break_all_collection_loops = 1; break; 
                        }
                        if (interpreter->continue_flag) { 
                            interpreter->continue_flag = 0; 
                        }
                        entry = entry->next;
                    }
                }
            }
        }
        end_for_collection_loop_label:;
        if (interpreter->return_flag) { // If loop exited due to return_flag
            if (coll_res.is_freshly_created_container) free_value_contents(collection_val);
            free(var_name_str);
            return; // Propagate the return (loop_depth already decremented)
        }
        if (coll_res.is_freshly_created_container) {
            free_value_contents(collection_val);
        }
        if (break_all_collection_loops && interpreter->exception_is_active) { // Exception from body caused loop termination
            interpreter->loop_depth--; free(var_name_str); return;
        }
        // If loop terminated naturally or by break, ensure we are positioned after the body
        // skip_to_loop_end is usually called inside the break logic, but if it's a natural finish,
        // execute_loop_body_iteration should leave us at the correct spot or this ensures it.
        if (!interpreter->exception_is_active) skip_to_loop_end(interpreter, loop_col);
        
    } else {
        free(var_name_str); interpreter->loop_depth--;
        report_error("Syntax", "Expected 'from' or 'in' after 'for <variable>'.", interpreter->current_token);
    }

    free(var_name_str);
    interpreter->loop_depth--;
    if (interpreter->loop_depth == 0) { // Clear flags if this was the outermost loop
        // If this loop was part of a coroutine that is now finishing,
        // these flags should be local to the coroutine's execution context,
        // or reset when a coroutine yields/completes.
        // For now, global reset is fine for synchronous code.
        interpreter->break_flag = 0;
        interpreter->continue_flag = 0;
    }
}

static void interpret_raise_statement(Interpreter* interpreter) {
    Token* raise_token = interpreter->current_token;
    interpreter_eat(interpreter, TOKEN_RAISE);
    interpreter_eat(interpreter, TOKEN_COLON); // Colon after 'raise'

    ExprResult err_val_res = interpret_expression(interpreter);
    Value error_val = err_val_res.value;
    if (interpreter->exception_is_active) { // If expression itself had an error
        free_value_contents(error_val); // Free partially evaluated error_val
        return; // Propagate the original error
    }

    interpreter_eat(interpreter, TOKEN_COLON);

    if (error_val.type != VAL_STRING) {
        char* val_str_repr = value_to_string_representation(error_val, interpreter, raise_token);
        char err_msg[300]; // Pass interpreter to value_to_string_representation if op_str is involved
        snprintf(err_msg, sizeof(err_msg), "Can only raise a string value as an exception. Got type for value '%s'.", val_str_repr);
        free(val_str_repr);
        free_value_contents(error_val);
        report_error("Runtime", err_msg, raise_token); // This exits
    }

    // Free any existing exception before setting the new one
    free_value_contents(interpreter->current_exception);
    interpreter->current_exception = value_deep_copy(error_val); // error_val is now owned by current_exception
    interpreter->exception_is_active = 1;

    free_value_contents(error_val); // Free the original expression result as it's copied
    DEBUG_PRINTF("RAISED EXCEPTION: %s", interpreter->current_exception.as.string_val);
}

static void interpret_try_statement(Interpreter* interpreter) {
    Token* try_keyword_token = interpreter->current_token;
    int try_col = try_keyword_token->col;
    interpreter_eat(interpreter, TOKEN_TRY); // Consume 'try'
    interpreter_eat(interpreter, TOKEN_COLON); // Consume ':'

    TryCatchFrame* frame = malloc(sizeof(TryCatchFrame));
    if (!frame) report_error("System", "Failed to allocate memory for TryCatchFrame.", try_keyword_token);
    frame->catch_clause = NULL;
    frame->finally_present = 0;
    frame->pending_exception_after_finally = create_null_value();
    frame->pending_exception_active_after_finally = 0;
    frame->prev = interpreter->try_catch_stack_top;
    interpreter->try_catch_stack_top = frame;

    // --- Pre-scan for catch/finally blocks and end ---
    // This is a simplified pre-scan. A more robust one would handle nested try-catch.
    LexerState original_lexer_state = get_lexer_state(interpreter->lexer);
    Token* original_current_token = token_deep_copy(interpreter->current_token);
    Token* end_token_for_try = NULL;
    int try_nesting = 0;

    // Pre-scan to find catch/finally clauses and the final 'end:' for this try block
    while(interpreter->current_token->type != TOKEN_EOF) {
        if (interpreter->current_token->type == TOKEN_TRY) {
            try_nesting++;
        } else if (interpreter->current_token->type == TOKEN_END) {
            if (try_nesting > 0) {
                try_nesting--; // This 'end:' closes a nested 'try'
            } else { // try_nesting == 0
                if (interpreter->current_token->col == try_col) { // Matches current 'try' block's column
                    end_token_for_try = token_deep_copy(interpreter->current_token);
                    break; // Found the 'end:' for the current 'try' block
                }
                // If 'end' at nesting 0 but not matching try_col, it's for another structure or error.
            }
        } else if (try_nesting == 0 && interpreter->current_token->type == TOKEN_CATCH && interpreter->current_token->col == try_col) {
            if (!frame->catch_clause) { // Only first catch for now
                frame->catch_clause = malloc(sizeof(CatchClauseInfo));
                if (!frame->catch_clause) report_error("System", "Failed to alloc CatchClauseInfo", interpreter->current_token);
                frame->catch_clause->variable_name_present = 0;
                frame->catch_clause->variable_name = NULL;
                frame->catch_clause->next = NULL;
                /* Token* catch_token = interpreter->current_token; // Marked as unused */
                interpreter_eat(interpreter, TOKEN_CATCH); // Eat 'catch'
                if (interpreter->current_token->type == TOKEN_AS) {
                    interpreter_eat(interpreter, TOKEN_AS);
                    if (interpreter->current_token->type != TOKEN_ID) report_error("Syntax", "Expected identifier after 'catch as'", interpreter->current_token);
                    frame->catch_clause->variable_name_present = 1;
                    frame->catch_clause->variable_name = strdup(interpreter->current_token->value);
                    interpreter_eat(interpreter, TOKEN_ID);
                }
                interpreter_eat(interpreter, TOKEN_COLON); // Eat ':' after catch [as id]
                frame->catch_clause->body_start_state = get_lexer_state_for_token_start(interpreter->lexer, interpreter->current_token->line, interpreter->current_token->col, interpreter->current_token);
                continue; // Continue scan from after catch clause header
            }
        } else if (try_nesting == 0 && interpreter->current_token->type == TOKEN_FINALLY && interpreter->current_token->col == try_col) {
            frame->finally_present = 1;
            interpreter_eat(interpreter, TOKEN_FINALLY); // Eat 'finally'
            interpreter_eat(interpreter, TOKEN_COLON);   // Eat ':'
            frame->finally_body_start_state = get_lexer_state_for_token_start(interpreter->lexer, interpreter->current_token->line, interpreter->current_token->col, interpreter->current_token);
            continue; // Continue scan
        }
        Token* temp_tok = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(temp_tok);
    }
    if (!end_token_for_try) report_error("Syntax", "Missing 'end:' for 'try' statement.", try_keyword_token);
    free_token(end_token_for_try); // We just needed to confirm its existence and position.

    // Restore lexer to start of try body
    set_lexer_state(interpreter->lexer, original_lexer_state);
    free_token(interpreter->current_token);
    interpreter->current_token = original_current_token;
    // --- End Pre-scan ---

    // 1. Execute TRY block
    int old_in_tcf_block_def = interpreter->in_try_catch_finally_block_definition;
    interpreter->in_try_catch_finally_block_definition = 1;
    // Statements in try block are indented by try_col + 4
    execute_statements_in_controlled_block(interpreter, try_col + 4, "try", TOKEN_CATCH, TOKEN_FINALLY, TOKEN_END);
    interpreter->in_try_catch_finally_block_definition = old_in_tcf_block_def;
    // Store if an exception occurred in try or was already propagating
    int exception_occurred_in_try = interpreter->exception_is_active;
    Value exception_from_try = create_null_value();
    if (exception_occurred_in_try) {
        exception_from_try = value_deep_copy(interpreter->current_exception);
    }

    // 2. Execute CATCH block (if an exception occurred and catch exists)
    if (exception_occurred_in_try && frame->catch_clause) {
        interpreter->exception_is_active = 0; // Exception is "caught" for now
        free_value_contents(interpreter->current_exception);
        interpreter->current_exception = create_null_value();

        rewind_lexer_and_token(interpreter, frame->catch_clause->body_start_state, NULL);
        enter_scope(interpreter); // Scope for catch block
        if (frame->catch_clause->variable_name_present) {
            symbol_table_set(interpreter->current_scope, frame->catch_clause->variable_name, exception_from_try);
        }
        old_in_tcf_block_def = interpreter->in_try_catch_finally_block_definition;
        interpreter->in_try_catch_finally_block_definition = 1;
        execute_statements_in_controlled_block(interpreter, try_col + 4, "catch", TOKEN_FINALLY, TOKEN_END, TOKEN_END); // Catch ends at finally or end
        interpreter->in_try_catch_finally_block_definition = old_in_tcf_block_def;

        exit_scope(interpreter); // Exit catch block scope
        // If catch block raised a new exception, interpreter->exception_is_active will be true.
        // The original exception_from_try is considered handled by this catch.
        exception_occurred_in_try = interpreter->exception_is_active; // Update based on catch block's outcome
        if (exception_occurred_in_try) { // If catch re-raised or raised new
            free_value_contents(exception_from_try); // Free the original try exception
            exception_from_try = value_deep_copy(interpreter->current_exception); // Store the new one
        } else {
            free_value_contents(exception_from_try); // Original try exception was handled
            exception_from_try = create_null_value();
        }
    } else if (exception_occurred_in_try) {
        // Exception occurred in try, but no catch clause. It remains active.
        // exception_from_try already holds it.
    }

    // 3. Execute FINALLY block (if present)
    if (frame->finally_present) {
        // Save current exception state (which might be from try or catch)
        frame->pending_exception_after_finally = value_deep_copy(exception_from_try);
        frame->pending_exception_active_after_finally = exception_occurred_in_try;

        interpreter->exception_is_active = 0; // Temporarily clear for finally execution
        free_value_contents(interpreter->current_exception);
        interpreter->current_exception = create_null_value();

        rewind_lexer_and_token(interpreter, frame->finally_body_start_state, NULL);
        old_in_tcf_block_def = interpreter->in_try_catch_finally_block_definition;
        interpreter->in_try_catch_finally_block_definition = 1;
        execute_statements_in_controlled_block(interpreter, try_col + 4, "finally", TOKEN_END, TOKEN_END, TOKEN_END); // Finally ends at 'end:'
        interpreter->in_try_catch_finally_block_definition = old_in_tcf_block_def;
        
        if (interpreter->exception_is_active) { // If finally raised its own exception
            free_value_contents(frame->pending_exception_after_finally); // Original/catch exception is superseded
            // The new exception from finally is already in interpreter->current_exception and active.
        } else { // Finally completed without new exception, restore pending one
            interpreter->current_exception = value_deep_copy(frame->pending_exception_after_finally);
            interpreter->exception_is_active = frame->pending_exception_active_after_finally;
            free_value_contents(frame->pending_exception_after_finally);
        }
    } else if (exception_occurred_in_try) { // No finally, but an exception is still active
        free_value_contents(interpreter->current_exception);
        interpreter->current_exception = value_deep_copy(exception_from_try);
        interpreter->exception_is_active = 1;
    }
    free_value_contents(exception_from_try);

    // Skip to the 'end:' token of the try-catch-finally block
    // The execute_statements_in_controlled_block should leave current_token on CATCH, FINALLY, or END
    // We need to ensure we are at the correct 'end:'
    while(interpreter->current_token->type != TOKEN_EOF &&
          !(interpreter->current_token->type == TOKEN_END && interpreter->current_token->col == try_col)) {
        Token* temp_tok = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(temp_tok);
    }

    if (interpreter->current_token->type == TOKEN_END && interpreter->current_token->col == try_col) {
        interpreter_eat(interpreter, TOKEN_END);
        interpreter_eat(interpreter, TOKEN_COLON);
    } else {
        // This should have been caught by pre-scan or means something went wrong during execution
        report_error("Syntax", "Could not find matching 'end:' for 'try' statement after execution.", try_keyword_token);
    }

    // Pop the frame
    interpreter->try_catch_stack_top = frame->prev;
    if (frame->catch_clause) {
        if (frame->catch_clause->variable_name) free(frame->catch_clause->variable_name);
        free(frame->catch_clause);
    }
    free(frame);

    // Exception, if still active, will propagate upwards.
}

static void interpret_blueprint_statement(Interpreter* interpreter) {
    Token* blueprint_keyword_token = interpreter->current_token;
    int blueprint_def_col = blueprint_keyword_token->col;
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

    interpreter_eat(interpreter, TOKEN_COLON); // Colon after name [inherits Parent]

    // --- Pre-scan for 'end:' to correctly handle function body skipping ---
    // This is crucial because 'funct' definitions inside blueprints also use 'end:'
    LexerState state_before_prescan = get_lexer_state(interpreter->lexer);
    Token* token_to_restore_after_prescan = token_deep_copy(interpreter->current_token);

    int bp_nesting_level = 0;
    Token* found_bp_end_token_info = NULL;
    Token* last_token_of_scan = NULL;
    while(interpreter->current_token->type != TOKEN_EOF) {
        DEBUG_PRINTF("Blueprint Pre-scan for '%s': Token %s ('%s') at L%d C%d, Nesting: %d, TargetCol: %d",
            new_bp->name ? new_bp->name : "<NULL_BP_NAME>",
            token_type_to_string(interpreter->current_token->type),
            interpreter->current_token->value ? interpreter->current_token->value : "",
            interpreter->current_token->line, interpreter->current_token->col,
            bp_nesting_level, blueprint_def_col);        
        if (interpreter->current_token->type == TOKEN_FUNCT || // funct inside blueprint
            interpreter->current_token->type == TOKEN_IF ||
            interpreter->current_token->type == TOKEN_LOOP ||
            interpreter->current_token->type == TOKEN_TRY) {
            bp_nesting_level++;
        } else if (interpreter->current_token->type == TOKEN_END) {
            if (bp_nesting_level == 0 && interpreter->current_token->col == blueprint_def_col) {
                found_bp_end_token_info = token_deep_copy(interpreter->current_token);
                last_token_of_scan = interpreter->current_token; // END token itself
                break;
            }
            if (bp_nesting_level > 0) bp_nesting_level--;
        }
        Token* temp_tok = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(temp_tok);
    }
    if (!found_bp_end_token_info) { // Loop terminated by EOF
        last_token_of_scan = interpreter->current_token; // EOF token
    }

    // Restore interpreter's lexer and current_token to their state *before* the pre-scan.
    set_lexer_state(interpreter->lexer, state_before_prescan);
    free_token(last_token_of_scan); // Free the token that was live at the end of the scan (END or EOF)
    interpreter->current_token = token_to_restore_after_prescan; // Assign the deep copied token back.

    if (!found_bp_end_token_info) {
        char err_msg[256];
        // Use bp_name_token->value for the error message as new_bp->name (bp_name_str) will be freed.
        snprintf(err_msg, sizeof(err_msg), "Unclosed blueprint definition for '%s'. Missing 'end:' aligned at column %d?",
                 bp_name_token->value, blueprint_def_col);

        // Cleanup blueprint resources
        free(new_bp->name); // new_bp->name is bp_name_str
        if (new_bp->class_attributes_and_methods) free_scope(new_bp->class_attributes_and_methods);
        free(new_bp);
        free_token(bp_name_token); // Free the copied name token

        // interpreter->current_token is already restored
        report_error("Syntax", err_msg, interpreter->current_token); // Report error with restored token context
    }

    int end_bp_line = found_bp_end_token_info->line;
    int end_bp_col = found_bp_end_token_info->col;
    free_token(found_bp_end_token_info);

    // Lexer and current_token are already restored to the start of the blueprint body.

    // --- End Pre-scan ---

    // Process blueprint body (class attributes and methods)
    Scope* old_scope = interpreter->current_scope;
    interpreter->current_scope = new_bp->class_attributes_and_methods; // Set scope for 'let' and 'funct' inside blueprint

    while (!(interpreter->current_token->type == TOKEN_END &&
             interpreter->current_token->line == end_bp_line &&
             interpreter->current_token->col == end_bp_col) &&
           interpreter->current_token->type != TOKEN_EOF) {

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

    interpreter_eat(interpreter, TOKEN_END); // Eat the blueprint's 'end'
    interpreter_eat(interpreter, TOKEN_COLON);

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
}

static void interpret_load_statement(Interpreter* interpreter) {
    // Token* load_keyword_token = interpreter->current_token; // Unused variable
    interpreter_eat(interpreter, TOKEN_LOAD);
    interpreter_eat(interpreter, TOKEN_COLON); // Consume ':'

    do {
        if (interpreter->current_token->type == TOKEN_ID) { // Form 1: load: module [as alias]
            // Form 1: load: module_ident_or_string [as alias]
            if (interpreter->current_token->type == TOKEN_ID || interpreter->current_token->type == TOKEN_STRING) {
                char* module_source_identifier_str = strdup(interpreter->current_token->value);
                Token* module_source_token = token_deep_copy(interpreter->current_token);
                interpreter_eat(interpreter, interpreter->current_token->type); // Eat ID or STRING

                char* alias_str = NULL; // Initialize to NULL
                bool explicit_as_keyword_used = false;

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

                char* abs_path = resolve_module_path(interpreter, module_source_identifier_str, module_source_token);
                if (abs_path) {
                    Value module_namespace_dict = load_module_from_path(interpreter, abs_path, module_source_token); // VAL_DICT

                    if (explicit_as_keyword_used) {
                        // Load module into an alias (namespace)
                        symbol_table_set(interpreter->current_scope, alias_str, module_namespace_dict);
                    } else {
                        // No 'as', so import all into current scope
                        if (module_namespace_dict.type == VAL_DICT) {
                            Dictionary* exports = module_namespace_dict.as.dict_val;
                            for (int i = 0; i < exports->num_buckets; ++i) {
                                DictEntry* entry = exports->buckets[i];
                                while (entry) {
                                    symbol_table_set(interpreter->current_scope, entry->key, entry->value);
                                    entry = entry->next;
                                }
                            }
                        } else {
                            report_error("Internal", "Module did not return a dictionary of exports.", module_source_token);
                        }
                    }
                    free_value_contents(module_namespace_dict);
                    free(abs_path);
                }
                // Cleanup
                free(module_source_identifier_str);
                free_token(module_source_token);
                if (alias_str) free(alias_str);
            }  // <-- Added missing closing brace for the TOKEN_ID branch
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

            char* abs_path = resolve_module_path(interpreter, module_name_or_path_str, module_origin_token);
            if (abs_path) {
                Value module_namespace = load_module_from_path(interpreter, abs_path, module_origin_token); // VAL_DICT
                for (int i = 0; i < item_count; ++i) {
                    // dictionary_get returns a deep copy or reports an error if not found.
                    Value item_val = dictionary_get(module_namespace.as.dict_val, item_names[i], module_origin_token);
                    symbol_table_set(interpreter->current_scope, item_aliases[i], item_val);
                    // free the copy from dictionary_get, as symbol_table_set makes its own deep copy.
                    free_value_contents(item_val);
                }
                free_value_contents(module_namespace); // Free the module namespace dict itself
                free(abs_path);
            }
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

static void interpret_run_statement(Interpreter* interpreter) {
    interpreter_eat(interpreter, TOKEN_RUN);
    interpreter_eat(interpreter, TOKEN_COLON);
    // Changed from interpret_ternary_expr to interpret_expression
    ExprResult coro_expr_res = interpret_expression(interpreter); // Should evaluate to a coroutine
    if (interpreter->exception_is_active) {
        free_value_contents(coro_expr_res.value);
        return;
    }
    Value coro_val = coro_expr_res.value; // Keep a handle to it

    if (coro_expr_res.value.type != VAL_COROUTINE) {
        if (coro_expr_res.is_freshly_created_container) free_value_contents(coro_val);
        report_error("Runtime", "'run:' expects a coroutine to execute.", interpreter->current_token);
    }
    
    Coroutine* initial_coro = coro_expr_res.value.as.coroutine_val;

    // If the coroutine came from an expression like `run: some_func_that_returns_a_coro():`
    // and `some_func_that_returns_a_coro` itself was async, it needs arguments set up.
    // This is complex. For now, assume `run:` takes a direct async function call like `run: my_async_func(arg1):`
    // In this case, `interpret_any_function_call` (called by `interpret_ternary_expr`)
    // would have created the coroutine and set up its initial scope with arguments if it's an EchoC async func.

    if (initial_coro->function_def && initial_coro->state == CORO_NEW && initial_coro->execution_scope == NULL) {
        // This implies it's an EchoC async function called directly by run:
        // Its arguments should have been parsed by interpret_any_function_call.
        // The initial scope with arguments needs to be associated here.
        // This part is tricky because interpret_ternary_expr doesn't easily pass back the argument scope.
        // For now, let's assume if it's an EchoC func, its scope is set up during creation.
        // If it's a C-created coroutine (like async_sleep), execution_scope is NULL.
    }

    if (initial_coro->state != CORO_NEW) {
        if (coro_expr_res.is_freshly_created_container) free_value_contents(coro_val);
        report_error("Runtime", "Coroutine passed to 'run:' has already been started or completed.", interpreter->current_token);
    }
    if (initial_coro->is_cancelled) {
        if (coro_expr_res.is_freshly_created_container) free_value_contents(coro_val);
        report_error("Runtime", "Cannot 'run:' a coroutine that has already been cancelled.", interpreter->current_token);
    }
    initial_coro->state = CORO_RUNNABLE;
    add_to_ready_queue(interpreter, initial_coro); // add_to_ready_queue is now in interpreter.c
    
    run_event_loop(interpreter); // This will execute queued coroutines
    // The coro_val (VAL_COROUTINE) and its underlying Coroutine* are now managed by the async system.
    // Do not free coro_val here if it was fresh, as its pointer is in the queue.
    // The `run:` statement should end with a colon.
    // `interpret_ternary_expr` would have consumed tokens up to the colon.
    if (interpreter->current_token->type == TOKEN_COLON) {
        interpreter_eat(interpreter, TOKEN_COLON);
    } else {
        report_error_unexpected_token(interpreter, "':' to end 'run:' statement");
    }
}