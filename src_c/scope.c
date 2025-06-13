// src_c/scope.c
#include "scope.h"
#include <string.h> // For strcmp, strdup
#include <stdlib.h> // For malloc, free

void enter_scope(Interpreter* interpreter) {
    Scope* new_scope = (Scope*)malloc(sizeof(Scope));
    if (!new_scope) {
        report_error("System", "Failed to allocate memory for new scope", interpreter->current_token);
    }
    new_scope->symbols = NULL;
    new_scope->outer = interpreter->current_scope;
    interpreter->current_scope = new_scope;
}

void exit_scope(Interpreter* interpreter) {
    if (interpreter->current_scope == NULL) { // Should not happen if balanced
        report_error("System", "Attempted to exit non-existent scope", interpreter->current_token);
        return;
    }
    Scope* scope_to_free = interpreter->current_scope;
    interpreter->current_scope = scope_to_free->outer;
    free_scope(scope_to_free); // free_scope will handle freeing symbols and the scope struct
}

void symbol_table_set(Scope* current_scope, const char* name, Value value) {
    DEBUG_PRINTF("SYMBOL_TABLE_SET: Attempting to set variable '%s' in current scope %p", name, (void*)current_scope);
    if (value.type == VAL_INT) {
        DEBUG_PRINTF("  Value to set: Type: VAL_INT, Value: %ld", value.as.integer);
    } else if (value.type == VAL_FLOAT) {
        DEBUG_PRINTF("  Value to set: Type: VAL_FLOAT, Value: %f", value.as.floating);
    }

        // Check if variable already exists in the *current* scope to update it.
    // This ensures 'let' re-declaration in the same scope updates the existing variable in that scope.
    SymbolNode* current_symbol_node = current_scope->symbols;
    while (current_symbol_node != NULL) {
        if (strcmp(current_symbol_node->name, name) == 0) {
            DEBUG_PRINTF("  Updating existing variable '%s' in current scope %p", name, (void*)current_scope);
            free_value_contents(current_symbol_node->value); // Free existing value contents
            current_symbol_node->value = value_deep_copy(value); // Deep copy the new value
            return;
        }
        current_symbol_node = current_symbol_node->next;
    }

    // Variable does not exist in the current scope, create a new one.
    DEBUG_PRINTF("  Creating new variable '%s' in current scope %p", name, (void*)current_scope);
    SymbolNode* newNode = (SymbolNode*)malloc(sizeof(SymbolNode));
    if (!newNode) {
        report_error("System", "Failed to allocate memory for new symbol", NULL); // Token might not be relevant here
    }
    newNode->name = strdup(name);
    if (!newNode->name) {
        free(newNode);
        report_error("System", "Failed to allocate memory for symbol name", NULL);
    }
    newNode->value = value_deep_copy(value); // Deep copy the value for storage
    newNode->next = current_scope->symbols; // Add to the front of the current scope's list
    current_scope->symbols = newNode;
}

Value* symbol_table_get_recursive(Scope* current_scope, const char* name) {
    DEBUG_PRINTF("SYMBOL_TABLE_GET: Attempting to find variable '%s'", name);
    Scope* scope_to_search = current_scope;
    while (scope_to_search != NULL) {
        DEBUG_PRINTF("  Searching scope %p (outer: %p)", (void*)scope_to_search, (void*)scope_to_search->outer);
        SymbolNode* current_symbol = scope_to_search->symbols;
        while (current_symbol != NULL) {
            DEBUG_PRINTF("    Checking against symbol '%s' in scope %p", current_symbol->name, (void*)scope_to_search);
            if (strcmp(current_symbol->name, name) == 0) {
                DEBUG_PRINTF("    FOUND variable '%s' in scope %p.", name, (void*)scope_to_search);
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
    DEBUG_PRINTF("  Variable '%s' NOT FOUND in any accessible scope starting from %p.", name, (void*)current_scope);
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