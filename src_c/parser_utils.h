// src_c/parser_utils.h
#ifndef ECHOC_PARSER_UTILS_H
#define ECHOC_PARSER_UTILS_H

#include "header.h" // Provides Interpreter, Token, TokenType, report_error

// Consumes the current token if it matches the expected type, otherwise reports an error.
void interpreter_eat(Interpreter* interpreter, TokenType expected_type);

// Returns a string representation for a given TokenType.
const char* token_type_to_string(TokenType type);

// Reports a syntax error for an unexpected token.
void report_error_unexpected_token(Interpreter* interpreter, const char* expected_description);

#endif // ECHOC_PARSER_UTILS_H