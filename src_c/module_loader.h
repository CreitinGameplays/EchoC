// src_c/module_loader.h
#ifndef ECHOC_MODULE_LOADER_H
#define ECHOC_MODULE_LOADER_H

#include "header.h"

// Initializes the module cache in the interpreter.
void initialize_module_system(Interpreter* interpreter);

// Cleans up the module cache.
void cleanup_module_system(Interpreter* interpreter);

// Resolves a module name/path to an absolute path.
// Considers relative paths, standard library, and ECHOC_PATH.
// Caller must free the returned string if not NULL.
char* resolve_module_path(Interpreter* interpreter, const char* module_name_or_path, Token* error_token);

// Loads a module by its absolute path.
// If already in cache, returns the cached module namespace (a VAL_DICT).
// Otherwise, executes the module, caches it, and returns its namespace.
// The returned Value is owned by the cache or is a fresh VAL_DICT.
Value load_module_from_path(Interpreter* interpreter, const char* absolute_module_path, Token* error_token);
Value get_or_create_builtin_module(Interpreter* interpreter, const char* module_name, Token* error_token);

// Helper to get the directory part of a file path.
// Caller must free the returned string.
char* get_directory_from_path(const char* file_path);

// Helper to join directory and file name into a new path.
// Caller must free the returned string.
char* join_paths(const char* dir, const char* filename);
#endif // ECHOC_MODULE_LOADER_H