#include "header.h"
#include "interpreter.h"      // Include the new header for its own declarations
#include "modules/builtins.h" // Include the builtins header
#include <time.h>             // For time() in event loop
#include "parser_utils.h"      // For token_type_to_string
#include "expression_parser.h" // For actual expression parsing functions
#include "statement_parser.h"  // For actual statement parsing functions

// Forward declarations for dictionary functions to avoid implicit declaration warnings/conflicts
Dictionary* dictionary_create(int initial_buckets, Token* error_token);
void dictionary_set(Dictionary* dict, const char* key_str, Value value, Token* error_token);
Value dictionary_get(Dictionary* dict, const char* key, Token* error_token);
// Forward declaration for symbol table lookup
Value* symbol_table_get(Scope* scope, const char* var_name);

// NOTE: The expression parsing functions (interpret_ternary_expr, interpret_primary_expr, etc.)
// and statement parsing functions (interpret_statement, interpret_block_statement, etc.)
// are now expected to come from expression_parser.c and statement_parser.c respectively,
// included via their headers. Stubs/partial implementations previously in this file should be removed.

// -----------------------------------------------------------------
// Removed stub/partial implementations of expression parsing functions
// (e.g., interpret_primary_expr, interpret_postfix_expr, interpret_ternary_expr, etc.)
// These are now provided by expression_parser.c

// Removed stub implementations of statement parsing functions
// (e.g., interpret_statement, interpret_block_statement, etc.)
// These are now provided by statement_parser.c
// this code has more comments than actual code 💀 damn gemini

// Main interpret function: process statements until EOF.
void interpret(Interpreter* interpreter) {
    while (interpreter->current_token->type != TOKEN_EOF)
        interpret_statement(interpreter);
}

// --- Async Event Loop and Coroutine Management ---

void add_to_ready_queue(Interpreter* interpreter, Coroutine* coro) {
    CoroutineQueueNode* new_node = malloc(sizeof(CoroutineQueueNode));
    if (!new_node) report_error("System", "Failed to allocate CoroutineQueueNode.", NULL);
    new_node->coro = coro;
    new_node->next = NULL;

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
    if (!interpreter->async_ready_queue_head) return NULL;

    CoroutineQueueNode* head_node = interpreter->async_ready_queue_head;
    Coroutine* coro = head_node->coro;
    interpreter->async_ready_queue_head = head_node->next;
    if (!interpreter->async_ready_queue_head) {
        interpreter->async_ready_queue_tail = NULL;
    }
    free(head_node);
    DEBUG_PRINTF("Got coro %s (%p) from ready queue. State: %d", coro->name ? coro->name : "unnamed", (void*)coro, coro->state);
    return coro;
}

void run_event_loop(Interpreter* interpreter) {
    interpreter->async_event_loop_active = 1;
    DEBUG_PRINTF("%s", "--- Event Loop Started ---");

    while (interpreter->async_ready_queue_head) {
        Coroutine* current_coro = get_from_ready_queue(interpreter);
        if (!current_coro) break; // Should not happen if head was not NULL

        DEBUG_PRINTF("Event Loop: Processing coro %s (%p), state: %d", current_coro->name ? current_coro->name : "unnamed_coro", (void*)current_coro, current_coro->state);

        if (current_coro->is_cancelled && current_coro->state != CORO_DONE) {
            DEBUG_PRINTF("Event Loop: Coro %s (%p) is cancelled. Transitioning to DONE.", current_coro->name, (void*)current_coro);
            current_coro->state = CORO_DONE;
            current_coro->has_exception = 1;
            free_value_contents(current_coro->exception_value); // Free old exception_value
            current_coro->exception_value.type = VAL_STRING;
            current_coro->exception_value.as.string_val = strdup(CANCELLED_ERROR_MSG);
            // If this cancelled coroutine was awaited by another, that await will now pick up the CancelledError.
            // If it's part of a gather, gather logic will handle it.
        }

        interpreter->current_executing_coroutine = current_coro;
        interpreter->coroutine_yielded_for_await = 0; // Reset flag

        if (current_coro->state == CORO_RUNNABLE) {
            current_coro->state = CORO_RUNNING; // Mark as running for this turn
            if (current_coro->function_def) { // EchoC function based coroutine
                // Save the interpreter's global state ONLY if we are about to switch
                // to a coroutine that might have a *different* source text.
                // However, for a single event loop processing its queue, the primary
                // lexer and token should just advance. The coroutine's resume_state handles its specific restart point.

                // Restore context for this coroutine
                Scope* old_scope = interpreter->current_scope;
                Object* old_self_obj = interpreter->current_self_object; // Async methods not yet supported
                // LexerState old_lexer_state = get_lexer_state(interpreter->lexer); // Marked as unused
                // Token* old_token = token_deep_copy(interpreter->current_token); // Marked as unused

                // The interpreter's main lexer and token are now dedicated to the current coroutine's execution.
                // No need to save/restore them around this block if this is the primary execution flow.
                // The coroutine's resume_state dictates where ITS execution begins.

                // Manage function context for coroutine execution
                interpreter->function_nesting_level++;
                interpreter->return_flag = 0; // Reset before executing coroutine statements
                interpreter->current_function_return_value = create_null_value();

                interpreter->current_scope = current_coro->execution_scope;
                set_lexer_state(interpreter->lexer, current_coro->resume_state);
                free_token(interpreter->current_token);
                interpreter->current_token = get_next_token(interpreter->lexer);

                interpret_coroutine_body(interpreter, current_coro);

                // After interpret_coroutine_body, interpreter->current_token and interpreter->lexer
                // are at the point where the coroutine finished or yielded.
                // interpreter->current_token is the token that would be processed next if not yielded.

                if (current_coro->state != CORO_DONE && interpreter->current_token) { // If not done, it must have yielded or suspended
                    current_coro->resume_state = get_lexer_state_for_token_start(interpreter->lexer, interpreter->current_token->line, interpreter->current_token->col, interpreter->current_token);
                }

                // Restore function context
                interpreter->function_nesting_level--;

                // DO NOT restore old_lexer_state and old_token here.
                // The interpreter's state should reflect the progress made by the coroutine.
                interpreter->current_scope = old_scope;
                interpreter->current_self_object = old_self_obj;

            } else if (current_coro->wakeup_time_sec > 0) { // C-created timer coroutine (e.g. async_sleep)
                if ((double)time(NULL) >= current_coro->wakeup_time_sec) {
                    current_coro->state = CORO_DONE;
                    current_coro->result_value = create_null_value(); // Sleep resolves to null
                } else {
                    current_coro->state = CORO_SUSPENDED_TIMER; // Keep it suspended
                }
            } else if (current_coro->gather_tasks) { // C-created gather coroutine
                // Gather logic: activate children, then wait.
                current_coro->state = CORO_GATHER_WAIT;
                for (int i = 0; i < current_coro->gather_tasks->count; ++i) {
                    Coroutine* child_coro = current_coro->gather_tasks->elements[i].as.coroutine_val;
                    child_coro->parent_gather_coro = current_coro; // Link child to parent gather
                    if (child_coro->state == CORO_NEW) {
                        child_coro->state = CORO_RUNNABLE;
                        add_to_ready_queue(interpreter, child_coro);
                    }
                }
            }
        }

        // Post-execution processing for the current_coro
        if (current_coro->state == CORO_DONE) {
            DEBUG_PRINTF("Event Loop: Coro %s (%p) is DONE.", current_coro->name, (void*)current_coro);

            // Resume any coroutines that were waiting on this one
            CoroutineWaiterNode* waiter_node = current_coro->waiters_head;
            while (waiter_node) {
                    Coroutine* waiter = waiter_node->waiter_coro; // The coroutine that was waiting
                // Ensure the waiter is actually in a suspended state and was waiting on this specific coroutine
                if (waiter->state == CORO_SUSPENDED_AWAIT && waiter->awaiting_on_coro == current_coro) {
                    waiter->state = CORO_RUNNABLE;
                    waiter->is_resumed_from_await = 1;
                    free_value_contents(waiter->value_from_await); // Clear old value

                    if (current_coro->has_exception) {
                        waiter->value_from_await = value_deep_copy(current_coro->exception_value);
                        // The interpret_await_expr resumption logic will handle setting interpreter->exception_is_active
                    } else {
                        waiter->value_from_await = value_deep_copy(current_coro->result_value);
                    }
                    // Decrement ref_count of the coroutine that was awaited (current_coro)
                    // as this waiter is no longer awaiting it.
                    if (waiter->awaiting_on_coro) { // Should be current_coro
                        Value temp_val_for_free;
                        temp_val_for_free.type = (waiter->awaiting_on_coro->gather_tasks) ? VAL_GATHER_TASK : VAL_COROUTINE;
                        temp_val_for_free.as.coroutine_val = waiter->awaiting_on_coro;
                        DEBUG_PRINTF("AWAIT_RESUME: Coro %s (%p) releasing (ref_dec) awaited coro %s (%p)", waiter->name ? waiter->name : "unnamed_w", (void*)waiter, waiter->awaiting_on_coro->name ? waiter->awaiting_on_coro->name : "unnamed_target", (void*)waiter->awaiting_on_coro);
                        free_value_contents(temp_val_for_free); // This will decrement ref_count
                    }
                    waiter->awaiting_on_coro = NULL; // No longer awaiting this specific one
                    add_to_ready_queue(interpreter, waiter);
                    DEBUG_PRINTF("Event Loop: Woke up waiter %s (%p) from completed %s (%p)",
                                 waiter->name ? waiter->name : "unnamed_w", (void*)waiter,
                                 current_coro->name ? current_coro->name : "unnamed_c", (void*)current_coro);
                }
                waiter_node = waiter_node->next;
            }
            // The waiters_head list itself will be freed when current_coro's ref_count reaches 0.

            // If this coroutine was part of a gather task:
            if (current_coro->parent_gather_coro) {
                Coroutine* gather_parent = current_coro->parent_gather_coro;
                int child_idx = -1; // Find this child's index in the parent's task list
                for (int i = 0; i < gather_parent->gather_tasks->count; ++i) {
                    if (gather_parent->gather_tasks->elements[i].as.coroutine_val == current_coro) {
                        child_idx = i;
                        break;
                    }
                }
                if (child_idx != -1) {
                    // Ensure gather_results array is large enough (should be from creation)
                    if (child_idx < gather_parent->gather_results->capacity) {
                         free_value_contents(gather_parent->gather_results->elements[child_idx]); // Free old placeholder
                        if (current_coro->has_exception) {
                            gather_parent->gather_results->elements[child_idx] = value_deep_copy(current_coro->exception_value);
                            if (gather_parent->gather_first_exception_idx == -1) {
                                gather_parent->gather_first_exception_idx = child_idx;
                            }
                        } else {
                            gather_parent->gather_results->elements[child_idx] = value_deep_copy(current_coro->result_value);
                        }
                        // If gather_results->count was used to track filled slots:
                        // gather_parent->gather_results->elements[gather_parent->gather_results->count++] = ...
                        // But using fixed indices is better here. We need to ensure count reflects actual size if used.
                        // For now, assume indices are pre-allocated.
                    }

                    gather_parent->gather_pending_count--;
                    if (gather_parent->gather_pending_count == 0) {
                        gather_parent->state = CORO_DONE;
                        if (gather_parent->gather_first_exception_idx != -1) {
                            gather_parent->has_exception = 1;
                            free_value_contents(gather_parent->exception_value);
                            gather_parent->exception_value = value_deep_copy(gather_parent->gather_results->elements[gather_parent->gather_first_exception_idx]);
                        }
                        // The result of gather is the array of results. Create a Value wrapper for deep copy.
                        free_value_contents(gather_parent->result_value); // Free old result
                        gather_parent->result_value.type = VAL_ARRAY;

                        Value source_array_val_for_copy;
                        source_array_val_for_copy.type = VAL_ARRAY;
                        source_array_val_for_copy.as.array_val = gather_parent->gather_results;

                        Value copied_results_array_value = value_deep_copy(source_array_val_for_copy);
                        gather_parent->result_value.type = VAL_ARRAY; // Ensure type is set
                        gather_parent->result_value.as.array_val = copied_results_array_value.as.array_val;

                        add_to_ready_queue(interpreter, gather_parent); // Make gather task runnable to be awaited
                    }
                }
            }
        } else if (current_coro->state == CORO_SUSPENDED_AWAIT || current_coro->state == CORO_SUSPENDED_TIMER || current_coro->state == CORO_GATHER_WAIT) {
            // If suspended, it's waiting. Add back to a list of suspended coroutines or re-evaluate later.
            // For simplicity, if it's timer based and not yet time, or gather_wait, don't re-add to ready queue yet.
            // If SUSPENDED_AWAIT, it will be re-added when its target completes.
            // For now, timer and gather_wait coroutines are implicitly polled by being re-queued if not done.
            if (current_coro->state == CORO_SUSPENDED_TIMER && (double)time(NULL) < current_coro->wakeup_time_sec) {
                add_to_ready_queue(interpreter, current_coro); // Re-queue to check later
            } else if (current_coro->state == CORO_SUSPENDED_TIMER) { // Timer expired
                current_coro->state = CORO_RUNNABLE; // Make it runnable to become DONE in next cycle
                add_to_ready_queue(interpreter, current_coro);
            }
            // GATHER_WAIT coroutines are woken up by their children.
            // SUSPENDED_AWAIT coroutines are woken up by the coroutine they await.
        } else if (current_coro->state == CORO_RUNNING) {
            // If it's still RUNNING here, it means it yielded implicitly (end of its timeslice, not yet implemented)
            // or there's a logic error. For now, assume explicit await causes suspension.
            // If it was supposed to suspend on await, `coroutine_yielded_for_await` would be set.
            // If it's still RUNNING, it means it didn't complete and didn't await.
            // This shouldn't happen with current `interpret_coroutine_body` which loops until end/await/return.
        }

        interpreter->current_executing_coroutine = NULL;

        // Check for other runnable coroutines (e.g. one that was waiting on current_coro)
        // This scan is inefficient. A better way is for completed coroutines to directly schedule their awaiters.
        // For now, let's assume `interpret_await_expr` handles the resumption when `target_coro->state == CORO_DONE`.
    } // while ready queue has items

    interpreter->async_event_loop_active = 0;
    DEBUG_PRINTF("%s", "--- Event Loop Finished ---");
    if (interpreter->exception_is_active) {
        // If an exception propagated out of the event loop (e.g., from the initial 'run:' task)
        report_error("Runtime", interpreter->current_exception.as.string_val, NULL); // Or a better token
    }
}

// In interpreter.c (or wherever run_event_loop is)
StatementExecStatus interpret_coroutine_body(Interpreter* interpreter, Coroutine* coro_to_run) {
    Function* func_def = coro_to_run->function_def;
    StatementExecStatus overall_status = STATEMENT_EXECUTED_OK;

    // Loop to execute statements until yield, return, or end of function
    while (coro_to_run->state == CORO_RUNNING && interpreter->current_token->type != TOKEN_EOF) {
        if (coro_to_run->is_cancelled) { // Check for cancellation at start of each "statement"
            coro_to_run->state = CORO_DONE;
            coro_to_run->has_exception = 1;
            free_value_contents(coro_to_run->exception_value);
            coro_to_run->exception_value.type = VAL_STRING;
            coro_to_run->exception_value.as.string_val = strdup(CANCELLED_ERROR_MSG);
            overall_status = STATEMENT_PROPAGATE_FLAG; // Signal to event loop
            break;
        }
        // Check for natural end of function body
        if (interpreter->current_token->type == TOKEN_END &&
            (func_def->body_end_token_original_line == -1 ||
             (interpreter->current_token->line == func_def->body_end_token_original_line &&
              interpreter->current_token->col == func_def->body_end_token_original_col))) {
            
            coro_to_run->state = CORO_DONE;
            // Result is from interpreter->current_function_return_value (default null if no return)
            free_value_contents(coro_to_run->result_value);
            // Ensure current_function_return_value is initialized if no return statement was hit
            if (interpreter->current_function_return_value.type == VAL_NULL && interpreter->return_flag == 0) {} // It's already null
            coro_to_run->result_value = value_deep_copy(interpreter->current_function_return_value);
            overall_status = STATEMENT_EXECUTED_OK; // Or a specific "completed" status
            break; 
        }

        StatementExecStatus statement_status = interpret_statement(interpreter);

        if (statement_status == STATEMENT_YIELDED_AWAIT) {
            // interpret_await_expr already set coro_to_run->state = CORO_SUSPENDED
            // and interpreter->coroutine_yielded_for_await = 1
            // and interpreter->coroutine_yielded_for_await = 1
            overall_status = STATEMENT_YIELDED_AWAIT;
            break; // Exit this coroutine's execution turn
        }
        
        if (interpreter->return_flag) {
            coro_to_run->state = CORO_DONE;
            free_value_contents(coro_to_run->result_value);
            coro_to_run->result_value = value_deep_copy(interpreter->current_function_return_value);
            interpreter->return_flag = 0;
            overall_status = STATEMENT_EXECUTED_OK; // Or "completed"
            break;
        }

        if (interpreter->exception_is_active) {
            coro_to_run->state = CORO_DONE; // Unhandled exception
            coro_to_run->has_exception = 1;
            free_value_contents(coro_to_run->exception_value); // Free old, then copy current
            coro_to_run->result_value = value_deep_copy(interpreter->current_exception);
            // Do not clear interpreter->exception_is_active here, let event loop see it.
            overall_status = STATEMENT_PROPAGATE_FLAG;
            break;
        }
        // Break/Continue flags are relevant for loops within the coroutine, not the coroutine execution itself.
        // They should be handled by loop constructs and reset.
    }
    return overall_status;
}

// -----------------------------------------------------------------
// Stubs for functions that might still be forward-declared here but implemented elsewhere
// (e.g., dictionary_get, symbol_table_get) are generally okay if those files are linked.
// However, the primary parser stubs that caused the infinite loop (empty interpret_statement)
// and incorrect behavior (expression stubs not consuming tokens) are removed.
// The actual implementations of dictionary_get, symbol_table_get, etc.,
// are in their respective .c files (dictionary.c, scope.c) which are now
// included in C_SOURCE_FILES.