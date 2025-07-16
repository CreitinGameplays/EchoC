#include "header.h"
#include "interpreter.h"      // Include the new header for its own declarations
#include "modules/builtins.h" // Include the builtins header
#include <time.h>             // For time() in event loop
#include "parser_utils.h"      // For token_type_to_string
#include "expression_parser.h" // For actual expression parsing functions
#include "scope.h"             // For VarScopeInfo, symbol_table_set, etc.
#include "value_utils.h"       // For value_to_string_representation
#include "statement_parser.h"  // For actual statement parsing functions

// Forward declarations for dictionary functions to avoid implicit declaration warnings/conflicts
Dictionary* dictionary_create(int initial_buckets, Token* error_token);
void dictionary_set(Dictionary* dict, const char* key_str, Value value, Token* error_token);
Value dictionary_get(Dictionary* dict, const char* key, Token* error_token);
// Forward declaration for symbol table lookup
Value* symbol_table_get(Scope* scope, const char* var_name);

// Forward declaration for the new helper function
static void handle_completed_coroutine(Interpreter* interpreter, Coroutine* done_coro);

// NOTE: The expression parsing functions (interpret_ternary_expr, interpret_primary_expr, etc.)
// and statement parsing functions (interpret_statement, interpret_block_statement, etc.)
// are now expected to come from expression_parser.c and statement_parser.c respectively,
// included via their headers. Stubs/partial implementations previously in this file should be removed.

// Implementation for get_monotonic_time_sec
double get_monotonic_time_sec(void) {
#ifdef _WIN32
    // Windows-specific implementation
    LARGE_INTEGER freq;
    LARGE_INTEGER count;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&count)) {
        // Fallback or error handling if QPC is not available
        return (double)time(NULL); // Low-resolution fallback
    }
    return (double)count.QuadPart / (double)freq.QuadPart;
#else
    // POSIX-specific implementation (Linux, macOS, etc.)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        // Handle error, e.g., by falling back or reporting
        perror("clock_gettime(CLOCK_MONOTONIC) failed");
        return (double)time(NULL); // Low-resolution fallback
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

// Expression and statement parsing functions are now in their respective files.

// Main interpret function: process statements until EOF.
void interpret(Interpreter* interpreter) {
    while (interpreter->current_token->type != TOKEN_EOF) {
        interpret_statement(interpreter);
        if (interpreter->exception_is_active) {
            interpreter->unhandled_error_occured = 1;
            break; // Exit loop on unhandled exception
        }
    }
}

// --- Async Event Loop and Coroutine Management ---

void add_to_ready_queue(Interpreter* interpreter, Coroutine* coro) {
    // If the coro is already in the queue, do nothing.
    if (coro->is_in_ready_queue) {
        DEBUG_PRINTF("ADD_TO_READY_QUEUE: Coro %s (%p) is already in the ready queue. Skipping.", coro->name ? coro->name : "unnamed", (void*)coro);
        return;
    }

    CoroutineQueueNode* new_node = malloc(sizeof(CoroutineQueueNode));
    if (!new_node) report_error("System", "Failed to allocate CoroutineQueueNode.", NULL);
    new_node->coro = coro;
    new_node->next = NULL;

    // Increment ref_count as the queue now holds a reference
    coro->ref_count++;
    // Set the flag to indicate it's now in the queue.
    coro->is_in_ready_queue = 1;

    DEBUG_PRINTF("ADD_TO_READY_QUEUE: Coro %s (%p) ref_count incremented to %d.", coro->name ? coro->name : "unnamed", (void*)coro, coro->ref_count);

    if (interpreter->async_ready_queue_tail) {
        interpreter->async_ready_queue_tail->next = new_node;
        interpreter->async_ready_queue_tail = new_node;
    } else {
        interpreter->async_ready_queue_head = new_node;
        interpreter->async_ready_queue_tail = new_node;
    }
    DEBUG_PRINTF("Added coro %s (%p) to ready queue. State: %d", coro->name ? coro->name : "unnamed", (void*)coro, coro->state);
}

Coroutine* get_from_ready_queue(Interpreter* interpreter) {
    while (interpreter->async_ready_queue_head) {
        CoroutineQueueNode* head_node = interpreter->async_ready_queue_head;
        Coroutine* coro = head_node->coro;

        interpreter->async_ready_queue_head = head_node->next;
        if (!interpreter->async_ready_queue_head) {
            interpreter->async_ready_queue_tail = NULL;
        }
        DEBUG_PRINTF("GET_FROM_READY_QUEUE: Popping node %p for coro %s (%p). New ReadyQ_Head: %p",
                     (void*)head_node, coro->name ? coro->name : "unnamed", (void*)coro, (void*)interpreter->async_ready_queue_head);
        free(head_node); // Free the node regardless

        if (coro && coro->magic_number == COROUTINE_MAGIC) {
            DEBUG_PRINTF("Got coro %s (%p) from ready queue. State: %d", coro->name ? coro->name : "unnamed", (void*)coro, coro->state);
            // This coro is no longer in the queue, so reset the flag.
            coro->is_in_ready_queue = 0;
            return coro;
        } else {
            // Coro was null or magic number mismatch (likely freed while in queue)
            DEBUG_PRINTF("GET_FROM_READY_QUEUE: Skipped stale/freed coroutine node (coro: %p, magic: %08X).",
                         (void*)coro, coro ? coro->magic_number : 0);
            // Loop again to get next node
        }
    }
    return NULL; // Queue is empty or only contained stale nodes
}

// Helper to add to sleep queue (sorted by wakeup_time_sec)
void add_to_sleep_queue(Interpreter* interpreter, Coroutine* coro) {
    CoroutineQueueNode* new_node = malloc(sizeof(CoroutineQueueNode));
    if (!new_node) {
        // Attempt to provide context if current_token is available from the interpreter
        Token* error_token = interpreter->current_token ? interpreter->current_token : NULL;
        report_error("System", "Failed to allocate CoroutineQueueNode for sleep queue.", error_token);
        return; // Should not be reached if report_error exits
    }
    new_node->coro = coro;
    // Increment ref_count as the sleep queue now holds a reference
    coro->ref_count++;
    DEBUG_PRINTF("ADD_TO_SLEEP_QUEUE: Coro %s (%p) ref_count incremented to %d.", coro->name ? coro->name : "unnamed", (void*)coro, coro->ref_count);

    new_node->next = NULL;

    if (!interpreter->async_sleep_queue_head || coro->wakeup_time_sec < interpreter->async_sleep_queue_head->coro->wakeup_time_sec) {
        new_node->next = interpreter->async_sleep_queue_head;
        interpreter->async_sleep_queue_head = new_node;
        if (!interpreter->async_sleep_queue_tail) { // If queue was empty
            interpreter->async_sleep_queue_tail = new_node;
        }
    } else {
        CoroutineQueueNode* current = interpreter->async_sleep_queue_head;
        while (current->next && current->next->coro->wakeup_time_sec <= coro->wakeup_time_sec) {
            current = current->next;
        }
        new_node->next = current->next;
        current->next = new_node;
        if (!new_node->next) { // Inserted at the end
            interpreter->async_sleep_queue_tail = new_node;
        }
    }
    DEBUG_PRINTF("Added coro %s (%p) to sleep queue. Wakeup: %.2f", coro->name ? coro->name : "unnamed", (void*)coro, coro->wakeup_time_sec);
}

// Helper to check sleep queue and move ready coroutines to ready queue
static void check_and_move_sleepers_to_ready_queue(Interpreter* interpreter) {
#ifdef DEBUG_ECHOC
    // IMPORTANT: current_time should ideally be fetched once at the start of this function
    double current_time = get_monotonic_time_sec();
#else
    double current_time = get_monotonic_time_sec();
#endif
    while (interpreter->async_sleep_queue_head && interpreter->async_sleep_queue_head->coro->wakeup_time_sec <= current_time) { // Peek before pop
        CoroutineQueueNode* head_node = interpreter->async_sleep_queue_head;
        Coroutine* sleeper_coro = head_node->coro;
#ifdef DEBUG_ECHOC
        fprintf(stderr, "SLEEP_DEBUG: Waking up sleeper %s (%p). Wakeup: %.2f, Current: %.2f. SleepQ_Head before pop: %p\n",
                 sleeper_coro->name ? sleeper_coro->name : "unnamed", (void*)sleeper_coro,
                 sleeper_coro->wakeup_time_sec, current_time, (void*)interpreter->async_sleep_queue_head);
        fflush(stderr);

#endif
        interpreter->async_sleep_queue_head = head_node->next;
        if (!interpreter->async_sleep_queue_head) {
            interpreter->async_sleep_queue_tail = NULL;
        }
        free(head_node);

        // Save name for logging *before* any potential free by waiters
        char* sleeper_name_for_log = NULL;
        if (sleeper_coro) {
            if (sleeper_coro->name) {
                sleeper_name_for_log = strdup(sleeper_coro->name);
                if (!sleeper_name_for_log) {
                    report_error("System", "Failed to strdup sleeper_coro name for log in check_and_move_sleepers.", NULL);
                }
            } else {
                sleeper_name_for_log = strdup("unnamed_sleeper_in_queue");
                if (!sleeper_name_for_log) {
                    report_error("System", "Failed to strdup fallback sleeper_coro name for log in check_and_move_sleepers.", NULL);
                }
            }
        } else { // Should not happen if queue logic is correct
        sleeper_name_for_log = strdup("unnamed_sleeper_coro_null_ptr"); // Fallback if sleeper_coro itself was NULL (highly unlikely)
            if (!sleeper_name_for_log) {
                report_error("System", "Failed to strdup critical fallback sleeper_coro name for log.", NULL);
            }
    }
        // Check magic number for sleeper_coro *after* removing from queue but *before* processing
        if (sleeper_coro && sleeper_coro->magic_number != COROUTINE_MAGIC) {
            DEBUG_PRINTF("CHECK_SLEEPERS: Sleeper coro %s (%p) has invalid magic number %08X. Likely freed. Skipping.", sleeper_name_for_log, (void*)sleeper_coro, sleeper_coro->magic_number);
            if (sleeper_name_for_log) free(sleeper_name_for_log);
            coroutine_decref_and_free_if_zero(sleeper_coro); // Still need to decref for the queue's reference
            continue; // Skip to next sleeper
        }

        if (sleeper_coro->is_cancelled) {
            DEBUG_PRINTF("Sleeper coro %s (%p) was cancelled. Marking done with exception.", sleeper_coro->name ? sleeper_coro->name : "unnamed", (void*)sleeper_coro);
            sleeper_coro->state = CORO_DONE;
            sleeper_coro->has_exception = 1; // Mark that it completed with an exception
        if (sleeper_coro->exception_value.type != VAL_NULL) free_value_contents(sleeper_coro->exception_value);
            sleeper_coro->exception_value.type = VAL_STRING;
            sleeper_coro->exception_value.as.string_val = strdup(CANCELLED_ERROR_MSG);
            if (!sleeper_coro->exception_value.as.string_val) {
                if (sleeper_name_for_log) free(sleeper_name_for_log);
                // report_error exits, so sleeper_name_for_log might not be freed if it was non-NULL.
                report_error("System", "Failed to strdup CANCELLED_ERROR_MSG for sleeper.", NULL);
            }
            // Wake its waiters with the cancellation error
            CoroutineWaiterNode* waiter_node = sleeper_coro->waiters_head;
            CoroutineWaiterNode* next_waiter_node = NULL;
            // Detach waiters list before processing to avoid issues if a waiter re-registers or modifies list
            sleeper_coro->waiters_head = NULL; // Clear the list head now that we are processing it
            while (waiter_node) {
                next_waiter_node = waiter_node->next;
                Coroutine* waiter = waiter_node->waiter_coro;
                if (waiter->state == CORO_SUSPENDED_AWAIT && waiter->awaiting_on_coro == sleeper_coro) {
                    if (waiter->value_from_await.type != VAL_NULL) free_value_contents(waiter->value_from_await);
                    waiter->value_from_await = value_deep_copy(sleeper_coro->exception_value);
                    waiter->awaiting_on_coro = NULL; // The waiter no longer awaits this coro.
                    waiter->state = CORO_RESUMING;
                    coroutine_decref_and_free_if_zero(sleeper_coro); // The waiter releases its reference
                    
                    add_to_ready_queue(interpreter, waiter); // This increments waiter's ref_count
                }
                free(waiter_node); // Free the waiter node from the list
                waiter_node = next_waiter_node;
            } // End while (waiter_node)
        } else { // Not cancelled
            if (sleeper_name_for_log && strcmp(sleeper_name_for_log, "weaver.rest") == 0) {
                sleeper_coro->state = CORO_DONE;
                // result_value is already VAL_NULL.
                // Wake its waiters.
                CoroutineWaiterNode* waiter_node = sleeper_coro->waiters_head;
                // Detach waiters list
                CoroutineWaiterNode* next_waiter_node = NULL;
                sleeper_coro->waiters_head = NULL; // Clear the list head now that we are processing it
                while (waiter_node) {
                    next_waiter_node = waiter_node->next;
                    Coroutine* waiter = waiter_node->waiter_coro;
                    if (waiter->state == CORO_SUSPENDED_AWAIT && waiter->awaiting_on_coro == sleeper_coro) {
                        if (waiter->value_from_await.type != VAL_NULL) free_value_contents(waiter->value_from_await); // Clear old
                        
                        waiter->resumed_with_exception = sleeper_coro->has_exception; // Should be false for normal async_sleep

                        waiter->value_from_await = value_deep_copy(sleeper_coro->result_value); // VAL_NULL
                        
                        waiter->awaiting_on_coro = NULL; // The waiter no longer awaits this coro.
                        waiter->state = CORO_RESUMING;
                        coroutine_decref_and_free_if_zero(sleeper_coro); // The waiter releases its reference
                        
                        add_to_ready_queue(interpreter, waiter); // This increments waiter's ref_count
                    }
                    free(waiter_node); // Free the waiter node after processing
                    waiter_node = next_waiter_node;
                }
#ifdef DEBUG_ECHOC
                fprintf(stderr, "SLEEP_DEBUG: Timer coro %s (%p) DONE. Waking waiters. Destroyed self. SleepQ_Head after pop: %p\n",
                         sleeper_name_for_log ? sleeper_name_for_log : "unnamed_async_sleep",
                         (void*)sleeper_coro, (void*)interpreter->async_sleep_queue_head);
                fflush(stderr);
#endif
            } else { 
                // This path is for non-cancelled, non-async_sleep coroutines that were on the sleep queue.
                // sleeper_coro should still be valid here.
                DEBUG_PRINTF("Sleeper coro %s (%p) woke up. Adding to ready queue.", 
                             sleeper_coro->name ? sleeper_coro->name : "unnamed", (void*)sleeper_coro);
                sleeper_coro->state = CORO_RUNNABLE;
                add_to_ready_queue(interpreter, sleeper_coro);
#ifdef DEBUG_ECHOC
                fprintf(stderr, "SLEEP_DEBUG: Non-async_sleep sleeper %s (%p) woke. Added to readyQ. SleepQ_Head after pop: %p\n",
                         sleeper_name_for_log ? sleeper_name_for_log : "unnamed", 
                         (void*)sleeper_coro, 
                         (void*)interpreter->async_sleep_queue_head);
                fflush(stderr);
#endif
            }
        }
        // Decrement ref_count for the reference previously held by the sleep queue.
        // This is done *after* all processing of sleeper_coro for this iteration.
        coroutine_decref_and_free_if_zero(sleeper_coro);

        if (sleeper_name_for_log) {
            free(sleeper_name_for_log);
            sleeper_name_for_log = NULL;
        }
    }
    // Ensure current_time is only fetched once if DEBUG_ECHOC is not defined
    (void)current_time; // Suppress unused warning if DEBUG_ECHOC is not defined
}

void run_event_loop(Interpreter* interpreter) {
    // Main event loop to process runnable coroutines and manage suspended ones.
#ifdef DEBUG_ECHOC
    fprintf(stderr, "EVENT_LOOP_DEBUG: Entered run_event_loop. ReadyQ_Head: %p, SleepQ_Head: %p\n",
                 (void*)interpreter->async_ready_queue_head,
                 (void*)interpreter->async_sleep_queue_head);
    fflush(stderr);
#endif
    interpreter->async_event_loop_active = 1; // Indicate event loop is running

    while (interpreter->async_ready_queue_head || interpreter->async_sleep_queue_head) {
// --- Start of New Event Loop Implementation ---

#ifdef DEBUG_ECHOC
        fprintf(stderr, "EVENT_LOOP_DEBUG: Top of loop. ReadyQ: %p, SleepQ: %p\n",
                     (void*)interpreter->async_ready_queue_head,
                     (void*)interpreter->async_sleep_queue_head);
        fflush(stderr);
#endif
    // First, move any ready sleepers to the ready queue
    check_and_move_sleepers_to_ready_queue(interpreter);
#ifdef DEBUG_ECHOC
    fprintf(stderr, "EVENT_LOOP_DEBUG: After check_sleepers. ReadyQ: %p, SleepQ: %p\n",
                     (void*)interpreter->async_ready_queue_head,
                     (void*)interpreter->async_sleep_queue_head);
    fflush(stderr);
#endif
    if (!interpreter->async_ready_queue_head) {
        if (interpreter->async_sleep_queue_head) {
            // Ready queue is empty, but there are sleeping tasks.
            // Calculate how long to sleep until the next task wakes up.
            double now = get_monotonic_time_sec();
            double next_wakeup_time = interpreter->async_sleep_queue_head->coro->wakeup_time_sec;
            double sleep_duration_sec = next_wakeup_time - now;

            if (sleep_duration_sec > 0) {
                // Convert to microseconds for usleep or milliseconds for Sleep
                #ifdef _WIN32
                DWORD sleep_ms = (DWORD)(sleep_duration_sec * 1000);
                if (sleep_ms > 0) {
                    DEBUG_PRINTF("EVENT_LOOP: ReadyQ empty, sleeping for %lu ms until next task.", sleep_ms);
                    Sleep(sleep_ms);
                }
                #else
                useconds_t sleep_us = (useconds_t)(sleep_duration_sec * 1000000);
                if (sleep_us > 0) {
                    DEBUG_PRINTF("EVENT_LOOP: ReadyQ empty, sleeping for %u us until next task.", sleep_us);
                    usleep(sleep_us);
                }
                #endif
            }
            continue; 
        } else { // Both queues are empty, the event loop is done.
            break;
        }
    }

    Coroutine* current_coro = get_from_ready_queue(interpreter);
    if (!current_coro) {
         continue;
    } // This should not happen if the check above passed, but it's a safe guard.

    // This is the main execution block for the dequeued coroutine.
    // We use an if-else structure to handle different states.
    if (current_coro->state == CORO_RUNNABLE || current_coro->state == CORO_RESUMING) {
        // Coroutine is not yet done, so we execute it.
        if (current_coro->is_cancelled) {
            // Finalize it as cancelled.
            DEBUG_PRINTF("Event Loop: Coro %s (%p) from ready queue is cancelled. Finalizing.", current_coro->name ? current_coro->name : "unnamed", (void*)current_coro);
            current_coro->state = CORO_DONE;
            current_coro->has_exception = 1;
            if (current_coro->exception_value.type != VAL_NULL) free_value_contents(current_coro->exception_value);
            current_coro->exception_value.type = VAL_STRING;
            current_coro->exception_value.as.string_val = strdup(CANCELLED_ERROR_MSG);
            if (!current_coro->exception_value.as.string_val) {
                 report_error("System", "Failed to strdup CANCELLED_ERROR_MSG for ready queue coro.", NULL);
            }
        } else { // If the coroutine is not already done, execute a "tick" of its body.
            // Not cancelled and not done, so execute a tick.
            interpreter->current_executing_coroutine = current_coro;
#ifdef DEBUG_ECHOC
            fprintf(stderr, "EVENT_LOOP_DEBUG: Processing coro %s (%p) from readyQ. State: %d\n",
                    current_coro->name ? current_coro->name : "unnamed", (void*)current_coro, current_coro->state);
            fflush(stderr);
#endif
            interpret_coroutine_body(interpreter, current_coro);
            interpreter->current_executing_coroutine = NULL;
        }
    }

    // After execution, check if the coroutine is now finished.
    if (current_coro->state == CORO_DONE) {
        // The coroutine finished its execution in the last tick.
        // Now we process its completion, waking any waiters or parent gather tasks.
        // This is a complex block, so we'll create a helper function for clarity.
        handle_completed_coroutine(interpreter, current_coro);
    }
    else if (current_coro->state == CORO_SUSPENDED_TIMER) {
        // The coroutine was an async_sleep task that was just placed on the sleep queue by interpret_coroutine_body.
        // No further action is needed here.
        DEBUG_PRINTF("Event Loop: Coro %s (%p) suspended for timer.", current_coro->name ? current_coro->name : "unnamed", (void*)current_coro);
    }
    else if (current_coro->state == CORO_SUSPENDED_AWAIT) {
        // The coroutine has successfully suspended via 'await'. It should NOT be re-queued by the event loop.
        // No further action is needed here.
        DEBUG_PRINTF("Event Loop: Coro %s (%p) suspended for await.", current_coro->name ? current_coro->name : "unnamed", (void*)current_coro);
    }
    else if (current_coro->state == CORO_GATHER_WAIT) {
        // This is a gather task that is now waiting for its children.
        // It should NOT be re-queued. It will be woken up by handle_completed_coroutine
        // when its last child finishes and sets its state to CORO_DONE.
        DEBUG_PRINTF("Event Loop: gather_task %s (%p) is waiting. Not re-queuing.", current_coro->name ? current_coro->name : "unnamed_gather", (void*)current_coro);
    }

    // Finally, the event loop releases its reference to the coroutine for this tick.
    // If the coroutine is suspended or done, other references (from waiters, etc.) will keep it alive.
    // If it was re-queued, its ref_count was incremented again by add_to_ready_queue.
    coroutine_decref_and_free_if_zero(current_coro);
}
#ifdef DEBUG_ECHOC
    fprintf(stderr, "EVENT_LOOP_DEBUG: Exited run_event_loop. ReadyQ_Head: %p, SleepQ_Head: %p\n",
                 (void*)interpreter->async_ready_queue_head,
                 (void*)interpreter->async_sleep_queue_head);
    fflush(stderr);
#endif
    interpreter->async_event_loop_active = 0; // Indicate event loop has finished
}

// Helper function to handle a coroutine that has completed its execution.
// This includes waking up its waiters and notifying its parent gather task if any.
static void handle_completed_coroutine(Interpreter* interpreter, Coroutine* done_coro) {
    char* done_coro_name_log = done_coro->name ? strdup(done_coro->name) : strdup("unnamed_done_coro");
    if (!done_coro_name_log) {
        report_error("System", "Failed to allocate memory for done_coro_name_log.", NULL);
    }
    DEBUG_PRINTF("Event Loop: Coro %s (%p) is DONE. Processing waiters and parent gather task.", done_coro_name_log, (void*)done_coro);

    // Handle parent gather task
    if (done_coro->parent_gather_coro) {
        Coroutine* parent_gather = done_coro->parent_gather_coro;
        // Ensure parent_gather is still valid before accessing its members
        if (parent_gather && parent_gather->magic_number == COROUTINE_MAGIC && parent_gather->state != CORO_DONE) {
            for (int i = 0; i < parent_gather->gather_tasks->count; ++i) {
                // New safe logic
            if (parent_gather->gather_tasks->elements[i].type == VAL_COROUTINE &&
                parent_gather->gather_tasks->elements[i].as.coroutine_val == done_coro) {
                    // Store the result from the completed child.
                    if (parent_gather->gather_results->elements[i].type != VAL_NULL) {
                        free_value_contents(parent_gather->gather_results->elements[i]);
                    }
                    if (done_coro->has_exception) {
                        parent_gather->gather_results->elements[i] = value_deep_copy(done_coro->exception_value);
                        if (parent_gather->gather_first_exception_idx == -1) {
                            parent_gather->gather_first_exception_idx = i;
                        }
                    } else {
                        parent_gather->gather_results->elements[i] = value_deep_copy(done_coro->result_value);
                    }

                    // Decrement pending count and check if the gather task is now complete.
                    parent_gather->gather_pending_count--;
                    bool is_gather_task_now_done = (parent_gather->gather_pending_count == 0);

                    DEBUG_PRINTF("Event Loop: Child coro %s (%p) completed for gather task %s (%p). Pending: %d",
                                 done_coro_name_log, (void*)done_coro,
                                 parent_gather->name ? parent_gather->name : "unnamed_gather", (void*)parent_gather,
                                 parent_gather->gather_pending_count);

                    // Clean up the child task from the gather list.
                    free_value_contents(parent_gather->gather_tasks->elements[i]);
                    parent_gather->gather_tasks->elements[i] = create_null_value();

                    // If the gather task is now done, finalize it and schedule it to wake its awaiter.
                    if (is_gather_task_now_done) {
                        DEBUG_PRINTF("Gather task %s (%p) is now complete. Finalizing.", parent_gather->name, (void*)parent_gather);
                        parent_gather->state = CORO_DONE;
                        parent_gather->has_exception = 0;

                        // The final result of a gather task is a NEW array containing the results
                        // of all its children. This decouples the result from the internal storage.

                        // 1. Create a new Array struct for the final result.
                        Array* final_results_array = (Array*)malloc(sizeof(Array));
                        if (!final_results_array) report_error("System", "Failed to allocate final results array for gather.", NULL);
                        
                        final_results_array->count = parent_gather->gather_results->count;
                        final_results_array->capacity = parent_gather->gather_results->capacity;
                        
                        if (final_results_array->count > 0) {
                            final_results_array->elements = (Value*)malloc(sizeof(Value) * final_results_array->capacity);
                            if (!final_results_array->elements) {
                                free(final_results_array);
                                report_error("System", "Failed to allocate elements for final gather results array.", NULL);
                            }
                            // 2. Deep-copy each result from the gather task's internal storage into our new public-facing array.
                            for (int i = 0; i < final_results_array->count; i++) {
                                final_results_array->elements[i] = value_deep_copy(parent_gather->gather_results->elements[i]);
                            }
                        } else {
                            final_results_array->elements = NULL;
                        }

                        // 3. Free the old placeholder result_value before assigning the new one.
                        if (parent_gather->result_value.type != VAL_NULL) {
                            free_value_contents(parent_gather->result_value);
                        }

                        // 4. Assign the new array as the gather task's final result.
                        parent_gather->result_value.type = VAL_ARRAY;
                        parent_gather->result_value.as.array_val = final_results_array;

                        // The internal gather_results array will be freed when the gather coroutine is freed.
                        
                        // Now that the gather task is done and has its result,
                        // add it to the ready queue so it can wake up its awaiter.
                        add_to_ready_queue(interpreter, parent_gather);
                    }

                    // Now, it is safe to release the child's reference to the parent.
                    // This is the LAST operation involving the parent_gather pointer in this block.
                }
            }
        }
        // After interacting with the parent, the child releases its strong reference.
        done_coro->parent_gather_coro = NULL; // Break the link
    }

    CoroutineWaiterNode* waiter_node = done_coro->waiters_head;
    done_coro->waiters_head = NULL;
    while (waiter_node) {
        CoroutineWaiterNode* next_waiter_node = waiter_node->next;
        Coroutine* waiter = waiter_node->waiter_coro;
        if (waiter->state == CORO_SUSPENDED_AWAIT && waiter->awaiting_on_coro == done_coro) {
            if (waiter->value_from_await.type != VAL_NULL) free_value_contents(waiter->value_from_await);
            waiter->resumed_with_exception = done_coro->has_exception;
            if (done_coro->has_exception) {
                waiter->value_from_await = value_deep_copy(done_coro->exception_value);
            } else {
                waiter->value_from_await = value_deep_copy(done_coro->result_value);
            }
            waiter->awaiting_on_coro = NULL;
            coroutine_decref_and_free_if_zero(done_coro);
            waiter->state = CORO_RESUMING;
            add_to_ready_queue(interpreter, waiter);
        }
        free(waiter_node);
        waiter_node = next_waiter_node;
    }
    if (done_coro_name_log) {
        free(done_coro_name_log);
    }
}