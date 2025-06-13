// src_c/value_utils.h
#ifndef ECHOC_VALUE_UTILS_H
#define ECHOC_VALUE_UTILS_H
#include "header.h" // Provides Value, Interpreter, Token, report_error, Dictionary

// Converts a Value to its string representation.
// The caller is responsible for freeing the returned string.
char* value_to_string_representation(Value val, Interpreter* interpreter, Token* error_token_context);

// Evaluates a string literal that may contain %{variable} interpolations.
// Returns a new VAL_STRING Value. The caller is responsible for its contents.
Value evaluate_interpolated_string(Interpreter* interpreter, const char* raw_string, Token* string_token_for_errors);

#endif // ECHOC_VALUE_UTILS_H