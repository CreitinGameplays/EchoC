// src_c/value_utils.h
#ifndef ECHOC_VALUE_UTILS_H
#define ECHOC_VALUE_UTILS_H
#include "header.h" // Provides Value, Interpreter, Token, report_error, Dictionary

typedef struct {
    char* buffer;
    size_t length;
    size_t capacity;
} DynamicString;

void ds_init(DynamicString* ds, size_t initial_capacity);
void ds_ensure_capacity(DynamicString* ds, size_t additional_needed);
void ds_append_str(DynamicString* ds, const char* str);
char* ds_finalize(DynamicString* ds);
void ds_free(DynamicString* ds);
// --- End DynamicString Helper ---

// Converts a Value to its string representation.
// The caller is responsible for freeing the returned string.
char* value_to_string_representation(Value val, Interpreter* interpreter, Token* error_token_context);

// Evaluates a string literal that may contain %{variable} interpolations.
// Returns a new VAL_STRING Value. The caller is responsible for its contents.
Value evaluate_interpolated_string(Interpreter* interpreter, const char* raw_string, Token* string_token_for_errors);

void coroutine_decref_and_free_if_zero(Coroutine* coro);

// Increments coroutine ref_count.
void coroutine_incref(Coroutine* coro);

// Deep copies a Value. For container types (Array, Dict, etc.),
// new memory is allocated for the container and its elements are deep-copied.
// For reference-counted types like Coroutine, the ref_count is incremented.
// The caller is responsible for freeing the returned Value's contents.
Value value_deep_copy(Value val);

#endif // ECHOC_VALUE_UTILS_H