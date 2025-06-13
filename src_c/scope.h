// src_c/scope.h
#ifndef ECHOC_SCOPE_H
#define ECHOC_SCOPE_H

#include "header.h" // Provides Interpreter, Value, Scope, Token, report_error, free_scope

// Enters a new scope, making it the current scope.
void enter_scope(Interpreter* interpreter);

// Exits the current scope, restoring the outer scope. Frees the exited scope.
void exit_scope(Interpreter* interpreter);

// Sets (or updates) a variable in the current scope's symbol table.
// Makes a deep copy of the value.
void symbol_table_set(Scope* current_scope, const char* name, Value value);

// Gets a variable's value from the symbol table, searching current and outer scopes.
// Returns a pointer to the Value in the table (not a copy), or NULL if not found.
Value* symbol_table_get(Scope* current_scope, const char* name);

// Gets a variable's value from the specified scope only (not outer scopes).
// Returns a pointer to the Value in the table, or NULL if not found locally.
Value* symbol_table_get_local(Scope* scope, const char* name);

// Structure to hold both value pointer and its definition scope
typedef struct {
    Value* value_ptr;
    Scope* definition_scope;
} VarScopeInfo;

// Gets a variable's value and its definition scope.
VarScopeInfo get_variable_definition_scope_and_value(Scope* search_start_scope, const char* name);

#endif // ECHOC_SCOPE_H