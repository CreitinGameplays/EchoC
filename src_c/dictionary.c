// src_c/dictionary.c
#include "dictionary.h"
#include <string.h> // For strcmp, strdup, strcpy
#include <stdlib.h> // For malloc, free, calloc
#include <stdio.h>  // For snprintf

// Simple hash function for strings (djb2)
unsigned long hash_string(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash;
}

static void dictionary_resize(Dictionary* dict, Token* error_token); // Forward declaration

Dictionary* dictionary_create(int initial_buckets, Token* error_token) {
    Dictionary* dict = malloc(sizeof(Dictionary));
    if (!dict) report_error("System", "Failed to allocate memory for dictionary", error_token);
    dict->id = next_dictionary_id++;
    dict->num_buckets = initial_buckets > 0 ? initial_buckets : 16; // Default to 16 buckets
    dict->count = 0;
    dict->buckets = calloc(dict->num_buckets, sizeof(DictEntry*)); // Initialize all bucket pointers to NULL
    if (!dict->buckets) { free(dict); report_error("System", "Failed to allocate memory for dictionary buckets", error_token); }
    DEBUG_PRINTF("DICTIONARY_CREATE: Created [Dict #%llu] at %p", dict->id, (void*)dict);
    return dict;
}

// Helper to create a dictionary entry (used internally for optimized copying and set)
DictEntry* dictionary_create_entry(const char* key, Value value, Token* error_token) {
    DictEntry* new_entry = malloc(sizeof(DictEntry));
    if (!new_entry) {
        report_error("System", "Failed to allocate memory for dictionary entry", error_token);
    }
    new_entry->key = strdup(key);
    if (!new_entry->key) {
        free(new_entry);
        report_error("System", "Failed to allocate memory for dictionary key", error_token);
    }
    new_entry->value = value_deep_copy(value); // Deep copy the value
    new_entry->next = NULL;

    return new_entry;
}

void dictionary_set(Dictionary* dict, const char* key_str, Value value, Token* error_token) {
    unsigned long hash = hash_string(key_str);
    int index = hash % dict->num_buckets;

    DictEntry* current_entry = dict->buckets[index];
    DictEntry* prev_entry = NULL;

    // Check if key already exists in the chain
    while (current_entry != NULL) {
        if (strcmp(current_entry->key, key_str) == 0) {
            free_value_contents(current_entry->value); // Free old value
            current_entry->value = value_deep_copy(value); // Set new value (deep copy)
            return; // Key updated
        }
        prev_entry = current_entry;
        current_entry = current_entry->next;
    }

    // Key does not exist, create new entry
    DictEntry* new_entry = dictionary_create_entry(key_str, value, error_token);

    if (prev_entry == NULL) { // Bucket was empty
        dict->buckets[index] = new_entry;
    } else { // Add to end of chain in this bucket
        prev_entry->next = new_entry;
    }
    dict->count++;

    // Check load factor and resize if necessary
    if ((double)dict->count / dict->num_buckets > 0.75) {
        dictionary_resize(dict, error_token);
    }
}

Value dictionary_get(Dictionary* dict, const char* key_str, Token* error_token) {
    unsigned long hash = hash_string(key_str);
    int index = hash % dict->num_buckets;

    DictEntry* current_entry = dict->buckets[index];
    while (current_entry != NULL) {
        if (strcmp(current_entry->key, key_str) == 0) {
            return value_deep_copy(current_entry->value); // Return a deep copy
        }
        current_entry = current_entry->next;
    }

    char err_msg[300];
    snprintf(err_msg, sizeof(err_msg), "Key '%s' not found in dictionary.", key_str);
    report_error("Runtime", err_msg, error_token);
    // Should not be reached due to report_error exiting
    Value not_found_val; not_found_val.type = VAL_BOOL; not_found_val.as.bool_val = 0; /* Placeholder */ return not_found_val;
}

// Helper to rehash a dictionary into a new set of buckets
static void dictionary_resize(Dictionary* dict, Token* error_token) {
    int old_num_buckets = dict->num_buckets;
    DictEntry** old_buckets = dict->buckets;

    dict->num_buckets *= 2; // Double the number of buckets
    dict->buckets = calloc(dict->num_buckets, sizeof(DictEntry*));
    if (!dict->buckets) {
        // Attempt to restore old state if new allocation fails (though program might be unstable)
        dict->num_buckets = old_num_buckets;
        dict->buckets = old_buckets; // This is risky as old_buckets will be freed if we don't exit
        report_error("System", "Failed to allocate memory for resized dictionary buckets", error_token);
    }
    dict->count = 0; // Reset count, will be incremented as items are re-inserted

    for (int i = 0; i < old_num_buckets; ++i) {
        DictEntry* entry = old_buckets[i];
        while (entry) {
            DictEntry* next_entry = entry->next;
            // Re-insert entry into the new buckets. dictionary_set will make a deep copy
            // of entry->value.
            dictionary_set(dict, entry->key, entry->value, error_token); 
            free(entry->key); // Free old key
            // The value within 'entry' was copied by dictionary_set.
            // If dictionary_set reuses the value object, this free_value_contents might be an issue.
            // However, dictionary_set does value_deep_copy, so the original entry->value is safe to free.
            free_value_contents(entry->value); // Free old value contents
            free(entry); // Free old entry struct
            entry = next_entry;
        }
    }
    free(old_buckets); // Free the old array of bucket pointers
}

bool dictionary_try_get(Dictionary* dict, const char* key_str, Value* out_val, bool create_deep_copy_of_value_contents) {
    if (!dict || !key_str || !out_val) return false; // Basic safety
    unsigned long hash = hash_string(key_str);
    int index = hash % dict->num_buckets;

    DictEntry* current_entry = dict->buckets[index];
    while (current_entry != NULL) {
        if (strcmp(current_entry->key, key_str) == 0) {
            if (create_deep_copy_of_value_contents) {
                *out_val = value_deep_copy(current_entry->value); // Populate with a deep copy
            } else {
                *out_val = current_entry->value; // Shallow copy of Value struct, shares internal pointers for complex types
            }
            return true; // Found
        }
        current_entry = current_entry->next;
    }
    return false; // Not found
}

Value* dictionary_try_get_value_ptr(Dictionary* dict, const char* key_str) {
    if (!dict || !key_str) return NULL;
    unsigned long hash = hash_string(key_str);
    int index = hash % dict->num_buckets;

    DictEntry* current_entry = dict->buckets[index];
    while (current_entry != NULL) {
        if (strcmp(current_entry->key, key_str) == 0) {
            return &(current_entry->value);
        }
        current_entry = current_entry->next;
    }
    return NULL; // Not found
}

void dictionary_free(Dictionary* dict, int free_keys, int free_values_contents) {
    if (!dict) return;
    for (int i = 0; i < dict->num_buckets; ++i) {
        DictEntry* entry = dict->buckets[i];
        while (entry) {
            DictEntry* next_entry = entry->next;
            if (free_keys && entry->key) {
                free(entry->key);
            }
            if (free_values_contents) {
                free_value_contents(entry->value);
            }
            free(entry);
            entry = next_entry;
        }
    }
    free(dict->buckets);
    free(dict);
}