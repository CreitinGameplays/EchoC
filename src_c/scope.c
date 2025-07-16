// src_c/scope.c
#include "scope.h"
#include "value_utils.h" // For value_to_string_representation
#include <string.h> // For strcmp, strdup
#include <stdlib.h> // For malloc, free
#include <stdbool.h> // For bool

void free_scope(Scope* scope) {
    if (!scope) return;

    // Free all symbol nodes in the scope
    SymbolNode* current = scope->symbols;
    while (current) {
        SymbolNode* next = current->next;
        
        bool is_self_object_reference = false;
        if (current->name) {
            is_self_object_reference = (current->value.type == VAL_OBJECT && strcmp(current->name, "self") == 0);
        }

        if (current->name) {
            free(current->name);
            current->name = NULL; // Good practice
        }
        
        if (!is_self_object_reference) {
            free_value_contents(current->value);
        }
        free(current);
        current = next;
    }
    free(scope); // Free the Scope struct itself
}


void enter_scope(Interpreter* interpreter) {
    Scope* new_scope = (Scope*)malloc(sizeof(Scope));
    if (!new_scope) {
        // This is a critical error - we can't continue without memory
        report_error("System", "Failed to allocate memory for new scope", interpreter->current_token);
    }
    new_scope->id = next_scope_id++;
    DEBUG_PRINTF("ENTER_SCOPE: Created [Scope #%llu] at %p, outer is [Scope #%llu]", new_scope->id, (void*)new_scope, interpreter->current_scope ? interpreter->current_scope->id : (uint64_t)-1);
    new_scope->symbols = NULL;
    new_scope->outer = interpreter->current_scope;
    interpreter->current_scope = new_scope;
}

void exit_scope(Interpreter* interpreter) {
    if (interpreter->current_scope == NULL) { // Should not happen if balanced
        // This is a programming error - log it but don't crash
        fprintf(stderr, "Warning: Attempted to exit non-existent scope\n");
        return;
    }
    if (interpreter->current_scope->outer == NULL && interpreter->current_scope->symbols != NULL) {
        report_error("System", "Attempted to exit non-existent scope", interpreter->current_token);
        return;
    }
    Scope* scope_to_free = interpreter->current_scope;
    interpreter->current_scope = scope_to_free->outer;
    free_scope(scope_to_free); // free_scope will handle freeing symbols and the scope struct
}

void symbol_table_set(Scope* current_scope, const char* name, Value value) {
    DEBUG_PRINTF("SYMBOL_TABLE_SET_ENTRY: Setting '%s' in [Scope #%llu] %p. Current head: %s (NodeAddr: %p)",
                 name, current_scope->id, (void*)current_scope,
                 current_scope->symbols ? current_scope->symbols->name : "NULL_HEAD",
                 (void*)current_scope->symbols);

    // Search from the current scope outwards.
    Scope* scope_to_search = current_scope;
    while (scope_to_search != NULL) {
        for (SymbolNode* node = scope_to_search->symbols; node != NULL; node = node->next) {
            if (strcmp(node->name, name) == 0) {
                // Found it. Update the value in its definition scope and return.
                DEBUG_PRINTF("  Updating existing variable '%s' in [Scope #%llu] %p", name, scope_to_search->id, (void*)scope_to_search);
                Value new_value_copy = value_deep_copy(value); // Copy the new value first to handle self-assignment (e.g. let: x = x + 1)
                free_value_contents(node->value);              // Then free the old value's contents
                node->value = new_value_copy;                  // Then assign the new copied value
                return;
            }
        }
        scope_to_search = scope_to_search->outer;
    }

    // If we get here, the variable was not found in any accessible scope.
    // Create a new one in the *current* scope.
    DEBUG_PRINTF("  Creating new variable '%s' in current [Scope #%llu] %p.", name, current_scope->id, (void*)current_scope);

    SymbolNode* newNode = (SymbolNode*)calloc(1, sizeof(SymbolNode));
    if (!newNode) {
        // Set exception flag instead of calling report_error directly
        // This allows callers to handle the error gracefully
        fprintf(stderr, "Failed to allocate memory for new symbol\n");
        exit(1); // Or set a global error flag
        report_error("System", "Failed to allocate memory for new symbol", NULL);
    }
    newNode->name = strdup(name);
    if (!newNode->name) {
        free(newNode);
        report_error("System", "Failed to allocate memory for symbol name in symbol_table_set", NULL);
    }
    // Use value_deep_copy which handles NULL values properly
    // and returns an appropriate error value if copying fails
    newNode->value = value_deep_copy(value);
    newNode->next = current_scope->symbols;
    current_scope->symbols = newNode;
    DEBUG_PRINTF("  SYMBOL_TABLE_SET_EXIT: After adding '%s', new head is: %s (NodeAddr: %p).",
                 name,
                 current_scope->symbols ? current_scope->symbols->name : "NULL_UNEXPECTED", 
                 (void*)current_scope->symbols);
}

void symbol_table_define(Scope* scope, const char* name, Value value) {
    if (!scope) {
        report_error("Internal", "Attempted to define variable in a NULL scope.", NULL);
        return;
    }

    // Check if the variable already exists in the *current* scope.
    for (SymbolNode* node = scope->symbols; node != NULL; node = node->next) {
        if (strcmp(node->name, name) == 0) {
            // Variable exists in the current scope, so update it.
            DEBUG_PRINTF("  Updating existing variable '%s' with 'let' in current scope %p", name, (void*)scope);
            free_value_contents(node->value);
            node->value = value_deep_copy(value);
            return;
        }
    }

    // Variable does not exist in the current scope, so create it.
    DEBUG_PRINTF("  Defining new variable '%s' with 'let' in current scope %p.", name, (void*)scope);
    SymbolNode* newNode = (SymbolNode*)calloc(1, sizeof(SymbolNode));
    if (!newNode) {
        report_error("System", "Failed to allocate memory for new symbol in symbol_table_define", NULL);
    }
    newNode->name = strdup(name);
    if (!newNode->name) {
        free(newNode);
        report_error("System", "Failed to allocate memory for symbol name in symbol_table_define", NULL);
    }
    newNode->value = value_deep_copy(value);
    newNode->next = scope->symbols;
    scope->symbols = newNode;
}

Value* symbol_table_get_recursive(Scope* current_scope, const char* name) {
    // Log the symbols head pointer at the very beginning of the function
    DEBUG_PRINTF("SYMBOL_TABLE_GET_RECURSIVE_START: Searching for '%s'. Input [Scope #%llu] %p. Symbols Head: %p (%s)", name, current_scope ? current_scope->id : (uint64_t)-1, (void*)current_scope, (void*)(current_scope ? current_scope->symbols : NULL),
                 (current_scope && current_scope->symbols) ? current_scope->symbols->name : "NULL_HEAD");
    DEBUG_PRINTF("SYMBOL_TABLE_GET_RECURSIVE_ENTRY: Searching for '%s' in [Scope #%llu] %p. Initial head: %s (NodeAddr: %p)", name, current_scope ? current_scope->id : (uint64_t)-1, (void*)current_scope,
                 current_scope->symbols ? current_scope->symbols->name : "NULL_HEAD",
                 (void*)current_scope->symbols);
    Scope* temp_s_log = current_scope;
    int depth_log = 0;
    while(temp_s_log && depth_log < 5) { // Limit depth for logging
        DEBUG_PRINTF("  [Scope #%llu] %p (depth %d) symbols for '%s' lookup:", temp_s_log->id, (void*)temp_s_log, depth_log, name);
        SymbolNode* sym_iter_log = temp_s_log->symbols;
        int sym_count_log = 0;
        while(sym_iter_log && sym_count_log < 15) { // Limit symbols per scope
            DEBUG_PRINTF("    -> '%s' (NodeAddr: %p, Type: %d)", sym_iter_log->name, (void*)sym_iter_log, sym_iter_log->value.type);
            sym_iter_log = sym_iter_log->next;
            sym_count_log++;
        }
        if (sym_iter_log) DEBUG_PRINTF("    -> ... (more symbols in this scope)%s","");
        temp_s_log = temp_s_log->outer;
        depth_log++;
    }
    if (temp_s_log) DEBUG_PRINTF("  ... (more outer scopes for '%s' lookup)%s", name, "");

    DEBUG_PRINTF("SYMBOL_TABLE_GET: Attempting to find variable '%s'", name);
    Scope* scope_to_search = current_scope;
    while (scope_to_search != NULL) {
        DEBUG_PRINTF("  Searching [Scope #%llu] %p (outer: %p)", scope_to_search->id, (void*)scope_to_search, (void*)scope_to_search->outer);
        SymbolNode* current_symbol = scope_to_search->symbols;
        while (current_symbol != NULL) {
            DEBUG_PRINTF("    Checking against symbol '%s' in [Scope #%llu]", current_symbol->name, scope_to_search->id);
            // --- START: ADD THIS DEBUG BLOCK ---
            //if (strcmp(current_symbol->name, name) == 0 && strcmp(name, "extra_resources") == 0) {
            //    printf("\n>>> SCOPE_GET_DEBUG: Accessing variable 'extra_resources' in scope %p\n", (void*)scope_to_search);
            //    char* dbg_str = value_to_string_representation(current_symbol->value, NULL, NULL);
            //    printf(">>> SCOPE_GET_DEBUG: Value Type: %d, Content: %s\n\n", current_symbol->value.type, dbg_str);
            //    fflush(stdout);
            //    free(dbg_str);
            //}
            // --- END: ADD THIS DEBUG BLOCK ---

            if (strcmp(current_symbol->name, name) == 0) {
                DEBUG_PRINTF("    FOUND variable '%s' in [Scope #%llu] %p.", name, scope_to_search->id, (void*)scope_to_search);
                if (current_symbol->value.type == VAL_INT) {
                    DEBUG_PRINTF("      Type: VAL_INT, Value: %ld", current_symbol->value.as.integer);
                } else if (current_symbol->value.type == VAL_FLOAT) {
                    DEBUG_PRINTF("      Type: VAL_FLOAT, Value: %f", current_symbol->value.as.floating);
                } else {
                    DEBUG_PRINTF("      Type: %d (Non-numeric)", current_symbol->value.type);
                }
                return &(current_symbol->value);
            }
            current_symbol = current_symbol->next;
        }
        scope_to_search = scope_to_search->outer; // Move to outer scope
    }
    DEBUG_PRINTF("  Variable '%s' NOT FOUND in any accessible scope starting from [Scope #%llu] %p.", name, current_scope ? current_scope->id : (uint64_t)-1, (void*)current_scope);
    return NULL; // Not found in any accessible scope
}

Value* symbol_table_get(Scope* current_scope, const char* name) {
    // Standard recursive lookup
    return symbol_table_get_recursive(current_scope, name);
}

// Get from a specific scope only, not its outer scopes.
Value* symbol_table_get_local(Scope* scope, const char* name) {
    if (!scope) return NULL;
    DEBUG_PRINTF("SYMBOL_TABLE_GET_LOCAL: Attempting to find variable '%s' in scope %p", name, (void*)scope);
    SymbolNode* current_symbol = scope->symbols;
    while (current_symbol != NULL) {
        DEBUG_PRINTF("    Checking against symbol '%s'", current_symbol->name);
        if (strcmp(current_symbol->name, name) == 0) {
            DEBUG_PRINTF("    FOUND variable '%s' locally.", name);
             if (current_symbol->value.type == VAL_INT) DEBUG_PRINTF("      Type: VAL_INT, Value: %ld", current_symbol->value.as.integer);
             else if (current_symbol->value.type == VAL_FLOAT) DEBUG_PRINTF("      Type: VAL_FLOAT, Value: %f", current_symbol->value.as.floating);
             else DEBUG_PRINTF("      Type: %d (Non-numeric/complex)", current_symbol->value.type);
            return &(current_symbol->value);
        }
        current_symbol = current_symbol->next;
    }
    DEBUG_PRINTF("  Variable '%s' NOT FOUND locally in scope %p.", name, (void*)scope);
    return NULL;
}

VarScopeInfo get_variable_definition_scope_and_value(Scope* search_start_scope, const char* name) {
    VarScopeInfo info = {NULL, NULL};
    Scope* scope_to_search = search_start_scope;
    while (scope_to_search != NULL) {
        SymbolNode* current_symbol = scope_to_search->symbols;
        while (current_symbol != NULL) {
            if (strcmp(current_symbol->name, name) == 0) {
                info.value_ptr = &(current_symbol->value);
                info.definition_scope = scope_to_search; // This is the scope where the symbol node resides
                return info;
            }
            current_symbol = current_symbol->next;
        }
        scope_to_search = scope_to_search->outer;
    }
    return info; // Not found
}

void print_scope_contents(Scope* scope) {
    if (!scope) {
        DEBUG_PRINTF("Scope is NULL.%s", ""); // Added empty string argument
        return;
    }
    DEBUG_PRINTF("Scope contents (Scope Addr: %p):", (void*)scope);
    for (SymbolNode* current = scope->symbols; current != NULL; current = current->next) {
        DEBUG_PRINTF("  - Symbol: '%s' (Type: %d, Addr: %p, Next: %p)", current->name, current->value.type, (void*)current, (void*)current->next);
    }
}