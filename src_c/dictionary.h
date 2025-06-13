// src_c/dictionary.h
#ifndef ECHOC_DICTIONARY_H
#define ECHOC_DICTIONARY_H

#include "header.h" // Provides Value, Dictionary, Token, report_error, value_deep_copy, free_value_contents

// Creates a new dictionary.
Dictionary* dictionary_create(int initial_buckets, Token* error_token);

// Sets a key-value pair in the dictionary. Handles new keys and updates to existing keys.
// Makes a deep copy of the value.
void dictionary_set(Dictionary* dict, const char* key_str, Value value, Token* error_token);

// Gets a value from the dictionary by key. Reports an error if the key is not found.
// Returns a deep copy of the value.
Value dictionary_get(Dictionary* dict, const char* key_str, Token* error_token);

// Attempts to get a value from the dictionary.
// Returns true if found, false otherwise.
// If create_deep_copy_of_value_contents is true, out_val is a deep copy.
// If false, out_val is a shallow copy of the Value struct (internal pointers for complex types are shared).
bool dictionary_try_get(Dictionary* dict, const char* key_str, Value* out_val, bool create_deep_copy_of_value_contents);

// Frees the dictionary, its entries, and optionally the keys and values if specified.
void dictionary_free(Dictionary* dict, int free_keys, int free_values_contents);

#endif // ECHOC_DICTIONARY_H