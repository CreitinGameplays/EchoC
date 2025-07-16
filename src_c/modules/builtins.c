// src_c/modules/builtins.c
#include "modules/builtins.h"
#include "value_utils.h" // For coroutine_decref_and_free_if_zero
#include "dictionary.h"
#include "scope.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h> // For time() in async_sleep
#include "../interpreter.h" // For add_to_ready_queue

#include "../header.h"// arrays

// show()
Value builtin_show(Interpreter* interpreter, ParsedArgument* args, int arg_count, Token* call_site_token) {
    // 1. Set default values and prepare for overrides
    const char* sep = " ";
    const char* end = "\n";
    bool flush = false;

    const char* sep_override = NULL;
    const char* end_override = NULL;

    // 2. Find keyword arguments and separate them from positional args
    Value positional_args[arg_count]; // Max possible
    int positional_arg_count = 0;

    for (int i = 0; i < arg_count; i++) {
        if (args[i].name) { // It's a keyword argument
            if (strcmp(args[i].name, "sep") == 0) {
                if (args[i].value.type != VAL_STRING) {
                    report_error("Runtime", "'sep' argument for show() must be a string.", call_site_token);
                }
                sep_override = args[i].value.as.string_val;
            } else if (strcmp(args[i].name, "end") == 0) {
                if (args[i].value.type != VAL_STRING) {
                    report_error("Runtime", "'end' argument for show() must be a string.", call_site_token);
                }
                end_override = args[i].value.as.string_val;
            } else if (strcmp(args[i].name, "flush") == 0) {
                if (args[i].value.type != VAL_BOOL) {
                    report_error("Runtime", "'flush' argument for show() must be a boolean.", call_site_token);
                }
                flush = args[i].value.as.bool_val;
            } else {
                char err_msg[100];
                snprintf(err_msg, sizeof(err_msg), "show() got an unexpected keyword argument '%s'", args[i].name);
                report_error("Runtime", err_msg, call_site_token);
            }
        } else { // It's a positional argument
            positional_args[positional_arg_count++] = args[i].value;
        }
    }

    if (sep_override) sep = sep_override;
    if (end_override) end = end_override;

    // 3. Print positional arguments
    for (int i = 0; i < positional_arg_count; i++) {
        char* str_repr = value_to_string_representation(positional_args[i], interpreter, call_site_token);
        debug_aware_printf("%s", str_repr);
        free(str_repr);
        if (i < positional_arg_count - 1) {
            debug_aware_printf("%s", sep);
        }
    }

    // 4. Print end string and handle flush
    debug_aware_printf("%s", end);
    if (flush) {
        fflush(stdout);
        #ifdef DEBUG_ECHOC
        if (echoc_debug_log_file) fflush(echoc_debug_log_file);
        #endif
    }

    return create_null_value();
}

// Placeholder for other includes if needed
// slice()
Value builtin_slice(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter; // Mark as unused if interpreter context is not needed for slice

    if (arg_count < 2 || arg_count > 3) {
        char err_msg[100];
        snprintf(err_msg, sizeof(err_msg), "slice() takes 2 or 3 arguments, but %d were given.", arg_count);
        report_error("Runtime", err_msg, call_site_token);
    }

    Value subject = args[0];
    Value start_val = args[1];
    Value end_val;
    int has_end_val = 0;

    if (arg_count == 3) {
        end_val = args[2];
        has_end_val = 1;
    }

    if (subject.type != VAL_STRING) {
        report_error("Runtime", "First argument to slice() must be a string.", call_site_token);
    }
    if (start_val.type != VAL_INT) {
        report_error("Runtime", "Second argument (start index) to slice() must be an integer.", call_site_token);
    }
    if (has_end_val && end_val.type != VAL_INT) {
        report_error("Runtime", "Third argument (end index) to slice() must be an integer.", call_site_token);
    }

    const char* original_str = subject.as.string_val;
    long original_len = (long)strlen(original_str); // Use long for consistency with indices
    long start_idx = start_val.as.integer;
    long end_idx;

    if (has_end_val) {
        end_idx = end_val.as.integer;
    } else {
        end_idx = original_len; // Slice to the end if end_idx is not provided
    }

    // Adjust negative indices
    if (start_idx < 0) {
        start_idx = original_len + start_idx;
    }
    if (end_idx < 0) {
        end_idx = original_len + end_idx;
    }

    // Clamp indices to valid range [0, original_len]
    // Start index can be up to original_len (results in empty string if start_idx == original_len)
    if (start_idx < 0) start_idx = 0;
    if (start_idx > original_len) start_idx = original_len;

    // End index can be up to original_len
    if (end_idx < 0) end_idx = 0;
    if (end_idx > original_len) end_idx = original_len;


    Value result_val;
    result_val.type = VAL_STRING;

    if (start_idx >= end_idx || start_idx >= original_len) {
        // If start is after end, or start is at/beyond string length, result is empty string
        result_val.as.string_val = strdup("");
        if (!result_val.as.string_val) {
            report_error("System", "Failed to allocate memory for empty slice result.", call_site_token);
        }
    } else {
        long slice_len = end_idx - start_idx;
        char* sliced_str = malloc(slice_len + 1);
        if (!sliced_str) {
            report_error("System", "Failed to allocate memory for slice result.", call_site_token);
        }
        strncpy(sliced_str, original_str + start_idx, slice_len);
        sliced_str[slice_len] = '\0';
        result_val.as.string_val = sliced_str;
    }

    return result_val;
}

// len()
Value builtin_len(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter; // Interpreter context not needed here
    if (arg_count != 1) {
        char err_msg[100];
        snprintf(err_msg, sizeof(err_msg), "len() takes exactly 1 argument, but %d were given.", arg_count);
        report_error("Runtime", err_msg, call_site_token);
    }
    Value subject = args[0];
    Value result;
    result.type = VAL_INT;
    switch (subject.type) {
        case VAL_STRING:
            result.as.integer = (long)strlen(subject.as.string_val);
            break;
        case VAL_ARRAY:
            result.as.integer = (long)subject.as.array_val->count;
            break;
        case VAL_TUPLE:
            result.as.integer = (long)subject.as.tuple_val->count;
            break;
        case VAL_DICT:
            result.as.integer = (long)subject.as.dict_val->count;
            break;
        default: {
            char err_msg[200];
            snprintf(err_msg, sizeof(err_msg), "len() unsupported for type (%d).", subject.type);
            report_error("Runtime", err_msg, call_site_token);
            result.as.integer = 0; // Fallback
            break;
        }
    }
    return result;
}

// append()
Value builtin_append(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter; // Mark interpreter as unused
    // Expect exactly one argument to append (besides the hidden 'self')
    if (arg_count != 2) {
        report_error("Runtime", "append() expects 1 argument.", call_site_token);
    }
    
    Value self = args[0];
    // Ensure we are operating on an array (type 4, e.g. VAL_ARRAY)
    if (self.type != VAL_ARRAY) {
        report_error("Runtime", "append() can only be used on arrays.", call_site_token);
    }
    
    Array* arr = self.as.array_val;
    // Grow array if needed
    if (arr->count >= arr->capacity) {
        arr->capacity = (arr->capacity == 0 ? 8 : arr->capacity * 2);
        Value* new_elements = realloc(arr->elements, arr->capacity * sizeof(Value));
        if (!new_elements) {
            report_error("System", "Failed to reallocate memory for array in append()", call_site_token);
        }
        arr->elements = new_elements;
    }
    // Append a deep copy of the element to insert
    arr->elements[arr->count] = value_deep_copy(args[1]);
    arr->count++;
    
    // append modifies in-place and returns null (like Python's list.append)
    return create_null_value();
}

// type()
Value builtin_type(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter; // Interpreter context not needed here
    if (arg_count != 1) {
        char err_msg[100];
        snprintf(err_msg, sizeof(err_msg), "type() takes exactly 1 argument, but %d were given.", arg_count);
        report_error("Runtime", err_msg, call_site_token);
    }
    Value subject = args[0];
    Value result_val;
    result_val.type = VAL_STRING;
    const char* type_str = NULL;

    switch (subject.type) {
        case VAL_INT:           type_str = "integer"; break;
        case VAL_FLOAT:         type_str = "float"; break;
        case VAL_STRING:        type_str = "string"; break;
        case VAL_BOOL:          type_str = "boolean"; break;
        case VAL_ARRAY:         type_str = "array"; break;
        case VAL_TUPLE:         type_str = "tuple"; break;
        case VAL_DICT:          type_str = "dictionary"; break;
        case VAL_FUNCTION:      type_str = "function"; break;
        case VAL_BLUEPRINT:     type_str = "blueprint"; break;
        case VAL_OBJECT:        type_str = "object"; break;
        case VAL_BOUND_METHOD:  type_str = "bound_method"; break;
        case VAL_COROUTINE:     type_str = "coroutine"; break;
        case VAL_GATHER_TASK:   type_str = "gather_task"; break;
        case VAL_SUPER_PROXY:   type_str = "internal_super_proxy"; break;
        case VAL_NULL:          type_str = "null"; break;
        default: {
            char err_msg[200];
            snprintf(err_msg, sizeof(err_msg), "type() called with unknown internal type (%d).", subject.type);
            report_error("Internal", err_msg, call_site_token);
            type_str = "unknown"; // Fallback, though report_error exits
            break;
        }
    }
    result_val.as.string_val = strdup(type_str);
    if (!result_val.as.string_val) {
        report_error("System", "Failed to allocate memory for type string result.", call_site_token);
    }
    return result_val;
}

