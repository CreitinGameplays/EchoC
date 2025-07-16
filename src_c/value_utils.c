// src_c/value_utils.c
#include "value_utils.h"
#include "expression_parser.h" // For interpret_statement for function body execution
#include "scope.h" // For symbol_table_get
#include "dictionary.h" // For dictionary_try_get
#include "modules/builtins.h" // For builtin_append
#include <stdio.h>  // For sprintf, snprintf
#include <string.h> // For strdup, strcpy, strcat, strncpy, strlen
#include <stdlib.h> // For malloc, free

extern void interpret_statement(Interpreter* interpreter); // From statement_parser.c

// --- DynamicString Helper ---

void ds_init(DynamicString* ds, size_t initial_capacity) {
    ds->capacity = initial_capacity > 0 ? initial_capacity : 64; // Ensure a minimum capacity
    ds->buffer = malloc(ds->capacity);
    if (!ds->buffer) {
        report_error("System", "Failed to allocate memory for dynamic string init", NULL);
    }
    ds->buffer[0] = '\0';
    ds->length = 0;
}

void ds_ensure_capacity(DynamicString* ds, size_t additional_needed) {
    if (ds->length + additional_needed + 1 > ds->capacity) { // +1 for null terminator
        size_t new_capacity = ds->capacity;
        if (new_capacity == 0) new_capacity = 64; // Initial allocation if capacity was 0
        while (ds->length + additional_needed + 1 > new_capacity) {
            new_capacity *= 2;
        }
        char* new_buffer = realloc(ds->buffer, new_capacity);
        if (!new_buffer) {
            report_error("System", "Failed to reallocate memory for dynamic string", NULL);
        }
        ds->buffer = new_buffer;
        ds->capacity = new_capacity;
    }
}

void ds_append_str(DynamicString* ds, const char* str) {
    if (!str) return;
    size_t str_len = strlen(str);
    if (str_len == 0) return;

    ds_ensure_capacity(ds, str_len);
    memcpy(ds->buffer + ds->length, str, str_len); // Use memcpy
    ds->length += str_len;
    ds->buffer[ds->length] = '\0';
}
// Returns the heap-allocated string. Caller takes ownership.
char* ds_finalize(DynamicString* ds) {
    // Optional: Shrink to fit if memory is a major concern
    // char* final_buffer = realloc(ds->buffer, ds->length + 1);
    // if (!final_buffer) { /* handle error or return original buffer */ return ds->buffer; }
    // ds->buffer = NULL; // ds no longer owns it
    // return final_buffer;
    char* result = ds->buffer;
    ds->buffer = NULL; // ds no longer owns the buffer
    ds->length = 0;
    ds->capacity = 0;
    return result;
}

// Add a check for ds_free to prevent double-free
void ds_free(DynamicString* ds) {
    if (ds && ds->buffer) {
        free(ds->buffer);
        ds->buffer = NULL;
        ds->length = 0;
        ds->capacity = 0;
    }
    // Note: ds itself is not freed as it's typically stack-allocated
}

// Helper to call op_str method on an object
static char* call_op_str_on_object(Interpreter* interpreter, Object* self_obj, Function* op_str_func, Token* error_token_context) {
    // Temporarily store and restore exception state around this utility call
    int old_exception_is_active = interpreter->exception_is_active;
    Value old_current_exception = value_deep_copy(interpreter->current_exception);
    
    LexerState state_before_op_str_call = get_lexer_state(interpreter->lexer);
    Token* token_before_op_str_call = token_deep_copy(interpreter->current_token);
    Scope* old_scope = interpreter->current_scope;
    Object* old_self_obj_ctx = interpreter->current_self_object;

    interpreter->current_scope = op_str_func->definition_scope;
    enter_scope(interpreter);
    interpreter->current_self_object = self_obj;
    // Manually insert 'self' as a direct reference, similar to execute_echoc_function
    SymbolNode* self_node_for_op_str = (SymbolNode*)malloc(sizeof(SymbolNode));
    if (!self_node_for_op_str) report_error("System", "Failed to allocate memory for 'self' symbol in op_str", error_token_context);
    self_node_for_op_str->name = strdup("self");
    if (!self_node_for_op_str->name) { free(self_node_for_op_str); report_error("System", "Failed to strdup 'self' name for op_str", error_token_context); }
    self_node_for_op_str->value.type = VAL_OBJECT;
    self_node_for_op_str->value.as.object_val = self_obj; // Direct reference, not a deep copy
    self_node_for_op_str->next = interpreter->current_scope->symbols;
    interpreter->current_scope->symbols = self_node_for_op_str;


    if (op_str_func->param_count != 1 || strcmp(op_str_func->params[0].name, "self") != 0) {

        interpreter->exception_is_active = 1;
        free_value_contents(interpreter->current_exception);
        interpreter->current_exception.type = VAL_STRING;
        interpreter->current_exception.as.string_val = strdup("op_str method must only take 'self' as a parameter.");
        exit_scope(interpreter);
        interpreter->current_scope = old_scope;
        interpreter->current_self_object = old_self_obj_ctx;
        free_value_contents(old_current_exception);
        set_lexer_state(interpreter->lexer, state_before_op_str_call);
        free_token(interpreter->current_token);
        interpreter->current_token = token_before_op_str_call;
        return strdup("<op_str error>");
    }

    // Prepare and set lexer state for op_str body execution
    LexerState effective_op_str_body_start_state = op_str_func->body_start_state;
    // Ensure the lexer state uses the function's persistent owned copy of the source text.
    effective_op_str_body_start_state.text = op_str_func->source_text_owned_copy;
    effective_op_str_body_start_state.text_length = op_str_func->source_text_length;
    // The pos, line, col, current_char from body_start_state are valid for this text.
    set_lexer_state(interpreter->lexer, effective_op_str_body_start_state);


    // Free the token that was current in the caller's context before fetching the new one
    free_token(interpreter->current_token); 
    interpreter->current_token = get_next_token(interpreter->lexer);


    interpreter->function_nesting_level++;
    interpreter->return_flag = 0;
    free_value_contents(interpreter->current_function_return_value);
    interpreter->current_function_return_value = create_null_value();

    // Loop as long as the current token is part of the function body (i.e., indented more than the function definition)
    while (interpreter->current_token->col > op_str_func->definition_col &&
           // Also stop if we hit EOF, which implies an unclosed function.
           interpreter->current_token->type != TOKEN_EOF) {
        interpret_statement(interpreter); // interpret_statement is in statement_parser.h
        
        // If op_str itself yields or contains other illegal control flow, it's an error.
        if (interpreter->break_flag || interpreter->continue_flag || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
            // FIX: Instead of calling report_error (which exits and leaks), set the exception flag
            // and return an error string. This allows the caller to clean up.
            interpreter->exception_is_active = 1;
            free_value_contents(interpreter->current_exception);
            interpreter->current_exception.type = VAL_STRING;
            interpreter->current_exception.as.string_val = strdup("op_str method cannot contain yield (await) or loop control statements (break, continue).");

            // Restore state before returning
            set_lexer_state(interpreter->lexer, state_before_op_str_call);
            free_token(interpreter->current_token);
            interpreter->current_token = token_before_op_str_call;
            exit_scope(interpreter);
            interpreter->current_scope = old_scope;
            interpreter->current_self_object = old_self_obj_ctx;
            report_error("Runtime", "op_str method cannot contain yield (await) or loop control statements (break, continue).", error_token_context);
            return strdup("<op_str error>");
        }
        if (interpreter->exception_is_active && op_str_func->body_end_token_original_line == -1) {
             // Break if exception in a function with no pre-scanned end (e.g. op_str)
        }
        if (interpreter->return_flag || interpreter->break_flag || interpreter->continue_flag || interpreter->exception_is_active) break;
    }
    interpreter->function_nesting_level--;

    // If the op_str body raised an exception, we must stop and propagate it.
    if (interpreter->exception_is_active) {
        // Restore the interpreter's state before this call.
        set_lexer_state(interpreter->lexer, state_before_op_str_call);
        free_token(interpreter->current_token);
        interpreter->current_token = token_before_op_str_call;
        exit_scope(interpreter);
        interpreter->current_scope = old_scope;
        interpreter->current_self_object = old_self_obj_ctx;
        interpreter->return_flag = 0;

        // The new exception from op_str is now the active one. Don't restore the old one.
        free_value_contents(old_current_exception);

        // Return a placeholder string. The caller will see the exception flag and free this.
        return strdup("<exception in op_str>");
    }

    Value result_val = value_deep_copy(interpreter->current_function_return_value);

    // Restore lexer state (which includes text) and token
    set_lexer_state(interpreter->lexer, state_before_op_str_call);
    free_token(interpreter->current_token); // Free last token of op_str body
    interpreter->current_token = token_before_op_str_call; // Restore original token

    exit_scope(interpreter); 
    interpreter->current_scope = old_scope; 
    interpreter->current_self_object = old_self_obj_ctx; 
    interpreter->return_flag = 0;

    // Restore original exception state unless op_str itself raised an unhandled one
    if (!interpreter->exception_is_active) { // if op_str completed without new exception
        interpreter->exception_is_active = old_exception_is_active;
        free_value_contents(interpreter->current_exception); // free the null one set by op_str call
        interpreter->current_exception = value_deep_copy(old_current_exception);
    }
    free_value_contents(old_current_exception);

    if (result_val.type != VAL_STRING) { 
        interpreter->exception_is_active = 1;
        free_value_contents(interpreter->current_exception);
        interpreter->current_exception.type = VAL_STRING;
        interpreter->current_exception.as.string_val = strdup("op_str method must return a string.");
        free_value_contents(result_val);

        return strdup("<op_str error>");
    }
    char* str_to_return = strdup(result_val.as.string_val);
    free_value_contents(result_val);
    return str_to_return;
}

#define MAX_REPR_DEPTH 8 // A reasonable depth limit

char* value_to_string_representation(Value val, Interpreter* interpreter, Token* error_token_context) {
    if (interpreter->repr_depth_count >= MAX_REPR_DEPTH) {
        if (val.type == VAL_ARRAY) return strdup("[...]");
        if (val.type == VAL_TUPLE) return strdup("(...)");
        if (val.type == VAL_DICT) return strdup("{...}");
        if (val.type == VAL_OBJECT) return strdup("<...>");
        return strdup("..."); // Generic fallback for other recursive types
    }

    interpreter->repr_depth_count++;

    char* result = NULL;
    char num_buffer[256];

    switch (val.type) {
        case VAL_INT:
            sprintf(num_buffer, "%ld", val.as.integer);
            result = strdup(num_buffer);
            break;
        case VAL_FLOAT:
            sprintf(num_buffer, "%g", val.as.floating);
            result = strdup(num_buffer);
            break;
        case VAL_STRING:
            if (val.as.string_val == NULL) { // Should not happen for a valid VAL_STRING
                DEBUG_PRINTF("Warning: value_to_string_representation encountered VAL_STRING with NULL pointer. Representing as empty string.%s","");
                result = strdup(""); 
            } else {
                result = strdup(val.as.string_val);
            }
            break;
        case VAL_BOOL:
            result = strdup(val.as.bool_val ? "true" : "false");
            break;
        case VAL_ARRAY: {
            DynamicString ds;
            ds_init(&ds, 128);
            ds_append_str(&ds, "[");
            for (int i = 0; i < val.as.array_val->count; ++i) {
                char* elem_str = value_to_string_representation(val.as.array_val->elements[i], interpreter, error_token_context); // Recursive call
                ds_append_str(&ds, elem_str);
                free(elem_str);
                if (i < val.as.array_val->count - 1) { // Only add comma if not the last element
                    ds_append_str(&ds, ", ");
                }
            }
            ds_append_str(&ds, "]");
            result = ds_finalize(&ds);
            break;
        }
        case VAL_TUPLE: {
            DynamicString ds;
            ds_init(&ds, 128);
            ds_append_str(&ds, "(");
            for (int i = 0; i < val.as.tuple_val->count; ++i) {
                char* elem_str = value_to_string_representation(val.as.tuple_val->elements[i], interpreter, error_token_context); // Recursive call
                ds_append_str(&ds, elem_str);
                free(elem_str);
                if (i < val.as.tuple_val->count - 1) { // Only add comma if not the last element
                    ds_append_str(&ds, ", ");
                }
            }
            if (val.as.tuple_val->count == 1) ds_append_str(&ds, ","); // Trailing comma for single-element tuple
            ds_append_str(&ds, ")");
            result = ds_finalize(&ds);
            break;
        }
        case VAL_DICT: {
            DynamicString ds;
            ds_init(&ds, 256);
            ds_append_str(&ds, "{");
            int first_entry = 1;
            Dictionary* dict = val.as.dict_val; // Get the dictionary pointer
            for (int i = 0; i < dict->num_buckets; ++i) {
                DictEntry* entry = dict->buckets[i];
                while (entry) {
                    if (!first_entry) {
                        ds_append_str(&ds, ", ");
                    }
                    ds_append_str(&ds, "\""); // Add quotes around the key
                    ds_append_str(&ds, entry->key); // Add the key string
                    ds_append_str(&ds, "\": "); // Add closing quote and colon-space
                    char* val_str = value_to_string_representation(entry->value, interpreter, error_token_context); // Recursive call for value
                    ds_append_str(&ds, val_str);
                    free(val_str);
                    first_entry = 0;
                    entry = entry->next;
                }
            }
            ds_append_str(&ds, "}"); // Add closing brace
            result = ds_finalize(&ds);
            break;
        }
        case VAL_FUNCTION: {
            snprintf(num_buffer, sizeof(num_buffer), "<function %s>", val.as.function_val->name); // Format function name
            result = strdup(num_buffer);
            break;
        }
        case VAL_BLUEPRINT: {
            snprintf(num_buffer, sizeof(num_buffer), "<blueprint %s>", val.as.blueprint_val->name); // Format blueprint name
            result = strdup(num_buffer);
            break;
        }
        case VAL_OBJECT: {
            Object* obj = val.as.object_val;
            Value* op_str_method_val = NULL;
            Blueprint* current_bp = obj->blueprint;
             while(current_bp) {
                op_str_method_val = symbol_table_get_local(current_bp->class_attributes_and_methods, "op_str");
                if (op_str_method_val && op_str_method_val->type == VAL_FUNCTION) break;
                op_str_method_val = NULL; // Not found or not a function here
                current_bp = current_bp->parent_blueprint; // Check parent blueprint for op_str
            }

            if (op_str_method_val) {
                result = call_op_str_on_object(interpreter, obj, op_str_method_val->as.function_val, error_token_context); // Call op_str
            } else {
                snprintf(num_buffer, sizeof(num_buffer), "<object %s instance at %p>", obj->blueprint->name, (void*)obj); // Default object representation
                result = strdup(num_buffer);
            }
            break;
        }
        case VAL_NULL:
            result = strdup("null"); // Null representation
            break;
        case VAL_COROUTINE: {
            snprintf(num_buffer, sizeof(num_buffer), "<coroutine %s at %p>",
                     val.as.coroutine_val->name ? val.as.coroutine_val->name : "unnamed",
                     (void*)val.as.coroutine_val);
            result = strdup(num_buffer);
            break;
        }
        case VAL_GATHER_TASK: { // Gather tasks are also coroutines internally
            snprintf(num_buffer, sizeof(num_buffer), "<gather_task %s at %p>",
                     val.as.coroutine_val->name ? val.as.coroutine_val->name : "unnamed_gather",
                     (void*)val.as.coroutine_val);
            result = strdup(num_buffer);
            break;
        }
        case VAL_BOUND_METHOD: {
            BoundMethod* bm = val.as.bound_method_val;
            const char* method_name = "unknown_method";
            const char* owner_type_name = "UnknownOwner";

            if (bm->self_value.type == VAL_OBJECT && bm->self_value.as.object_val && bm->self_value.as.object_val->blueprint) {
                owner_type_name = bm->self_value.as.object_val->blueprint->name;
            } else if (bm->self_value.type == VAL_ARRAY) {
                owner_type_name = "Array"; // For methods like array.append
            }
            // Add other self_value types here if they can have bound methods

            if (bm->type == FUNC_TYPE_ECHOC && bm->func_ptr.echoc_function) {
                method_name = bm->func_ptr.echoc_function->name;
            } else if (bm->type == FUNC_TYPE_C_BUILTIN) {
                // Attempt to identify common C built-ins if possible
                if (bm->self_value.type == VAL_ARRAY && bm->func_ptr.c_builtin == builtin_append) {
                     method_name = "append";
                } else {
                    method_name = "c_builtin"; // Generic name for other C built-ins
                }
            }
            snprintf(num_buffer, sizeof(num_buffer), "<bound_method %s.%s>", owner_type_name, method_name);
            result = strdup(num_buffer);
            break;
        }
        default:
            interpreter->exception_is_active = 1;
            free_value_contents(interpreter->current_exception);
            interpreter->current_exception.type = VAL_STRING;
            char err_msg[100];
            snprintf(err_msg, sizeof(err_msg), "Cannot convert unknown value type %d to string.", val.type);
            interpreter->current_exception.as.string_val = strdup(err_msg);
            result = strdup("<conversion error>");
            break;
    }

    interpreter->repr_depth_count--; // Decrement recursion depth before returning
    return result;
}

Value evaluate_interpolated_string(Interpreter* interpreter, const char* raw_string, Token* string_token_for_errors) {
    // If the string does not contain a '%', it cannot have an interpolation block.
    // In this common case, we can skip the complex dynamic string logic and just duplicate the string.
    if (strchr(raw_string, '%') == NULL) {
        Value val;
        val.type = VAL_STRING;
        val.as.string_val = strdup(raw_string);
        if (!val.as.string_val) report_error("System", "Failed to strdup non-interpolated string.", string_token_for_errors);
        return val;
    }

    DynamicString ds;
    memset(&ds, 0, sizeof(DynamicString)); // Initialize to zero
    ds_init(&ds, strlen(raw_string) + 64); // Initial guess for capacity

    DEBUG_PRINTF("INTERPOLATE_STRING: Raw: \"%s\". At line %d, col %d. Current scope: %p",
                 raw_string,
                 string_token_for_errors->line,
                 string_token_for_errors->col,
                 (void*)interpreter->current_scope);

    // Add error handling for ds_init failure
    if (!ds.buffer) {
        ds_free(&ds);
        report_error("System", "Failed to initialize dynamic string for interpolation", string_token_for_errors);
    }

    // string_token_for_errors is the original TOKEN_STRING, useful for errors like unterminated %{
    const char* p = raw_string;
    while (*p) {
        if (p[0] == '%' && p[1] == '{') { // Found an interpolation start
            p += 2; // Skip "%{"

            const char* expr_content_start = p;

            // --- START OF NEW BLOCK ---
            const char* expr_scan_ptr = p; // p is currently at the start of the expression content
            int brace_level = 1;   // Start at 1 to account for the opening '{' of %{
            int bracket_level = 0;  // For []
            int paren_level = 0;    // For ()

            while (*expr_scan_ptr != '\0') {
                char c = *expr_scan_ptr;

                // Skip over any nested strings within the expression
                if (c == '"' || c == '\'') {
                    char quote_type = c;
                    expr_scan_ptr++; // Move past opening quote
                    while (*expr_scan_ptr != '\0' && *expr_scan_ptr != quote_type) {
                        if (*expr_scan_ptr == '\\') {
                            expr_scan_ptr++; // Skip escaped character
                        }
                        if (*expr_scan_ptr == '\0') break; // Avoid incrementing past null terminator
                        expr_scan_ptr++;
                    }
                    if (*expr_scan_ptr == '\0') { // Unterminated string inside expression
                        ds_free(&ds);
                        report_error("Syntax", "Unterminated string literal within interpolated expression.", string_token_for_errors);
                    }
                    // After loop, expr_scan_ptr is on the closing quote. Advance past it.
                    expr_scan_ptr++;
                    // Continue to the next outer loop iteration to avoid the final expr_scan_ptr++
                    continue;
                } else if (c == '{') {
                    brace_level++;
                } else if (c == '}') {
                    brace_level--;
                    if (brace_level < 0) { // Mismatched '}'
                        ds_free(&ds);
                        report_error("Syntax", "Mismatched '}' in interpolated expression.", string_token_for_errors);
                    }
                    if (brace_level == 0) {
                        // Found the matching brace for our %{
                        // Now, ensure other brackets are balanced for a valid expression
                        if (bracket_level != 0 || paren_level != 0) {
                            ds_free(&ds);
                            report_error("Syntax", "Mismatched brackets/parentheses within balanced %{...} in interpolated expression.", string_token_for_errors);
                        }
                        p = expr_scan_ptr; // Set p to the position of the closing brace
                        break; // Exit the scanning loop
                    }
                } else if (c == '[') {
                    bracket_level++;
                } else if (c == ']') {
                    bracket_level--;
                    if (bracket_level < 0) { // Mismatched ']'
                        ds_free(&ds);
                        report_error("Syntax", "Mismatched ']' in interpolated expression.", string_token_for_errors);
                    }
                } else if (c == '(') {
                    paren_level++;
                } else if (c == ')') {
                    paren_level--;
                    if (paren_level < 0) { // Mismatched ')'
                        ds_free(&ds);
                        report_error("Syntax", "Mismatched ')' in interpolated expression.", string_token_for_errors);
                    }
                }
                expr_scan_ptr++;
            }
            // After the loop, if 'p' hasn't been updated to point to the closing '}',
            // it means the loop terminated due to *expr_scan_ptr == '\0' before finding the match.
            // The original 'p' (before this block) pointed to the start of the expression content.
            // If the break was hit, 'p' now points to the '}'.
            if (*p != '}' || p < expr_content_start) { // Check if p was updated and points to '}'
                ds_free(&ds);
                report_error("Syntax", "Unterminated '%{' in string interpolation (matching '}' not found).", string_token_for_errors);
            }
            
            size_t expr_len = (size_t)(p - expr_content_start);
            char* expr_str_for_parsing = malloc(expr_len + 1);
            if (!expr_str_for_parsing) {
                ds_free(&ds);
                report_error("System", "Failed to allocate memory for interpolated expression string.", string_token_for_errors);
            }
            // Ensure null termination even if strncpy doesn't fill the buffer
            if (expr_len < expr_len + 1) { // Check to prevent writing out of bounds if expr_len is SIZE_MAX
                 expr_str_for_parsing[expr_len] = '\0';
            }
            strncpy(expr_str_for_parsing, expr_content_start, expr_len);
            expr_str_for_parsing[expr_len] = '\0';
            DEBUG_PRINTF("  Interpolating expression: '%s'", expr_str_for_parsing);

            // --- START: New, Safer Sub-Expression Evaluation ---

            // 1. Save the main interpreter's current lexer and token.
            Lexer* old_lexer = interpreter->lexer;
            Token* old_main_token = interpreter->current_token;

            // 2. Create a temporary lexer instance on the stack for the expression string.
            Lexer temp_expr_lexer;
            temp_expr_lexer.text = expr_str_for_parsing;
            temp_expr_lexer.pos = 0;
            temp_expr_lexer.current_char = expr_str_for_parsing[0];
            temp_expr_lexer.line = 1; // Parse as a self-contained unit
            temp_expr_lexer.col = 1;
            temp_expr_lexer.text_length = expr_len;

            // 3. Temporarily point the main interpreter to our new lexer and get the first token.
            interpreter->lexer = &temp_expr_lexer;
            interpreter->current_token = get_next_token(interpreter->lexer); // This overwrites the pointer, which is fine since we saved it.

            // 4. Evaluate the expression using the main interpreter, which now has its full context
            //    but is reading from our temporary string.
            ExprResult sub_expr_res = interpret_expression(interpreter);
            
            // Check if the sub-expression evaluation raised an exception.
            if (interpreter->exception_is_active || (interpreter->current_executing_coroutine && interpreter->current_executing_coroutine->state == CORO_SUSPENDED_AWAIT)) {
                // An error occurred. We must stop interpolation immediately.
                DEBUG_PRINTF("INTERPOLATE_STRING: Exception or Yield detected during sub-expression evaluation. Aborting.%s", "");
                
                // The sub-interpreter run failed, but we must clean up the resources for this interpolation.
                // Clean up resources allocated by this function.
                ds_free(&ds);
                free(expr_str_for_parsing);
                if (sub_expr_res.is_freshly_created_container) {
                    free_value_contents(sub_expr_res.value);
                }

                // Restore the interpreter's original state.
                interpreter->lexer = old_lexer;
                free_token(interpreter->current_token); // Free the token from the temp lexer
                interpreter->current_token = old_main_token; // Restore the original token pointer

                // Return a dummy value. The caller (e.g., interpret_primary_expr)
                // must also check the exception_is_active flag and handle it.
                // This ensures the caller knows an error occurred and can abort correctly.
                interpreter->exception_is_active = 1;
                return create_null_value();
            }

            // 5. Clean up and restore the interpreter's state.
            free_token(interpreter->current_token); // Free the EOF token from the sub-expression.

            interpreter->lexer = old_lexer; // Restore the original lexer pointer.
            interpreter->current_token = old_main_token; // Restore the original token pointer.

            // --- END: New, Safer Sub-Expression Evaluation ---

            // If sub_expr_res evaluation caused an error, interpret_expression would call report_error and exit.
            // So, if we are here, the sub-expression was evaluated successfully (or returned a dummy on error if report_error didn't exit).

            Value resolved_value = sub_expr_res.value;
            bool resolved_value_is_owned = sub_expr_res.is_freshly_created_container;
            // Cleanup token from temp_interpreter_instance (should be EOF from sub-expression)
            // Convert the resolved value to its string representation
            char* var_str_repr = value_to_string_representation(resolved_value, interpreter, string_token_for_errors);

            // Check if value_to_string_representation (e.g., via op_str) raised an exception.
            if (interpreter->exception_is_active) {
                // An error occurred while converting the sub-expression's result to a string. Clean up.
                free(var_str_repr); // Free the placeholder string from the failed call.
                ds_free(&ds);
                free(expr_str_for_parsing);
                if (resolved_value_is_owned) free_value_contents(resolved_value);
                // The interpreter's lexer and token are already restored by the sub-expression evaluation logic.
                // We just need to return a dummy value; the caller will see the exception flag.
                return create_null_value();
            }

            ds_append_str(&ds, var_str_repr);
            free(var_str_repr);

            // Free the resolved value if it was freshly created by the sub-expression
            if (resolved_value_is_owned) {
                free_value_contents(resolved_value);
            }

            free(expr_str_for_parsing); // Free the malloc'd expression string after use.

            p++; // Skip "}"
        } else {
            ds_ensure_capacity(&ds, 1);
            ds.buffer[ds.length++] = *p;
            ds.buffer[ds.length] = '\0';
            p++;
        }
    }
	Value final_val;
	final_val.type = VAL_STRING;
	// ds_finalize transfers ownership, so no need to call ds_free after this
	final_val.as.string_val = ds_finalize(&ds);
    // This function should not modify the caller's token stream.
    // The state is restored inside the loop, but if the loop is empty (no interpolation),
    // we ensure the state is consistent here. The caller is responsible for advancing its own token.
    return final_val;
}

// Helper to free a single TryCatchFrame and its contents
static void free_try_catch_frame(TryCatchFrame* frame) {
    if (!frame) return;
    if (frame->catch_clause) {
        if (frame->catch_clause->variable_name) {
            free(frame->catch_clause->variable_name);
        }
        // In the future, if 'next' is used for multiple catch clauses, loop here.
        free(frame->catch_clause);
    }
    free_value_contents(frame->pending_exception_after_finally);
    free(frame);
}

// Increments coroutine ref_count.
void coroutine_incref(Coroutine* coro) {
    if (coro && coro->magic_number == COROUTINE_MAGIC) {
        coro->ref_count++;
        DEBUG_PRINTF("COROUTINE_INCREF: Coro %s (%p) ref_count is now %d\n", coro->name ? coro->name : "unnamed", (void*)coro, coro->ref_count);
    }
}

// In src_c/value_utils.c, replace the existing function
void coroutine_decref_and_free_if_zero(Coroutine* coro) {
    if (!coro) return;

    // First, check if the coroutine is valid. If not, it's already been freed.
    if (coro->magic_number != COROUTINE_MAGIC) {
        DEBUG_PRINTF("COROUTINE_DECREF: Attempt to decref an invalid or already freed coroutine at %p. Ignoring.\n", (void*)coro);
        return;
    }

    coro->ref_count--;
    DEBUG_PRINTF("COROUTINE_DECREF: Coro '%s' (%p) ref_count decremented to %d.\n",
                 coro->name ? coro->name : "unnamed", (void*)coro, coro->ref_count);

    if (coro->ref_count <= 0) {
        // --- START NEW WARNING LOGIC ---
        if (coro->state == CORO_NEW && coro->magic_number == COROUTINE_MAGIC) {
            // This coroutine is being destroyed without ever being run or scheduled.
            // This is the equivalent of Python's "coroutine was never awaited" warning.
            fprintf(stderr, "[EchoC RuntimeWarning] at line %d, col %d: Coroutine '%s' was created but never awaited or scheduled.\n",
                    coro->creation_line, coro->creation_col,
                    coro->name ? coro->name : "unnamed");
        }
        // --- END NEW WARNING LOGIC ---
        DEBUG_PRINTF("COROUTINE_FREE: Freeing coro '%s' (%p) as ref_count is zero.\n", coro->name ? coro->name : "unnamed", (void*)coro);

        coro->magic_number = 0; // Invalidate BEFORE any recursive calls or freeing.
        // NEW: Break child->parent links before decref'ing children to prevent use-after-free
        // in the child's handle_completed_coroutine if the parent is being freed now.
        if (coro->gather_tasks) {
            for (int i = 0; i < coro->gather_tasks->count; i++) {
                Value child_val = coro->gather_tasks->elements[i];
                if (child_val.type == VAL_COROUTINE || child_val.type == VAL_GATHER_TASK) {
                    Coroutine* child_coro = child_val.as.coroutine_val;
                    // Check magic number to avoid acting on an already-freed child
                    if (child_coro && child_coro->magic_number == COROUTINE_MAGIC && child_coro->parent_gather_coro == coro) {
                        child_coro->parent_gather_coro = NULL;
                    }
                }
            }
        }

        // Phase 1: Release references to other coroutines to break cycles and allow them to be freed.
        // This may trigger other decref calls, but they won't be able to
        // recursively free this now-invalidated coroutine.
        if (coro->awaiting_on_coro) {
            coroutine_decref_and_free_if_zero(coro->awaiting_on_coro);
        }
        if (coro->gather_tasks) {
            for (int i = 0; i < coro->gather_tasks->count; i++) {
                free_value_contents(coro->gather_tasks->elements[i]);
            }
        }
        
        // Phase 2: Free the coroutine's own contents.
        if (coro->name) free(coro->name);
        if (coro->execution_scope) free_scope(coro->execution_scope);

        if (coro->gather_tasks) { // Free the container itself
            free(coro->gather_tasks->elements);
            free(coro->gather_tasks);
        }

        if (coro->gather_results) {
            for (int i = 0; i < coro->gather_results->count; i++) {
                free_value_contents(coro->gather_results->elements[i]);
            }
            free(coro->gather_results->elements);
            free(coro->gather_results);
        }

        free_value_contents(coro->result_value);
        free_value_contents(coro->exception_value);
        free_value_contents(coro->value_from_await);
        if (coro->yielding_await_token) free_token(coro->yielding_await_token);


        // Free the try-catch stack associated with the coroutine
        TryCatchFrame* frame_iter = coro->try_catch_stack_top;
        while (frame_iter) {
            TryCatchFrame* next_frame = frame_iter->prev;
            free_try_catch_frame(frame_iter);
            frame_iter = next_frame;
        }

        CoroutineWaiterNode* waiter_node = coro->waiters_head;
        while (waiter_node) {
            CoroutineWaiterNode* next = waiter_node->next;
            free(waiter_node);
            waiter_node = next;
        }

        // Phase 3: Finally, free the coroutine struct itself
        free(coro);
    }
}