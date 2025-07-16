// src_c/statement_parser.h
#ifndef ECHOC_STATEMENT_PARSER_H
#define ECHOC_STATEMENT_PARSER_H

#include "header.h" // Provides Interpreter, Value, Token

// Parses and executes a single statement.
StatementExecStatus interpret_statement(Interpreter* interpreter);

// Performs indexed assignment (e.g., array[index] = value or dict[key] = value).
void perform_indexed_assignment(Value* target_container, Value final_index, Value value_to_set, Token* error_token, const char* base_var_name);

// void interpret_load_statement(Interpreter* interpreter); // Will be static in statement_parser.c
// These would typically be static in statement_parser.c, but declared here if needed by other modules (unlikely for these)
// static void interpret_raise_statement(Interpreter* interpreter);
// static void interpret_try_statement(Interpreter* interpreter);

#endif // ECHOC_STATEMENT_PARSER_H