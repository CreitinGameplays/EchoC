// src_c/main.c
#include "header.h"
#include "module_loader.h" // For initialize_module_system, cleanup_module_system
#include "value_utils.h"   // For coroutine_decref_and_free_if_zero
#include "dictionary.h"    // For dictionary_set

#include "scope.h"         // For symbol_table_set, free_scope
#include <sys/stat.h>      // For stat() to check file type


// Define global log file pointer
FILE* echoc_debug_log_file = NULL; // Define global log file pointer

Interpreter* g_interpreter_for_error_reporting = NULL;
#ifdef DEBUG_ECHOC
#endif

// Define global ID counters (declared as extern in header.h)
uint64_t next_scope_id = 0;
uint64_t next_dictionary_id = 0;
uint64_t next_object_id = 0;

// Implementation of free_value_contents (forward declared in header.c)
void free_value_contents(Value val) {
    if (val.type == VAL_STRING && val.as.string_val != NULL) {
        free(val.as.string_val);
    } else if (val.type == VAL_ARRAY && val.as.array_val != NULL) {
        for (int i = 0; i < val.as.array_val->count; ++i) {
            free_value_contents(val.as.array_val->elements[i]);
        }
        free(val.as.array_val->elements);
        free(val.as.array_val);
    } else if (val.type == VAL_TUPLE && val.as.tuple_val != NULL) {
        for (int i = 0; i < val.as.tuple_val->count; ++i) {
            free_value_contents(val.as.tuple_val->elements[i]);
        }
        if (val.as.tuple_val->elements) free(val.as.tuple_val->elements); // elements can be NULL for empty tuple
        free(val.as.tuple_val);
    } else if (val.type == VAL_DICT && val.as.dict_val != NULL) {
        Dictionary* dict = val.as.dict_val;
        for (int i = 0; i < dict->num_buckets; ++i) {
            DictEntry* entry = dict->buckets[i];
            while (entry) {
                DictEntry* next_entry = entry->next;
                free(entry->key);
                free_value_contents(entry->value);
                free(entry);
                entry = next_entry;
            }
        }
        free(dict->buckets);
        free(dict);
    } else if (val.type == VAL_FUNCTION && val.as.function_val != NULL) {
        Function* func = val.as.function_val;
        free(func->name);
        if (func->params) {
            for (int i = 0; i < func->param_count; ++i) {
                free(func->params[i].name);
                if (func->params[i].default_value) {
                    free_value_contents(*(func->params[i].default_value));
                    free(func->params[i].default_value);
                }
            }
            free(func->params);
        }
        // Only free source_text_owned_copy if this Function instance is marked as its owner.
        // This prevents double-free if multiple Value structs point to the same Function struct
        // (e.g., one in symbol table, one temporary Value being freed), and only one
        // should be responsible for the text.
        if (func->source_text_owned_copy && func->is_source_owner) { // Check ownership flag // TOKEN_END removed
            free(func->source_text_owned_copy);
        }
        // func->definition_scope is not freed here; scopes are managed by enter/exit_scope
        free(func);
    }
    // OOP related ValueTypes
    else if (val.type == VAL_BLUEPRINT) {
        // VAL_BLUEPRINT values are pointers to the canonical Blueprint definition.
        // The actual Blueprint struct is freed when its defining symbol is freed (see free_symbol_nodes).
        // Thus, free_value_contents does nothing for VAL_BLUEPRINT here.
        } else if (val.type == VAL_OBJECT && val.as.object_val != NULL) {
        Object* obj = val.as.object_val;
        
        obj->ref_count--;
        DEBUG_PRINTF("FREE_VALUE_CONTENTS: Decremented ref_count for object %s (%p) to %d", obj->blueprint ? obj->blueprint->name : "unnamed_obj", (void*)obj, obj->ref_count);
        if (obj->ref_count == 0) {
            DEBUG_PRINTF("FREE_VALUE_CONTENTS: Freeing actual Object struct %s (%p)", obj->blueprint ? obj->blueprint->name : "unnamed_obj", (void*)obj);
            if (obj->instance_attributes) {
                // Pass the interpreter context if available, NULL otherwise for general cleanup
                free_scope(obj->instance_attributes); 
            }
            free(obj); // Free the Object struct itself
        }
    } else if (val.type == VAL_BOUND_METHOD && val.as.bound_method_val != NULL) {
        BoundMethod* bm = val.as.bound_method_val;
        bm->ref_count--;
        DEBUG_PRINTF("FREE_VALUE_CONTENTS: Decremented ref_count for bound method (%p) to %d", (void*)bm, bm->ref_count);
        if (bm->ref_count == 0) {
            DEBUG_PRINTF("FREE_VALUE_CONTENTS: Freeing actual BoundMethod struct (%p)", (void*)bm);
            if (bm->self_is_owned_copy) {
                free_value_contents(bm->self_value);
            }
            free(bm);
        }
    } else if ((val.type == VAL_COROUTINE || val.type == VAL_GATHER_TASK) && val.as.coroutine_val != NULL) {
        coroutine_decref_and_free_if_zero(val.as.coroutine_val);
    }
    // VAL_SUPER_PROXY has no dynamic content in its union part.
    // VAL_NULL has no dynamic content.
    // VAL_INT, VAL_FLOAT, VAL_BOOL don't have dynamically allocated contents to free here
}

// Implementation of value_deep_copy (forward declared in header.c)
Value value_deep_copy(Value original) {
    Value copy = original; // Start with a shallow copy for simple types
    
    if (original.type == VAL_STRING && original.as.string_val != NULL) {
        copy.as.string_val = strdup(original.as.string_val);
        if (!copy.as.string_val) report_error("System", "Failed to strdup string in value_deep_copy", NULL);
    } else if (original.type == VAL_STRING && original.as.string_val == NULL) {
        // This case should ideally not occur if VAL_STRING always implies a valid string pointer.
        // However, to be robust, handle it by creating an empty string.
        copy.as.string_val = strdup("");
        if (!copy.as.string_val) report_error("System", "Failed to strdup empty string for NULL VAL_STRING", NULL);
    } else if (original.type == VAL_ARRAY && original.as.array_val != NULL) {
        Array* original_array = original.as.array_val;
        Array* new_array = malloc(sizeof(Array));
        if (!new_array) report_error("System", "Failed to allocate memory for array copy", NULL);
        new_array->count = original_array->count;
        new_array->capacity = original_array->capacity; // Or could start fresh with count as capacity
        new_array->elements = malloc(new_array->capacity * sizeof(Value));
        if (!new_array->elements) { free(new_array); report_error("System", "Failed to allocate memory for copied array elements", NULL); }
        
        for (int i = 0; i < new_array->count; ++i) {
            new_array->elements[i] = value_deep_copy(original_array->elements[i]);
        }
        copy.as.array_val = new_array;
    } else if (original.type == VAL_TUPLE && original.as.tuple_val != NULL) {
        Tuple* original_tuple = original.as.tuple_val;
        Tuple* new_tuple = malloc(sizeof(Tuple));
        if (!new_tuple) report_error("System", "Failed to allocate memory for tuple copy", NULL);
        new_tuple->count = original_tuple->count;
        if (new_tuple->count > 0) {
            new_tuple->elements = malloc(new_tuple->count * sizeof(Value)); // Tuples are fixed size
            if (!new_tuple->elements) { free(new_tuple); report_error("System", "Failed to allocate memory for copied tuple elements", NULL); }
            for (int i = 0; i < new_tuple->count; ++i) {
                new_tuple->elements[i] = value_deep_copy(original_tuple->elements[i]);
            }
        } else {
            new_tuple->elements = NULL; // Empty tuple
        }
        copy.as.tuple_val = new_tuple;
    } else if (original.type == VAL_DICT && original.as.dict_val != NULL) {
        // --- START: New, more direct dictionary copy logic ---
        Dictionary* original_dict = original.as.dict_val;
        Dictionary* new_dict = dictionary_create(original_dict->num_buckets, NULL);
        
        // Manually iterate and insert to avoid the recursive nature and side-effects
        // of using dictionary_set (which can resize) during a copy.
        for (int i = 0; i < original_dict->num_buckets; ++i) {
            DictEntry* original_entry = original_dict->buckets[i];
            while (original_entry) {
                // 1. Create the new entry with deep-copied value.
                // This helper also strdups the key.
                DictEntry* new_entry = dictionary_create_entry(original_entry->key, original_entry->value, NULL);

                // 2. Insert it into the new dictionary's hash table directly.
                unsigned long hash = hash_string(new_entry->key);
                int index = hash % new_dict->num_buckets;
                new_entry->next = new_dict->buckets[index];
                new_dict->buckets[index] = new_entry;
                new_dict->count++;

                original_entry = original_entry->next;
            }
        }
        copy.as.dict_val = new_dict;
        // --- END: New, more direct dictionary copy logic ---
    } else if (original.type == VAL_FUNCTION && original.as.function_val != NULL) {
        // Functions are typically "copied" by reference to their definition.
        // Here, we create a new Function struct but it points to the same underlying
        // definition details (body location, definition scope).
        // The name and parameters are duplicated as they are part of the Function struct.
        Function* original_func = original.as.function_val;
        Function* new_func = calloc(1, sizeof(Function));
        if (!new_func) {
            report_error("System", "Failed to allocate memory for function copy", NULL);
        }

        // Initialize all potentially allocated pointers to NULL for safe cleanup
        new_func->name = NULL;
        new_func->params = NULL;
        new_func->source_text_owned_copy = NULL;
        // Other fields will be copied directly or are not pointers needing cleanup here

        // Copy name
        new_func->name = strdup(original_func->name);
        if (!new_func->name) {
            free(new_func);
            report_error("System", "Failed to strdup function name in copy", NULL);
        }

        // Copy parameters
        new_func->param_count = original_func->param_count;
        // Only copy parameters if they exist on the original function (i.e., it's an EchoC function)
        if (new_func->param_count > 0 && original_func->params) {
            new_func->params = malloc(new_func->param_count * sizeof(Parameter));
            if (!new_func->params) {
                free(new_func->name);
                free(new_func);
                report_error("System", "Failed to alloc params for func copy", NULL);
            }
            // Initialize all param sub-pointers to NULL
            for (int i = 0; i < new_func->param_count; ++i) {
                new_func->params[i].name = NULL;
                new_func->params[i].default_value = NULL;
            }

            for (int i = 0; i < new_func->param_count; ++i) {
                new_func->params[i].name = strdup(original_func->params[i].name);
                if (!new_func->params[i].name) {
                    // Cleanup already allocated parts of new_func
                    for (int j = 0; j < i; ++j) { // Free successfully copied params before this one
                        free(new_func->params[j].name);
                        if (new_func->params[j].default_value) {
                            free_value_contents(*(new_func->params[j].default_value));
                            free(new_func->params[j].default_value);
                        }
                    }
                    free(new_func->params);
                    free(new_func->name);
                    free(new_func);
                    report_error("System", "Failed to strdup param name", NULL);
                }

                if (original_func->params[i].default_value) {
                    new_func->params[i].default_value = (Value*)malloc(sizeof(Value));
                    if (!new_func->params[i].default_value) {
                        // Cleanup
                        for (int j = 0; j <= i; ++j) { // Param name for 'i' is allocated
                            free(new_func->params[j].name);
                             // Default values for params < i are allocated
                            if (j < i && new_func->params[j].default_value) {
                                free_value_contents(*(new_func->params[j].default_value));
                                free(new_func->params[j].default_value);
                            }
                        }
                        free(new_func->params);
                        free(new_func->name);
                        free(new_func);
                        report_error("System", "Failed to alloc for default value copy", NULL);
                    }
                    *(new_func->params[i].default_value) = value_deep_copy(*(original_func->params[i].default_value)); // Deep copy the default value itself
                } else {
                    new_func->params[i].default_value = NULL;
                }
            }
        } else { // param_count is 0
            new_func->params = NULL;
        }

        new_func->body_start_state = original_func->body_start_state; // Shallow copy first
        new_func->body_end_token_original_line = original_func->body_end_token_original_line;
        new_func->body_end_token_original_col = original_func->body_end_token_original_col;
        new_func->definition_col = original_func->definition_col;
        new_func->definition_line = original_func->definition_line;
        new_func->definition_scope = original_func->definition_scope; // Share the definition scope

        // The original function might just be a temporary wrapper that points to a shared source text.
        // The new copy MUST own its own copy of the source text to be safe.
        if (original_func->source_text_owned_copy) {
            new_func->source_text_owned_copy = strdup(original_func->source_text_owned_copy);
            if (!new_func->source_text_owned_copy) {
                // Cleanup
                if (new_func->params) {
                    for (int i = 0; i < new_func->param_count; ++i) {
                        if (new_func->params[i].name) free(new_func->params[i].name);
                        if (new_func->params[i].default_value) {
                            free_value_contents(*(new_func->params[i].default_value));
                            free(new_func->params[i].default_value);
                        }
                    }
                    free(new_func->params);
                }
                free(new_func->name);
                free(new_func);
                report_error("System", "Failed to strdup function source text in copy", NULL);
            }
        } else {
            new_func->source_text_owned_copy = NULL;
        }
        new_func->source_text_length = new_func->source_text_owned_copy ? strlen(new_func->source_text_owned_copy) : 0; 
        // Crucially, update the text pointer in the copied lexer state to point to our new owned copy.
        // This prevents the new function from holding a dangling pointer to a temporary source buffer.
        new_func->body_start_state.text = new_func->source_text_owned_copy; 
        new_func->is_async = original_func->is_async;
        new_func->c_impl = original_func->c_impl; // Copy C function pointer
        new_func->is_source_owner = (new_func->source_text_owned_copy != NULL); // The copy owns its strdup'd text
        copy.as.function_val = new_func;
    } else if (original.type == VAL_BLUEPRINT && original.as.blueprint_val != NULL) {
        // VAL_BLUEPRINT values are pointers to the canonical Blueprint definition.
        // "Deep copy" of a VAL_BLUEPRINT just copies the pointer.
        // The actual Blueprint struct is managed by its defining symbol.
        copy.as.blueprint_val = original.as.blueprint_val;
    } else if (original.type == VAL_OBJECT && original.as.object_val != NULL) {
        copy.as.object_val = original.as.object_val; // Copy the pointer
        if (copy.as.object_val) {
            copy.as.object_val->ref_count++; // Increment ref count
            DEBUG_PRINTF("VALUE_DEEP_COPY: Incremented ref_count for object %s (%p) to %d", copy.as.object_val->blueprint ? copy.as.object_val->blueprint->name : "unnamed_obj", (void*)copy.as.object_val, copy.as.object_val->ref_count);
        }
    } else if (original.type == VAL_BOUND_METHOD && original.as.bound_method_val != NULL) {
        copy.as.bound_method_val = original.as.bound_method_val;
        if (copy.as.bound_method_val) {
            copy.as.bound_method_val->ref_count++;
            DEBUG_PRINTF("VALUE_DEEP_COPY: Incremented ref_count for bound method (%p) to %d", (void*)copy.as.bound_method_val, copy.as.bound_method_val->ref_count);
        }
    } else if ((original.type == VAL_COROUTINE || original.type == VAL_GATHER_TASK) && original.as.coroutine_val != NULL) {
        // For VAL_COROUTINE and VAL_GATHER_TASK, "deep copy" means copying the pointer
        // and incrementing the reference count of the Coroutine struct.
        copy.as.coroutine_val = original.as.coroutine_val; // Copy the pointer
        if (copy.as.coroutine_val) {
            copy.as.coroutine_val->ref_count++; // Increment ref count
            DEBUG_PRINTF("VALUE_DEEP_COPY: Incremented ref_count for coro %s (%p) to %d", copy.as.coroutine_val->name ? copy.as.coroutine_val->name : "unnamed", (void*)copy.as.coroutine_val, copy.as.coroutine_val->ref_count);
        }
    } else if (original.type == VAL_NULL) {
        // VAL_NULL has no dynamic parts, shallow copy is fine.
    }
    return copy;
}

// Helper to free a coroutine queue and its coroutines.
// Takes pointers to head and tail to nullify them after processing.
void robust_free_coroutine_queue(Interpreter* interpreter, CoroutineQueueNode** p_head, CoroutineQueueNode** p_tail) {
    (void)interpreter; // Mark interpreter as unused for now
    CoroutineQueueNode* current_node_iter = *p_head;
    if (!current_node_iter) return;

    // Step 1: Collect all Coroutine pointers from the queue into a temporary buffer.
    // This avoids issues if free_value_contents indirectly modifies this queue or another.
    #define MAX_COROS_IN_QUEUE_CLEANUP 1024 // Max coroutines expected in a single queue during cleanup
    Coroutine* coros_to_process[MAX_COROS_IN_QUEUE_CLEANUP];
    int coro_collect_count = 0;

    while(current_node_iter && coro_collect_count < MAX_COROS_IN_QUEUE_CLEANUP) {
        coros_to_process[coro_collect_count++] = current_node_iter->coro;
        current_node_iter = current_node_iter->next;
    }

    if (current_node_iter) { // Buffer was too small
        DEBUG_PRINTF("CRITICAL_WARNING: robust_free_coroutine_queue exceeded temporary buffer for queue at %p. Some coroutines may leak.", (void*)*p_head);
        // In a production system, this might realloc or use a dynamic list.
    }

    // Step 2: Free the queue nodes themselves
    current_node_iter = *p_head; // Reset iterator to original head
    CoroutineQueueNode* next_queue_node;
    while (current_node_iter) {
        next_queue_node = current_node_iter->next;
        free(current_node_iter);
        current_node_iter = next_queue_node;
    }
    *p_head = NULL; // Nullify the interpreter's head pointer
    if (p_tail) *p_tail = NULL; // Nullify the interpreter's tail pointer

    // Step 3: Process the collected coroutines for deallocation
    for (int i = 0; i < coro_collect_count; ++i) {
        Coroutine* coro_to_free = coros_to_process[i];
        if (!coro_to_free) continue;

        Value temp_coro_val;
        temp_coro_val.type = (coro_to_free->gather_tasks ? VAL_GATHER_TASK : VAL_COROUTINE);
        temp_coro_val.as.coroutine_val = coro_to_free;
        DEBUG_PRINTF("ROBUST_FREE_QUEUE: Processing coro %s (%p), ref_count before free: %d",
                     coro_to_free->name ? coro_to_free->name : "unnamed", (void*)coro_to_free, coro_to_free->ref_count);
        free_value_contents(temp_coro_val); // Decrements ref_count, frees if 0
    }
}

// Helper to free a list of symbol nodes
void free_symbol_nodes(SymbolNode* symbols) {
    SymbolNode* current = symbols;
    SymbolNode* next;
    while (current != NULL) {
        next = current->next;

        // Special handling for 'self' to prevent freeing the object it refers to,
        // as 'self' is a reference within a method scope and does not own the object.
        // This check MUST happen before freeing current->name.
        bool is_self_object_reference = false; // Default to false
        if (current->name) { // Ensure current->name is not NULL before strcmp
            is_self_object_reference = (current->value.type == VAL_OBJECT && strcmp(current->name, "self") == 0);
        }

        if (current->name) {
            free(current->name);
            current->name = NULL; // Good practice
        }
        // The Blueprint struct itself is managed by the interpreter's all_blueprints_head list.
        // For other types, free_value_contents handles their dynamically allocated parts.
        if (!is_self_object_reference) {
            // unless it's a 'self' reference.
            // The inner redundant check 'if (!is_self_object_reference)' was removed.
            free_value_contents(current->value);
        }
        free(current);
        current = next;
    }
}



int main(int argc, char* argv[]) {
    char* initial_file_abs_path = NULL;
    #ifdef DEBUG_ECHOC
    echoc_debug_log_file = fopen("echoc_runtime_log.txt", "w");
    if (echoc_debug_log_file == NULL) {
        // If file opening fails, BUG_PRINTF/DEBUG_PRINTF will fall back to stderr.
        fprintf(stderr, "CRITICAL: Failed to open echoc_runtime_log.txt for writing. Runtime debug logs will go to stderr.\n");
    } else { // Change to full buffering for performance
        setvbuf(echoc_debug_log_file, NULL, _IOFBF, 4096); // Use full buffering with a 4KB buffer
        fprintf(echoc_debug_log_file, "--- EchoC Runtime Log Initialized ---\n"); // This will be buffered
    }
    // By keeping echoc_debug_log_file as NULL, no file logging will occur.
    // keep commented out so your CPU won't overload
    #endif

    if (argc != 2) {
        printf("Usage: %s <filename.echoc>\n", argv[0]);
        return 1;
    }

    // Check if the provided path is a file and not a directory.
    struct stat path_stat;
    if (stat(argv[1], &path_stat) != 0) {
        // If stat fails, the file likely doesn't exist or there's a permission issue.
        // fopen below will also fail, but this gives a slightly better early error.
        printf("Error: Cannot access path '%s'.\n", argv[1]);
        return 1;
    }
    if (S_ISDIR(path_stat.st_mode)) {
        printf("Error: Expected a file, but '%s' is a directory.\n", argv[1]);
        return 1;
    }

    FILE* file = fopen(argv[1], "rb");
    if (file == NULL) {
        printf("Error: Could not open file '%s'\\n", argv[1]);
        return 1;
    }

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fsize < 0) {
        fprintf(stderr, "Error: Could not determine size of file '%s'.\n", argv[1]);
        fclose(file);
        return 1;
    }

    char* source_code = malloc(fsize + 1);
    if (!source_code) {
        fprintf(stderr, "Error: Could not allocate memory to read file '%s'.\n", argv[1]);
        fclose(file);
        return 1;
    }

    size_t bytes_read = fread(source_code, 1, fsize, file);
    fclose(file); // Close file immediately after reading

    if (bytes_read != (size_t)fsize) {
        fprintf(stderr, "Error: Failed to read entire file '%s'. Expected %ld bytes, got %zu.\n", argv[1], fsize, bytes_read);
        free(source_code);
        return 1;
    }

    source_code[bytes_read] = '\0'; // Null-terminate based on the actual bytes read

    Lexer lexer = { source_code, 0, source_code[0], 1, 1, bytes_read }; // Use the correct length

    initial_file_abs_path = realpath(argv[1], NULL);
    if (!initial_file_abs_path) {
        fprintf(stderr, "Error: Could not resolve absolute path for input file '%s'\n", argv[1]);
        free(source_code); return 1;
    }

    // Initialize global scope
    Scope* global_scope = (Scope*)malloc(sizeof(Scope));
    if (!global_scope) {
        fprintf(stderr, "Failed to allocate memory for global scope\n");
        free(source_code);
        return 1;
    }
    global_scope->id = next_scope_id++;
    global_scope->symbols = NULL;
    global_scope->outer = NULL; // Global scope has no outer scope

    Interpreter interpreter = {
        .lexer = &lexer,
        .current_token = get_next_token(&lexer),
        .current_scope = global_scope,
        .loop_depth = 0,
        .break_flag = 0,
        .continue_flag = 0,
        .function_nesting_level = 0,
        .current_function_return_value = create_null_value(), // Initialize
        .return_flag = 0,  // Added missing comma here
        .current_exception = create_null_value(),
        .current_self_object = NULL,
        .exception_is_active = 0,
        .try_catch_stack_top = NULL,
        .module_cache = NULL, // Will be initialized by initialize_module_system
        .active_module_scopes_head = NULL, // Initialize new field
        .current_executing_file_path = strdup(initial_file_abs_path),
        .current_executing_file_directory = get_directory_from_path(initial_file_abs_path),
        .in_try_catch_finally_block_definition = 0, // Initialize to false
        .async_ready_queue_head = NULL,
        .async_ready_queue_tail = NULL,
        .async_sleep_queue_head = NULL, // Initialize new field
        .async_sleep_queue_tail = NULL, // Initialize new field
        .current_executing_coroutine = NULL, // Add missing comma here
        .async_event_loop_active = 0, // Add missing comma here
        .error_token = NULL, // Initialize new field
        .unhandled_error_occured = 0, // Initialize new flag
        .repr_depth_count = 0, // Initialize new field
        .prevent_side_effects = false, // Initialize new flag
        .resume_depth = 0, // Initialize async resume depth
    };
    interpreter.is_dummy_resume_value = false; // Initialize new flag
    free(initial_file_abs_path); // directory path was strdup'd
    g_interpreter_for_error_reporting = &interpreter;

    initialize_module_system(&interpreter);

    interpret(&interpreter);

    if (interpreter.unhandled_error_occured) {
        #ifdef DEBUG_ECHOC
        print_recent_logs_to_stderr_internal();
        #endif
        char* err_str = value_to_string_representation(interpreter.current_exception, &interpreter, interpreter.error_token);
        const char* file_path = interpreter.current_executing_file_path ? interpreter.current_executing_file_path : "unknown file";

        if (interpreter.error_token) {
            fprintf(stderr, "[EchoC Unhandled Exception] in %s at line %d, col %d: %s\n", file_path, interpreter.error_token->line, interpreter.error_token->col, err_str);
        } else {
            fprintf(stderr, "[EchoC Unhandled Exception] in %s (unknown location): %s\n", file_path, err_str);
        }
        free(err_str);
    }

    free_token(interpreter.current_token); // Free the last token (usually EOF)
    if (interpreter.current_executing_file_path) free(interpreter.current_executing_file_path);
    free_value_contents(interpreter.current_function_return_value); // Free any lingering return value
    free_value_contents(interpreter.current_exception); // Free any unhandled exception
    if (interpreter.error_token) free_token(interpreter.error_token); // Free error token if set
    free_scope(interpreter.current_scope); // Clean up the (global) scope

    // Free all defined blueprints
    BlueprintListNode* current_bp_node = interpreter.all_blueprints_head;
    BlueprintListNode* next_bp_node;
    while (current_bp_node) {
        next_bp_node = current_bp_node->next;
        Blueprint* bp_to_free = current_bp_node->blueprint;
        if (bp_to_free) {
            DEBUG_PRINTF("Main cleanup: Freeing Blueprint '%s' and its class scope.", bp_to_free->name);
            if (bp_to_free->name) free(bp_to_free->name);
            // class_attributes_and_methods scope contains symbols (let vars, functs).
            // free_scope will handle freeing those symbols and their values.
            if (bp_to_free->class_attributes_and_methods) {
                free_scope(bp_to_free->class_attributes_and_methods);
            }
            free(bp_to_free); // Free the Blueprint struct itself
        }
        free(current_bp_node); // Free the list node
        current_bp_node = next_bp_node;
    }
    
    // Loop to ensure all coroutines are processed, even if freeing one queue adds to another.
    while (interpreter.async_ready_queue_head || interpreter.async_sleep_queue_head) {
        if (interpreter.async_ready_queue_head) {
            robust_free_coroutine_queue(&interpreter, &interpreter.async_ready_queue_head, &interpreter.async_ready_queue_tail);
        }
        if (interpreter.async_sleep_queue_head) {
            robust_free_coroutine_queue(&interpreter, &interpreter.async_sleep_queue_head, &interpreter.async_sleep_queue_tail);
        }
    }

    cleanup_module_system(&interpreter); // Clean up module cache and related resources

    #ifdef DEBUG_ECHOC
    /*
    if (echoc_debug_log_file) {
        fprintf(echoc_debug_log_file, "--- EchoC Runtime Log Finalizing ---\n");
        fflush(echoc_debug_log_file);
        fclose(echoc_debug_log_file); // Close the file normally
        echoc_debug_log_file = NULL;
    }
    */
    #endif
    free(source_code);
    return interpreter.unhandled_error_occured ? 1 : 0;
}