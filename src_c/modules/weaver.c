// src_c/modules/weaver.c
#include "weaver.h"
#include "../interpreter.h"
#include "../value_utils.h"
#include "../dictionary.h"

// --- Forward declarations for weaver functions ---
static Value weaver_weave(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);
static Value weaver_spawn_task(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);
static Value weaver_rest(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);
static Value weaver_gather(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);
static Value weaver_cancel(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);
static Value weaver_yield_now(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// Helper to create a VAL_FUNCTION wrapper for a C function
static Value create_c_function_value(CBuiltinFunction func_ptr, const char* name, int param_count) {
    Function* c_func_wrapper = calloc(1, sizeof(Function));
    if (!c_func_wrapper) {
        report_error("System", "Failed to allocate memory for C function wrapper.", NULL);
    }
    c_func_wrapper->name = strdup(name);
    c_func_wrapper->param_count = param_count; // Use -1 for varargs, or a specific number for arity checks
    c_func_wrapper->is_async = false; // The wrapper itself is not async
    c_func_wrapper->c_impl = func_ptr; // This is the new field pointing to the C implementation
    
    Value val;
    val.type = VAL_FUNCTION;
    val.as.function_val = c_func_wrapper;
    return val;
}

// --- Implementations ---

// weaver.weave(main_coroutine)
static Value weaver_weave(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    if (arg_count != 1) {
        report_error("Runtime", "weaver.weave() expects 1 argument (a coroutine).", call_site_token);
    }
    Value coro_val = args[0];
    if (coro_val.type != VAL_COROUTINE && coro_val.type != VAL_GATHER_TASK) {
        report_error("Runtime", "weaver.weave() expects a coroutine to execute.", call_site_token);
    }
    
    Coroutine* initial_coro = coro_val.as.coroutine_val;
    // --- START FIX: Manage coroutine reference count ---
    // Increment the ref count to signal that weaver.weave now holds a reference.
    // This prevents the coroutine from being freed prematurely by the caller.
    coroutine_incref(initial_coro);

    if (initial_coro->state != CORO_NEW) {
        // If we exit early, we must release the reference we just took.
        coroutine_decref_and_free_if_zero(initial_coro);
        report_error("Runtime", "Coroutine passed to weaver.weave() has already been started or completed.", call_site_token);
    }
    if (initial_coro->is_cancelled) {
        // If we exit early, we must release the reference we just took.
        coroutine_decref_and_free_if_zero(initial_coro);
        report_error("Runtime", "Cannot weave a coroutine that has already been cancelled.", call_site_token);
    }

    initial_coro->state = CORO_RUNNABLE;
    add_to_ready_queue(interpreter, initial_coro);
    
    run_event_loop(interpreter);

    Value result = create_null_value();

    // After the event loop, check for unhandled exceptions from the root task.
    if (initial_coro->state == CORO_DONE) {
        if (initial_coro->has_exception) {
            // Propagate the coroutine's exception to the main interpreter state
            // so that the final error message in main() is correct.
            free_value_contents(interpreter->current_exception); // Free any old exception
            interpreter->current_exception = value_deep_copy(initial_coro->exception_value);

            interpreter->unhandled_error_occured = 1;
            char* err_str_repr = value_to_string_representation(initial_coro->exception_value, interpreter, NULL);
            fprintf(stderr, "\n[EchoC Runtime Error] Unhandled exception in async workflow '%s': %s\n",
                     initial_coro->name ? initial_coro->name : "unnamed_root_coro",
                     err_str_repr);
            free(err_str_repr);
            // result is already a null value, so we just proceed to cleanup.
        }
        // If successful, return the result from the root coroutine
        else {
            result = value_deep_copy(initial_coro->result_value);
        }

    }
    // Release the reference held by weaver.weave.
    coroutine_decref_and_free_if_zero(initial_coro);
    return result;
    // --- END FIX ---
}

// weaver.spawn_task(coroutine)
static Value weaver_spawn_task(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    if (arg_count != 1 || (args[0].type != VAL_COROUTINE && args[0].type != VAL_GATHER_TASK)) {
        report_error("Runtime", "weaver.spawn_task() expects 1 argument (a coroutine or gather_task).", call_site_token);
    }
    
    Coroutine* coro_to_spawn = args[0].as.coroutine_val;
    
    if (coro_to_spawn->state != CORO_NEW) {
        report_error("Runtime", "Coroutine passed to weaver.spawn_task() has already been started.", call_site_token);
    }

    if (args[0].type == VAL_GATHER_TASK) {
        coro_to_spawn->state = CORO_GATHER_WAIT;
    } else {
        coro_to_spawn->state = CORO_RUNNABLE;
    }
    
    add_to_ready_queue(interpreter, coro_to_spawn);
    
    // Return the coroutine object itself as the task handle
    return value_deep_copy(args[0]);
}

// weaver.rest(duration)
static Value weaver_rest(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter; // Mark as unused
    if (arg_count != 1) {
        report_error("Runtime", "weaver.rest() takes exactly 1 argument (duration).", call_site_token);
    }
    if (args[0].type != VAL_INT && args[0].type != VAL_FLOAT) {
        report_error("Runtime", "weaver.rest() duration must be a number.", call_site_token);
    }

    double duration_ms = (args[0].type == VAL_INT) ? (double)args[0].as.integer : args[0].as.floating;
    if (duration_ms < 0) {
        report_error("Runtime", "weaver.rest() duration cannot be negative.", call_site_token);
    }

    double duration_sec = duration_ms / 1000.0; // Convert from milliseconds to seconds

    Coroutine* sleep_coro = calloc(1, sizeof(Coroutine));
    if (!sleep_coro) {
        report_error("System", "Failed to allocate Coroutine for weaver.rest().", call_site_token);
    }

    sleep_coro->magic_number = COROUTINE_MAGIC;
    sleep_coro->creation_line = call_site_token->line;
    sleep_coro->creation_col = call_site_token->col;
    sleep_coro->ref_count = 1;
    sleep_coro->name = strdup("weaver.rest");
    sleep_coro->state = CORO_NEW;
    sleep_coro->result_value = create_null_value();
    sleep_coro->exception_value = create_null_value();
    sleep_coro->value_from_await = create_null_value();
    sleep_coro->wakeup_time_sec = get_monotonic_time_sec() + duration_sec;
    sleep_coro->gather_first_exception_idx = -1;

    Value coro_val;
    coro_val.type = VAL_COROUTINE;
    coro_val.as.coroutine_val = sleep_coro;
    return coro_val;
}

// weaver.gather(array_of_coroutines)
static Value weaver_gather(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    // This logic is moved from the old `builtin_gather_create_coro`
    (void)interpreter;
    if (arg_count != 1 || args[0].type != VAL_ARRAY) {
        report_error("Runtime", "weaver.gather() takes one argument: an array of coroutines.", call_site_token);
    }

    Array* tasks_array = args[0].as.array_val;
    for (int i = 0; i < tasks_array->count; ++i) {
        if (tasks_array->elements[i].type != VAL_COROUTINE && tasks_array->elements[i].type != VAL_GATHER_TASK) {
            report_error("Runtime", "All elements in the array passed to weaver.gather() must be coroutines.", call_site_token);
        }
    }

    Coroutine* gather_coro = calloc(1, sizeof(Coroutine));
    if (!gather_coro) report_error("System", "Failed to allocate Coroutine for gather.", call_site_token);
    
    gather_coro->magic_number = COROUTINE_MAGIC;
    gather_coro->creation_line = call_site_token->line;
    gather_coro->creation_col = call_site_token->col;
    gather_coro->ref_count = 1;
    gather_coro->name = strdup("weaver.gather");
    gather_coro->state = CORO_NEW;
    gather_coro->result_value = create_null_value();
    gather_coro->exception_value = create_null_value();
    gather_coro->value_from_await = create_null_value();
    gather_coro->gather_tasks = value_deep_copy(args[0]).as.array_val;
    gather_coro->gather_results = malloc(sizeof(Array));
    gather_coro->gather_results->count = tasks_array->count;
    gather_coro->gather_results->capacity = tasks_array->count;
    gather_coro->gather_results->elements = tasks_array->count > 0 ? calloc(tasks_array->count, sizeof(Value)) : NULL;
    gather_coro->gather_pending_count = tasks_array->count;
    gather_coro->gather_first_exception_idx = -1;

    if (tasks_array->count == 0) {
        gather_coro->state = CORO_DONE;
        free_value_contents(gather_coro->result_value);
        gather_coro->result_value.type = VAL_ARRAY;
        gather_coro->result_value.as.array_val = gather_coro->gather_results;
        gather_coro->gather_results = NULL;
        if (gather_coro->gather_tasks) { free(gather_coro->gather_tasks->elements); free(gather_coro->gather_tasks); gather_coro->gather_tasks = NULL; }
    } // The 'else' case is now handled when the gather task is first awaited.

    Value coro_val;
    coro_val.type = VAL_GATHER_TASK;
    coro_val.as.coroutine_val = gather_coro;
    return coro_val;
}

// weaver.cancel(task)
static Value weaver_cancel(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)interpreter;
    if (arg_count != 1 || (args[0].type != VAL_COROUTINE && args[0].type != VAL_GATHER_TASK)) {
        report_error("Runtime", "weaver.cancel() takes one argument: a coroutine object.", call_site_token);
    }

    Coroutine* coro_to_cancel = args[0].as.coroutine_val;
    if (coro_to_cancel->state != CORO_DONE) {
        coro_to_cancel->is_cancelled = 1;
        if (args[0].type == VAL_GATHER_TASK && coro_to_cancel->gather_tasks) {
            for (int i = 0; i < coro_to_cancel->gather_tasks->count; ++i) {
                weaver_cancel(interpreter, &coro_to_cancel->gather_tasks->elements[i], 1, call_site_token);
            }
        }
    }
    return create_null_value();
}

// weaver.yield_now()
static Value weaver_yield_now(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token) {
    (void)args; // Mark as unused
    if (arg_count != 0) {
        report_error("Runtime", "weaver.yield_now() takes 0 arguments.", call_site_token);
    }
    Value zero_duration;
    zero_duration.type = VAL_FLOAT;
    zero_duration.as.floating = 0.0;
    return weaver_rest(interpreter, &zero_duration, 1, call_site_token);
}

// --- Module Creation ---

Value create_weaver_module(Interpreter* interpreter) {
    (void)interpreter;
    Dictionary* weaver_module = dictionary_create(16, NULL);

    // Helper macro to create, set, and free the temporary C function value
    #define ADD_WEAVER_FUNC(name, c_func, arity) do { \
        Value temp_val = create_c_function_value(c_func, name, arity); \
        dictionary_set(weaver_module, name, temp_val, NULL); \
        free_value_contents(temp_val); \
    } while (0)

    ADD_WEAVER_FUNC("weave", weaver_weave, 1);
    ADD_WEAVER_FUNC("spawn_task", weaver_spawn_task, 1);
    ADD_WEAVER_FUNC("rest", weaver_rest, 1);
    ADD_WEAVER_FUNC("gather", weaver_gather, 1);
    ADD_WEAVER_FUNC("cancel", weaver_cancel, 1);
    ADD_WEAVER_FUNC("yield_now", weaver_yield_now, 0);

    // Undefine the macro to keep it local to this function
    #undef ADD_WEAVER_FUNC

    Value module_val;
    module_val.type = VAL_DICT;
    module_val.as.dict_val = weaver_module;
    return module_val;
}
 