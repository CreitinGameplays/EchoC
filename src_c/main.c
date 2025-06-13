// src_c/main.c
#include "header.h"
#include "module_loader.h" // For initialize_module_system, cleanup_module_system

#include "scope.h"         // For symbol_table_set, free_scope

#ifdef DEBUG_ECHOC
FILE* echoc_debug_log_file = NULL; // Define global log file pointer
#endif

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
        // source_text_owned_copy is strdup'd by value_deep_copy.
        // The Function struct created by interpret_funct_statement does not own this string after it's copied.
        if (func->source_text_owned_copy && func->is_source_owner) { // Check ownership flag
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
        } else if (val.type == VAL_OBJECT && val.as.object_val != NULL) { // Corrected: free copied objects
        // If value_deep_copy for VAL_OBJECT only copies the pointer,
        // free_value_contents should not free the object itself here if it's not an owned copy.
        // However, if this Value is a fresh copy (e.g. from value_deep_copy of an object), it should be freed.
        DEBUG_PRINTF("Freeing object: %p", (void*)val.as.object_val);
        Object* obj_to_free = val.as.object_val;
        if (obj_to_free->instance_attributes) {
            free_scope(obj_to_free->instance_attributes); // Frees symbols and scope struct
        }
        free(obj_to_free); // Free the Object struct itself
    } else if (val.type == VAL_BOUND_METHOD && val.as.bound_method_val != NULL) {
        BoundMethod* bm = val.as.bound_method_val;
        // If bm->self_is_owned_copy is true, the BoundMethod owns the self_value.
        if (bm->self_is_owned_copy) {
            free_value_contents(bm->self_value); // Free the contents of the owned self_value
        }
        // The Function* or CBuiltinFunction pointer itself is not freed here,
        // as EchoC functions are owned by their definition scope, and C builtins are static.
        if (bm->type == FUNC_TYPE_ECHOC && bm->func_ptr.echoc_function == NULL) {
        }
        // bm->self_value is handled by self_is_owned_copy logic.
        free(bm);
    } else if ((val.type == VAL_COROUTINE || val.type == VAL_GATHER_TASK) && val.as.coroutine_val != NULL) {
        Coroutine* coro = val.as.coroutine_val;
        coro->ref_count--;
        DEBUG_PRINTF("FREE_VALUE_CONTENTS: Decremented ref_count for coro %s (%p) to %d", coro->name ? coro->name : "unnamed", (void*)coro, coro->ref_count);
        if (coro->ref_count == 0) {
            DEBUG_PRINTF("FREE_VALUE_CONTENTS: Freeing actual Coroutine struct %s (%p)", coro->name ? coro->name : "unnamed", (void*)coro);
            if (coro->name) free(coro->name);
            // The Function* (coro->function_def) is not freed here, it's owned by its definition scope.
            if (coro->execution_scope) {
                free_scope(coro->execution_scope); // Frees symbols and scope struct
            }
            free_value_contents(coro->result_value);
            free_value_contents(coro->exception_value);

            if (coro->gather_tasks) { // This is an Array of VAL_COROUTINE
                for (int i = 0; i < coro->gather_tasks->count; ++i) {
                    // VAL_COROUTINEs in gather_tasks had their ref_counts incremented when copied.
                    // free_value_contents here will decrement them.
                    free_value_contents(coro->gather_tasks->elements[i]);
                }
                free(coro->gather_tasks->elements);
                free(coro->gather_tasks);
            }
            if (coro->gather_results) { // This is an Array of Value
                for (int i = 0; i < coro->gather_results->count; ++i) {
                    free_value_contents(coro->gather_results->elements[i]);
                }
                free(coro->gather_results->elements);
                free(coro->gather_results);
            }
            free_value_contents(coro->value_from_await); // Free any lingering value from await
            
            // Free the waiters list
            CoroutineWaiterNode* current_waiter = coro->waiters_head;
            CoroutineWaiterNode* next_waiter;
            while (current_waiter) {
                next_waiter = current_waiter->next;
                // The waiter_coro itself is not freed here, its lifecycle is managed by its own ref_count.
                free(current_waiter);
                current_waiter = next_waiter;
            }
            coro->waiters_head = NULL;

            free(coro);
        }
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
        Dictionary* original_dict = original.as.dict_val;
        Dictionary* new_dict = malloc(sizeof(Dictionary));
        if (!new_dict) report_error("System", "Failed to allocate memory for dictionary copy", NULL);
        
        new_dict->num_buckets = original_dict->num_buckets;
        new_dict->count = original_dict->count;
        new_dict->buckets = calloc(new_dict->num_buckets, sizeof(DictEntry*));
        if (!new_dict->buckets) { free(new_dict); report_error("System", "Failed to allocate memory for copied dictionary buckets", NULL); }

        for (int i = 0; i < original_dict->num_buckets; ++i) {
            DictEntry* original_entry = original_dict->buckets[i];
            DictEntry* current_new_chain_tail = NULL;
            while (original_entry) {
                DictEntry* new_entry = malloc(sizeof(DictEntry));
                if (!new_entry) report_error("System", "Failed to allocate memory for copied dictionary entry", NULL);
                new_entry->key = strdup(original_entry->key);
                if (!new_entry->key) { free(new_entry); report_error("System", "Failed to strdup key for copied dictionary entry", NULL); }
                new_entry->value = value_deep_copy(original_entry->value);
                new_entry->next = NULL;
                if (current_new_chain_tail == NULL) {
                    new_dict->buckets[i] = new_entry;
                } else {
                    current_new_chain_tail->next = new_entry;
                }
                current_new_chain_tail = new_entry;
                original_entry = original_entry->next;
            }
        }
        copy.as.dict_val = new_dict;
    } else if (original.type == VAL_FUNCTION && original.as.function_val != NULL) {
        // Functions are typically "copied" by reference to their definition.
        // Here, we create a new Function struct but it points to the same underlying
        // definition details (body location, definition scope).
        // The name and parameters are duplicated as they are part of the Function struct.
        Function* original_func = original.as.function_val;
        Function* new_func = malloc(sizeof(Function));
        if (!new_func) report_error("System", "Failed to allocate memory for function copy", NULL);

        new_func->name = strdup(original_func->name);
        if (!new_func->name) { free(new_func); report_error("System", "Failed to strdup function name in copy", NULL); }
        new_func->param_count = original_func->param_count;
        new_func->params = malloc(new_func->param_count * sizeof(Parameter));
        if (!new_func->params && new_func->param_count > 0) { free(new_func->name); free(new_func); report_error("System", "Failed to alloc params for func copy", NULL); }
        for (int i = 0; i < new_func->param_count; ++i) {
            new_func->params[i].name = strdup(original_func->params[i].name);
            if (!new_func->params[i].name) { /* cleanup */ report_error("System", "Failed to strdup param name", NULL); }
            if (original_func->params[i].default_value) {
                new_func->params[i].default_value = malloc(sizeof(Value));
                *(new_func->params[i].default_value) = value_deep_copy(*(original_func->params[i].default_value));
            } else { new_func->params[i].default_value = NULL; }
        }
        new_func->body_start_state = original_func->body_start_state; // Copy struct
        new_func->body_end_token_original_line = original_func->body_end_token_original_line;
        new_func->body_end_token_original_col = original_func->body_end_token_original_col;
        new_func->definition_scope = original_func->definition_scope; // Share the definition scope
        // The original_func (from symbol table or another Value) already owns its source_text_owned_copy.
        // The original_func (from symbol table or another Value) already owns its source_text_owned_copy.
        // The new_func (the copy being made) must also get its own owned copy.
        // The new_func (the copy being made) will own its duplicated text.
        // The original_func->source_text_owned_copy points to the text buffer (e.g. from main lexer or another Function's owned copy).
        if (original_func->source_text_owned_copy) {
            new_func->source_text_owned_copy = strdup(original_func->source_text_owned_copy);
            if (!new_func->source_text_owned_copy) { /* error, cleanup */ report_error("System", "Failed to strdup function source text in copy", NULL); }
        } else {
            // This case implies original_func had no source text, which might be an issue elsewhere or intentional for some builtins (though unlikely for EchoC funcs)
            new_func->source_text_owned_copy = NULL;
        }
        new_func->source_text_length = original_func->source_text_length; // Copy length
        // CRITICAL: Update the copied body_start_state to use the new_func's owned text
        if (new_func->source_text_owned_copy) {
            new_func->body_start_state.text = new_func->source_text_owned_copy;
        }
        new_func->is_async = original_func->is_async; // <-- FIX: Copy the is_async flag
        new_func->is_source_owner = (new_func->source_text_owned_copy != NULL); // The copy owns its strdup'd text
        copy.as.function_val = new_func;
    } else if (original.type == VAL_BLUEPRINT && original.as.blueprint_val != NULL) {
        // VAL_BLUEPRINT values are pointers to the canonical Blueprint definition.
        // "Deep copy" of a VAL_BLUEPRINT just copies the pointer.
        // The actual Blueprint struct is managed by its defining symbol.
        copy.as.blueprint_val = original.as.blueprint_val;
    } else if (original.type == VAL_OBJECT && original.as.object_val != NULL) {        
        // Perform a true deep copy of the Object and its instance_attributes scope.
        Object* original_obj = original.as.object_val;
        Object* new_obj = malloc(sizeof(Object));
        if (!new_obj) report_error("System", "Failed to allocate memory for object copy", NULL);

        new_obj->blueprint = original_obj->blueprint; // Share blueprint definition
        
        // Deep copy the instance_attributes scope
        new_obj->instance_attributes = malloc(sizeof(Scope));
        if (!new_obj->instance_attributes) { free(new_obj); report_error("System", "Failed to alloc scope for object copy", NULL); }
        new_obj->instance_attributes->symbols = NULL;
        new_obj->instance_attributes->outer = NULL; // Instance scopes are isolated

        SymbolNode* current_original_symbol = original_obj->instance_attributes->symbols;
        while (current_original_symbol) {
            // symbol_table_set on new_obj->instance_attributes will deep_copy the value
            symbol_table_set(new_obj->instance_attributes, current_original_symbol->name, current_original_symbol->value);
            current_original_symbol = current_original_symbol->next;
        }
        copy.as.object_val = new_obj;
    } else if (original.type == VAL_BOUND_METHOD && original.as.bound_method_val != NULL) {
        // BoundMethods are also somewhat like references.
        BoundMethod* original_bm = original.as.bound_method_val;
        BoundMethod* new_bm = malloc(sizeof(BoundMethod));
        if (!new_bm) report_error("System", "Failed to allocate memory for bound_method copy", NULL);
        new_bm->type = original_bm->type;
        if (original_bm->type == FUNC_TYPE_ECHOC) {
            new_bm->func_ptr.echoc_function = original_bm->func_ptr.echoc_function; // Share EchoC Function definition pointer
        } else { // FUNC_TYPE_C_BUILTIN
            new_bm->func_ptr.c_builtin = original_bm->func_ptr.c_builtin; // Share C function pointer
        }

        // If the original bound method owned its self_value, the new one should get a deep copy.
        // Otherwise, it shares the (borrowed) self_value.
        if (original_bm->self_is_owned_copy) {
            new_bm->self_value = value_deep_copy(original_bm->self_value);
        } else {
            new_bm->self_value = original_bm->self_value; // Shallow copy of Value struct (shares underlying data)
        }
        new_bm->self_is_owned_copy = original_bm->self_is_owned_copy; // Copy ownership flag
        copy.as.bound_method_val = new_bm;
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

// Helper to free a coroutine queue
void free_coroutine_queue(CoroutineQueueNode* head) {
    CoroutineQueueNode* current = head;
    CoroutineQueueNode* next;
    while (current) {
        next = current->next;
        // free_value_contents should be called on the VAL_COROUTINE
        // before or after removing from queue, if the coroutine itself is being destroyed.
        // Here, we just free the queue nodes. The coroutines themselves are managed elsewhere.
        free(current);
        current = next;
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
            if (!is_self_object_reference) {
                free_value_contents(current->value);
            }
        }
        free(current);
        current = next;
    }
}

// Actual implementation for free_scope (forward declared in header.c)
void free_scope(Scope* scope) {
    if (scope) {
        free_symbol_nodes(scope->symbols); // Free all symbols in this scope
        free(scope);                       // Free the scope struct itself
    }
}


int main(int argc, char* argv[]) {
    #ifdef DEBUG_ECHOC
    /*
    echoc_debug_log_file = fopen("echoc_runtime_log.txt", "w");
    if (echoc_debug_log_file == NULL) {
        // If file opening fails, BUG_PRINTF/DEBUG_PRINTF will fall back to stderr.
        fprintf(stderr, "CRITICAL: Failed to open echoc_runtime_log.txt for writing. Runtime debug logs will go to stderr.\n");
    } else {
        fprintf(echoc_debug_log_file, "--- EchoC Runtime Log Initialized ---\n");
        setvbuf(echoc_debug_log_file, NULL, _IOLBF, 0); // Line buffering
        fflush(echoc_debug_log_file);
    }
    */
    // By keeping echoc_debug_log_file as NULL, no file logging will occur.
    // keep commented out so your CPU won't overload
    #endif

    if (argc != 2) {
        printf("Usage: %s <filename.echoc>\\n", argv[0]);
        return 1;
    }

    FILE* file = fopen(argv[1], "r");
    if (file == NULL) {
        printf("Error: Could not open file '%s'\\n", argv[1]);
        return 1;
    }

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* source_code = malloc(fsize + 1);
    fread(source_code, 1, fsize, file);
    fclose(file);
    source_code[fsize] = 0;

    Lexer lexer = { source_code, 0, source_code[0], 1, 1, (size_t)fsize };

    char* initial_file_abs_path = realpath(argv[1], NULL);
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
        .current_executing_file_directory = get_directory_from_path(initial_file_abs_path),
        .in_try_catch_finally_block_definition = 0, // Initialize to false
        .async_ready_queue_head = NULL,
        .async_ready_queue_tail = NULL,
        .current_executing_coroutine = NULL,
        .async_event_loop_active = 0,
        .coroutine_yielded_for_await = 0
    };
    free(initial_file_abs_path); // directory path was strdup'd

    initialize_module_system(&interpreter);

    interpret(&interpreter);

    free_token(interpreter.current_token); // Free the last token (usually EOF)
    free_value_contents(interpreter.current_function_return_value); // Free any lingering return value
    free_value_contents(interpreter.current_exception); // Free any unhandled exception
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

    // Free any remaining coroutines in the ready queue
    // (This assumes coroutines themselves are freed via free_value_contents when their VAL_COROUTINE Value is freed)
    free_coroutine_queue(interpreter.async_ready_queue_head);
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
    return 0;
}