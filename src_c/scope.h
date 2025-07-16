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

// Defines (or updates) a variable ONLY in the given scope.
// Does not search outer scopes. Used for 'let'.
void symbol_table_define(Scope* scope, const char* name, Value value);

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

// Prints the contents of a scope for debugging.
void print_scope_contents(Scope* scope);

// Frees all symbol nodes in a linked list
void free_symbol_nodes(SymbolNode* symbols);

#endif // ECHOC_SCOPE_H