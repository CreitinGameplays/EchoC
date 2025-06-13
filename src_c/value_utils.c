// src_c/value_utils.c
#include "value_utils.h"
#include "expression_parser.h" // For interpret_statement for function body execution
#include "scope.h" // For symbol_table_get
#include "dictionary.h" // For dictionary_try_get
#include <stdio.h>  // For sprintf, snprintf
#include <string.h> // For strdup, strcpy, strcat, strncpy, strlen
#include <stdlib.h> // For malloc, free

extern void interpret_statement(Interpreter* interpreter); // From statement_parser.c

// --- DynamicString Helper ---
typedef struct {
    char* buffer;
    size_t length;
    size_t capacity;
} DynamicString;

static void ds_init(DynamicString* ds, size_t initial_capacity) {
    ds->capacity = initial_capacity > 0 ? initial_capacity : 64; // Ensure a minimum capacity
    ds->buffer = malloc(ds->capacity);
    if (!ds->buffer) {
        report_error("System", "Failed to allocate memory for dynamic string init", NULL);
    }
    ds->buffer[0] = '\0';
    ds->length = 0; // add reset
}

static void ds_ensure_capacity(DynamicString* ds, size_t additional_needed) {
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

static void ds_append_str(DynamicString* ds, const char* str) {
    if (!str) return;
    size_t str_len = strlen(str);
    if (str_len == 0) return;

    ds_ensure_capacity(ds, str_len);
    memcpy(ds->buffer + ds->length, str, str_len); // Use memcpy
    ds->length += str_len;
    ds->buffer[ds->length] = '\0';
}
// Returns the heap-allocated string. Caller takes ownership.
static char* ds_finalize(DynamicString* ds) {
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
        exit_scope(interpreter); 
        interpreter->current_scope = old_scope;
        interpreter->current_self_object = old_self_obj_ctx;
        free_value_contents(old_current_exception);
        set_lexer_state(interpreter->lexer, state_before_op_str_call); // Restore full lexer state
        free_token(token_before_op_str_call); // Free the copied token
        report_error("Runtime", "op_str method must only take 'self' as a parameter.", error_token_context);
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

    while (!(interpreter->current_token->type == TOKEN_END &&
             (op_str_func->body_end_token_original_line == -1 || // Handle case where body might be empty
              (interpreter->current_token->line == op_str_func->body_end_token_original_line &&
               interpreter->current_token->col == op_str_func->body_end_token_original_col))) &&
           interpreter->current_token->type != TOKEN_EOF) {
        interpret_statement(interpreter); // interpret_statement is in statement_parser.h
        // If op_str itself raises an exception, it should propagate out.
        // The exception_is_active flag will be set by interpret_statement if a raise occurs.
        if (interpreter->exception_is_active && op_str_func->body_end_token_original_line == -1) {
             // Break if exception in a function with no pre-scanned end (e.g. op_str)
        }
        if (interpreter->return_flag || interpreter->break_flag || interpreter->continue_flag || interpreter->exception_is_active) break;
    }
    interpreter->function_nesting_level--;
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
        free_value_contents(result_val);
        // Lexer state already restored by set_lexer_state above
        report_error("Runtime", "op_str method must return a string.", error_token_context); 
    }
    char* str_to_return = strdup(result_val.as.string_val);
    free_value_contents(result_val);
    return str_to_return;
}

char* value_to_string_representation(Value val, Interpreter* interpreter, Token* error_token_context) {
    char num_buffer[256]; // For number/bool to string conversion (fixed size is ok for these)
    switch (val.type) {
        case VAL_INT:
            sprintf(num_buffer, "%ld", val.as.integer);
            return strdup(num_buffer);
        case VAL_FLOAT:
            sprintf(num_buffer, "%g", val.as.floating);
            return strdup(num_buffer);
        case VAL_STRING:
            return strdup(val.as.string_val); // Duplicate to maintain consistency (caller frees)
        case VAL_BOOL:
            return strdup(val.as.bool_val ? "true" : "false");
        case VAL_ARRAY: {
            DynamicString ds;
            ds_init(&ds, 128);
            ds_append_str(&ds, "[");
            for (int i = 0; i < val.as.array_val->count; ++i) { // Pass interpreter
                char* elem_str = value_to_string_representation(val.as.array_val->elements[i], interpreter, error_token_context);
                ds_append_str(&ds, elem_str);
                free(elem_str);
                if (i < val.as.array_val->count - 1) {
                    ds_append_str(&ds, ", ");
                }
            }
            ds_append_str(&ds, "]");
            return ds_finalize(&ds);
        }
        case VAL_TUPLE: {
            DynamicString ds;
            ds_init(&ds, 128);
            ds_append_str(&ds, "(");
            for (int i = 0; i < val.as.tuple_val->count; ++i) { // Pass interpreter
                char* elem_str = value_to_string_representation(val.as.tuple_val->elements[i], interpreter, error_token_context);
                ds_append_str(&ds, elem_str);
                free(elem_str);
                if (i < val.as.tuple_val->count - 1) {
                    ds_append_str(&ds, ", ");
                }
            }
            if (val.as.tuple_val->count == 1) ds_append_str(&ds, ","); // Trailing comma for single-element tuple
            ds_append_str(&ds, ")");
            return ds_finalize(&ds);
        }
        case VAL_DICT: {
            DynamicString ds;
            ds_init(&ds, 256);
            ds_append_str(&ds, "{");
            int first_entry = 1;
            Dictionary* dict = val.as.dict_val;
            for (int i = 0; i < dict->num_buckets; ++i) {
                DictEntry* entry = dict->buckets[i];
                while (entry) {
                    if (!first_entry) {
                        ds_append_str(&ds, ", ");
                    }
                    ds_append_str(&ds, "\"");
                    ds_append_str(&ds, entry->key);
                    ds_append_str(&ds, "\": ");
                    char* val_str = value_to_string_representation(entry->value, interpreter, error_token_context); // Pass interpreter
                    ds_append_str(&ds, val_str);
                    free(val_str);
                    first_entry = 0;
                    entry = entry->next;
                }
            }
            ds_append_str(&ds, "}");
            return ds_finalize(&ds);
        }
        case VAL_FUNCTION: {
            // Using num_buffer as it's large enough for typical function names
            snprintf(num_buffer, sizeof(num_buffer), "<function %s>", val.as.function_val->name);
            return strdup(num_buffer);
        }
        case VAL_BLUEPRINT: {
            snprintf(num_buffer, sizeof(num_buffer), "<blueprint %s>", val.as.blueprint_val->name);
            return strdup(num_buffer);
        }
        case VAL_OBJECT: {
            Object* obj = val.as.object_val;
            // Check for op_str method
            Value* op_str_method_val = NULL;
            Blueprint* current_bp = obj->blueprint;
             while(current_bp) {
                op_str_method_val = symbol_table_get_local(current_bp->class_attributes_and_methods, "op_str");
                if (op_str_method_val && op_str_method_val->type == VAL_FUNCTION) break;
                op_str_method_val = NULL; // Not found or not a function here
                current_bp = current_bp->parent_blueprint; // Check parent
            }

            if (op_str_method_val) {
                return call_op_str_on_object(interpreter, obj, op_str_method_val->as.function_val, error_token_context);
            }
            snprintf(num_buffer, sizeof(num_buffer), "<object %s instance at %p>", obj->blueprint->name, (void*)obj);
            return strdup(num_buffer);
        }
        case VAL_NULL:
            return strdup("null");

        default:
             report_error("Internal", "Cannot convert unknown value type to string for interpolation.", error_token_context);
             return NULL; // Should not reach here
    }
}

Value evaluate_interpolated_string(Interpreter* interpreter, const char* raw_string, Token* string_token_for_errors) {
    DynamicString ds;
    ds_init(&ds, strlen(raw_string) + 64); // Initial guess for capacity

    DEBUG_PRINTF("INTERPOLATE_STRING: Raw: \"%s\". Current scope: %p", raw_string, (void*)interpreter->current_scope);
    // string_token_for_errors is the original TOKEN_STRING, useful for errors like unterminated %{

    const char* p = raw_string;
    while (*p) {
        if (p[0] == '%' && p[1] == '{') { // Found an interpolation start
            p += 2; // Skip "%{"
            const char* expr_content_start = p;

            int brace_level = 0;
            const char* expr_scan_ptr = p;
            while (*expr_scan_ptr) {
                if (*expr_scan_ptr == '"' || *expr_scan_ptr == '\'') { // Skip string literals
                    char string_quote_char = *expr_scan_ptr;
                    expr_scan_ptr++; // Move past opening quote
                    while (*expr_scan_ptr && *expr_scan_ptr != string_quote_char) {
                        if (*expr_scan_ptr == '\\' && *(expr_scan_ptr + 1)) {
                            expr_scan_ptr++; // Skip escaped char part 1
                        }
                        expr_scan_ptr++; // Skip char or second part of escaped
                    }
                    if (!*expr_scan_ptr) { // Unterminated string inside expression
                        if (ds.buffer) free(ds.buffer);
                        report_error("Syntax", "Unterminated string literal within interpolated expression.", string_token_for_errors);
                    }
                    // expr_scan_ptr now at the closing quote of the inner string
                } else if (*expr_scan_ptr == '{') {
                    // Only increment brace_level if it's not the initial '{' of a %{ block
                    // This check is likely not needed as expr_scan_ptr starts *after* initial %{
                    // but as a defensive measure if structure was %{{}}
                    // if (expr_scan_ptr != expr_content_start || *(expr_scan_ptr-1) != '%') { brace_level++; }
                    brace_level++; // Standard brace counting within expression
                } else if (*expr_scan_ptr == '}') {
                    if (brace_level == 0) {
                        p = expr_scan_ptr; // Found the correct closing brace
                        break;
                    }
                    brace_level--;
                }
                expr_scan_ptr++;
            }

            // After the loop, if 'p' hasn't been updated to point to the closing '}',
            // it means the loop terminated due to *expr_scan_ptr == '\0' before finding the match.
            // The original 'p' (before this block) pointed to the start of the expression content.
            // If the break was hit, 'p' now points to the '}'.
            if (*p != '}' || p < expr_content_start) { // Check if p was updated and points to '}'
                if (ds.buffer) free(ds.buffer);
                report_error("Syntax", "Unterminated '%{' in string interpolation (matching '}' not found).", string_token_for_errors);
            }
            
            size_t expr_len = (size_t)(p - expr_content_start);
            char* expr_str_for_parsing = malloc(expr_len + 1);
            if (!expr_str_for_parsing) {
                if (ds.buffer) free(ds.buffer);
                report_error("System", "Failed to allocate memory for interpolated expression string.", string_token_for_errors);
            }
            strncpy(expr_str_for_parsing, expr_content_start, expr_len);
            expr_str_for_parsing[expr_len] = '\0';
            DEBUG_PRINTF("  Interpolating expression: '%s'", expr_str_for_parsing);

            // Save interpreter's main lexer and current_token (which is the string token itself)
            Lexer* main_lexer_ptr_backup = interpreter->lexer;
            Token* main_token_ptr_backup = interpreter->current_token;

            // Setup temporary lexer for the expression within %{...}
            Lexer temp_expr_lexer = { expr_str_for_parsing, 0, expr_str_for_parsing[0], 1, 1, expr_len };
            interpreter->lexer = &temp_expr_lexer;
            // The current interpreter->current_token (main_token_ptr_backup) is temporarily "inactive".
            // Get the first token of the sub-expression.
            interpreter->current_token = get_next_token(interpreter->lexer); 

            ExprResult sub_expr_res = interpret_expression(interpreter); // Uses current_scope
            // interpret_expression will call report_error if the sub-expression is invalid or causes a runtime error.
            // If it returns, the expression was successfully evaluated.
            // After interpret_expression, interpreter->current_token is the last token processed from temp_expr_lexer (likely EOF).

            Value resolved_value = sub_expr_res.value;
            bool resolved_value_is_owned = sub_expr_res.is_freshly_created_container;

            // Cleanup from sub-expression parsing
            free_token(interpreter->current_token); // Free the last token from temp_expr_lexer (e.g., EOF).
            free(expr_str_for_parsing);

            // Restore interpreter's main lexer and token pointer
            interpreter->lexer = main_lexer_ptr_backup;
            interpreter->current_token = main_token_ptr_backup; // Restore pointer to the original string token.
            
            // Convert the resolved value to its string representation
            char* var_str_repr = value_to_string_representation(resolved_value, interpreter, string_token_for_errors); 
            ds_append_str(&ds, var_str_repr);
            free(var_str_repr);

            // Free the resolved value if it was freshly created by the sub-expression
            if (resolved_value_is_owned) free_value_contents(resolved_value);

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
    final_val.as.string_val = ds_finalize(&ds);
    return final_val;
}