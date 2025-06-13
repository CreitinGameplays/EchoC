// src_c/modules/builtins.c
#include "modules/builtins.h"
#include "value_utils.h"
#include "dictionary.h"
#include "scope.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h> // For time() in async_sleep

#include "../header.h"// arrays

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

// --- Asynchronous Built-ins ---

Value builtin_async_sleep_create_coro(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter; // Not directly used for creation, but good practice for builtins
    if (arg_count != 1) {
        report_error("Runtime", "async_sleep() takes exactly 1 argument (duration).", call_site_token);
    }
    if (args[0].type != VAL_INT && args[0].type != VAL_FLOAT) {
        report_error("Runtime", "async_sleep() duration must be a number.", call_site_token);
    }

    double duration_sec = (args[0].type == VAL_INT) ? (double)args[0].as.integer : args[0].as.floating;
    if (duration_sec < 0) {
        report_error("Runtime", "async_sleep() duration cannot be negative.", call_site_token);
    }

    Coroutine* sleep_coro = malloc(sizeof(Coroutine));
    if (!sleep_coro) {
        report_error("System", "Failed to allocate Coroutine for async_sleep.", call_site_token);
    }

    sleep_coro->name = strdup("async_sleep");
    sleep_coro->function_def = NULL; // No EchoC function body
    sleep_coro->execution_scope = NULL; // No local scope needed
    // resume_state is not applicable for C-created coroutines like this
    sleep_coro->state = CORO_NEW;
    sleep_coro->result_value = create_null_value();
    sleep_coro->exception_value = create_null_value();
    sleep_coro->has_exception = 0;
    sleep_coro->awaiting_on_coro = NULL;
    sleep_coro->value_from_await = create_null_value();
    sleep_coro->is_resumed_from_await = 0;
    sleep_coro->wakeup_time_sec = (double)time(NULL) + duration_sec; // Using time(NULL) for seconds precision
    sleep_coro->gather_tasks = NULL;
    sleep_coro->gather_results = NULL;
    sleep_coro->gather_pending_count = 0;
    sleep_coro->gather_first_exception_idx = -1;
    sleep_coro->parent_gather_coro = NULL;
    sleep_coro->is_cancelled = 0;
    sleep_coro->ref_count = 1;
    sleep_coro->waiters_head = NULL;

    Value coro_val;
    coro_val.type = VAL_COROUTINE;
    coro_val.as.coroutine_val = sleep_coro;
    return coro_val;
}

Value builtin_gather_create_coro(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter;
    if (arg_count != 1 || args[0].type != VAL_ARRAY) {
        report_error("Runtime", "gather() takes one argument: an array of coroutines.", call_site_token);
    }

    Array* tasks_array = args[0].as.array_val;
    for (int i = 0; i < tasks_array->count; ++i) {
        if (tasks_array->elements[i].type != VAL_COROUTINE) {
            report_error("Runtime", "All elements in the array passed to gather() must be coroutines.", call_site_token);
        }
    }

    Coroutine* gather_coro = malloc(sizeof(Coroutine));
    if (!gather_coro) {
        report_error("System", "Failed to allocate Coroutine for gather.", call_site_token);
    }

    gather_coro->name = strdup("gather_task");
    gather_coro->function_def = NULL;
    gather_coro->execution_scope = NULL;
    gather_coro->state = CORO_NEW;
    gather_coro->result_value = create_null_value();
    gather_coro->exception_value = create_null_value();
    gather_coro->has_exception = 0;
    gather_coro->awaiting_on_coro = NULL;
    gather_coro->value_from_await = create_null_value();
    gather_coro->is_resumed_from_await = 0;
    gather_coro->wakeup_time_sec = 0; // Not timer based

    // Deep copy the array of coroutine Values for gather_tasks
    gather_coro->gather_tasks = malloc(sizeof(Array));
    if (!gather_coro->gather_tasks) { free(gather_coro->name); free(gather_coro); report_error("System", "Failed to allocate gather_tasks array.", call_site_token); }
    gather_coro->gather_tasks->count = tasks_array->count;
    gather_coro->gather_tasks->capacity = tasks_array->count; // Exact capacity
    if (tasks_array->count > 0) {
        gather_coro->gather_tasks->elements = malloc(tasks_array->count * sizeof(Value));
        if (!gather_coro->gather_tasks->elements) { free(gather_coro->name); free(gather_coro->gather_tasks); free(gather_coro); report_error("System", "Failed to allocate elements for gather_tasks.", call_site_token); }
        for (int i = 0; i < tasks_array->count; ++i) {
            gather_coro->gather_tasks->elements[i] = value_deep_copy(tasks_array->elements[i]);
        }
    } else {
        gather_coro->gather_tasks->elements = NULL;
    }

    gather_coro->gather_results = malloc(sizeof(Array));
     if (!gather_coro->gather_results) { /* cleanup */ report_error("System", "Failed to allocate gather_results array.", call_site_token); }
    gather_coro->gather_results->count = 0; // Will be filled as tasks complete
    gather_coro->gather_results->capacity = tasks_array->count;
    if (tasks_array->count > 0) {
        gather_coro->gather_results->elements = calloc(tasks_array->count, sizeof(Value)); // Initialize with null-like values
        if (!gather_coro->gather_results->elements) { /* cleanup */ report_error("System", "Failed to allocate elements for gather_results.", call_site_token); }
    } else {
        gather_coro->gather_results->elements = NULL;
    }

    gather_coro->gather_pending_count = tasks_array->count;
    gather_coro->gather_first_exception_idx = -1;
    gather_coro->parent_gather_coro = NULL; // Top-level gather has no parent gather
    gather_coro->is_cancelled = 0;
    gather_coro->ref_count = 1;
    gather_coro->waiters_head = NULL;

    Value coro_val;
    coro_val.type = VAL_GATHER_TASK; // Use specific type for gather tasks
    coro_val.as.coroutine_val = gather_coro;
    return coro_val;
}

Value builtin_cancel_coro(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter;
    if (arg_count != 1 || (args[0].type != VAL_COROUTINE && args[0].type != VAL_GATHER_TASK)) {
        report_error("Runtime", "cancel() takes one argument: a coroutine object.", call_site_token);
    }

    Coroutine* coro_to_cancel = args[0].as.coroutine_val;
    if (coro_to_cancel->state != CORO_DONE) {
        coro_to_cancel->is_cancelled = 1;
        // If it's a gather task, recursively cancel its children if they are not done
        if (args[0].type == VAL_GATHER_TASK && coro_to_cancel->gather_tasks) {
            for (int i = 0; i < coro_to_cancel->gather_tasks->count; ++i) {
                builtin_cancel_coro(interpreter, &coro_to_cancel->gather_tasks->elements[i], 1, call_site_token);
            }
        }
    }
    return create_null_value();
}