// src_c/module_loader.c
#include "module_loader.h"
#include "parser_utils.h"      // For token_type_to_string
#include "statement_parser.h"  // For interpret_statement
#include "scope.h"             // For Scope operations
#include "dictionary.h"        // For Dictionary operations
#include "modules/weaver.h"    // For create_weaver_module
#include <limits.h>            // For PATH_MAX (may need to include unistd.h for realpath on POSIX)
#include <errno.h>
#ifndef _WIN32
#include <sys/stat.h> // For stat() to check if path is a regular file
#endif


#ifdef _WIN32
#include <windows.h> // For GetFullPathName
// unistd.h (for realpath on POSIX) is now included via header.h
#endif

static Value execute_module_file_and_get_exports(Interpreter* interpreter, const char* absolute_module_path, Token* error_token);
static char* search_in_directory(const char* dir, const char* module_name, Token* error_token);
static char* get_echoc_executable_directory();

Value get_or_create_builtin_module(Interpreter* interpreter, const char* module_name, Token* error_token) {
    Value cached_val;
    // Use a prefix for built-in modules in the cache to avoid name collisions with files.
    char cache_key[256];
    snprintf(cache_key, sizeof(cache_key), "__builtin__:%s", module_name);

    if (dictionary_try_get(interpreter->module_cache, cache_key, &cached_val, true)) {
        return cached_val;
    }

    Value module_val;
    bool found = false;

    if (strcmp(module_name, "weaver") == 0) {
        module_val = create_weaver_module(interpreter);
        found = true;
    }
    // Add other built-in modules here with `else if`

    if (!found) {
        // This should not be reached if called correctly from interpret_load_statement
        report_error("Internal", "Attempted to load unknown built-in module.", error_token);
    }

    // Cache the newly created module
    dictionary_set(interpreter->module_cache, cache_key, module_val, error_token);

    // Return a deep copy for the caller to use, for consistency with file-based modules.
    Value return_val = value_deep_copy(module_val);
    free_value_contents(module_val); // Free the original created module, as it's now copied in the cache and for the return value.
    return return_val;
}

void initialize_module_system(Interpreter* interpreter) {
    interpreter->module_cache = dictionary_create(16, NULL); // Initial size, error token not critical here
    interpreter->active_module_scopes_head = NULL;
}

void cleanup_module_system(Interpreter* interpreter) {
    if (interpreter->module_cache) {
        // The module cache stores Values of type VAL_DICT.
        // Keys (absolute paths) are strdup'd by dictionary_set.
        // Values are VAL_DICT, whose contents are freed by free_value_contents.
        dictionary_free(interpreter->module_cache, 1 /*free_keys*/, 1 /*free_values_contents*/);
        interpreter->module_cache = NULL;
    }
    if (interpreter->current_executing_file_directory) {
        free(interpreter->current_executing_file_directory);
        interpreter->current_executing_file_directory = NULL;
    }
    // Free all active module scopes
    ScopeListNode* current_node = interpreter->active_module_scopes_head;
    ScopeListNode* next_node;
    while (current_node) {
        next_node = current_node->next;
        DEBUG_PRINTF("Cleanup: Freeing module scope %p", (void*)current_node->scope);
        free_scope(current_node->scope); // free_scope handles symbols and the scope struct itself
        free(current_node);
        current_node = next_node;
    }
}

static char* get_echoc_executable_directory() {
    // This is a placeholder. A robust implementation would use platform-specific APIs
    // (e.g., GetModuleFileName on Windows, readlink /proc/self/exe on Linux)
    // For now, let's assume a simple relative path or an environment variable.
    const char* echoc_home = getenv("ECHOC_HOME");
    if (echoc_home) {
        char* lib_path = join_paths(echoc_home, "lib/");
        return lib_path; // Caller must free
    }
    // Fallback: assume standard library is in a 'lib' subdirectory relative to where
    // the interpreter is run from, or a known install path. This is not very robust.
    // For simplicity, we'll return a path that might require the user to set ECHOC_HOME.
    return strdup("./lib/"); // Caller must free
}


char* get_directory_from_path(const char* file_path) {
    if (!file_path) return NULL;
    char* path_copy = strdup(file_path);
    if (!path_copy) return NULL;

    char* last_slash = strrchr(path_copy, '/');
    char* last_backslash = strrchr(path_copy, '\\');
    char* actual_last_sep = last_slash > last_backslash ? last_slash : last_backslash;

    if (actual_last_sep) {
        *(actual_last_sep + 1) = '\0'; // Terminate after the separator
        return path_copy;
    } else { // No directory separator, implies current directory or just a filename
        free(path_copy);
        return strdup("./"); // Or handle as error/empty string depending on desired behavior
    }
}

char* join_paths(const char* dir, const char* filename) {
    if (!dir || !filename) return NULL;

    // If filename is an absolute path, just duplicate it.
    // This check needs to be platform-specific for robustness.
    #ifdef _WIN32
    // Check for X:\ or \\ (basic check)
    if ((filename[0] != '\0' && filename[1] == ':' && (filename[2] == '\\' || filename[2] == '/')) ||
        (filename[0] == '\\' && filename[1] == '\\')) {
        return strdup(filename);
    }
    #else
    // POSIX: starts with /
    if (filename[0] == '/') {
        return strdup(filename);
    }
    #endif

    size_t dir_len = strlen(dir);
    size_t filename_len = strlen(filename);
    // +1 for separator, +1 for null terminator
    char* result = malloc(dir_len + filename_len + 2);
    if (!result) return NULL;

    strcpy(result, dir);
    // Add separator if dir doesn't end with one and filename doesn't start with one
    if (dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\' &&
        filename_len > 0 && filename[0] != '/' && filename[0] != '\\') {
        strcat(result, "/"); // Default to forward slash
    } else if (dir_len > 0 && (dir[dir_len - 1] == '/' || dir[dir_len - 1] == '\\') &&
               filename_len > 0 && (filename[0] == '/' || filename[0] == '\\')) {
        // Dir ends with sep, filename starts with sep. Remove one sep from filename.
        strcat(result, filename + 1);
        return result;
    }
    strcat(result, filename);
    return result;
}

static char* search_in_directory(const char* dir, const char* module_name, Token* error_token) {
    if (!dir) return NULL;
    char full_path_buffer[PATH_MAX];
    char* resolved_path_alloc = NULL;

    // Try with .ecc extension first
    char module_filename_ecc[256];
    snprintf(module_filename_ecc, sizeof(module_filename_ecc), "%s.ecc", module_name);

    char* temp_path_ecc = join_paths(dir, module_filename_ecc);
    if (!temp_path_ecc) { report_error("System", "Memory allocation failed for path joining (with .ecc).", error_token); }

    #ifdef _WIN32
    if (GetFullPathNameA(temp_path_ecc, PATH_MAX, full_path_buffer, NULL) != 0) {
        DWORD dwAttrib = GetFileAttributesA(full_path_buffer);
        if (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
            FILE* f = fopen(full_path_buffer, "rb"); // Check if it's readable
            if (f) { fclose(f); resolved_path_alloc = strdup(full_path_buffer); }
        }
    }
    #else
    if (realpath(temp_path_ecc, full_path_buffer)) {
        struct stat path_stat;
        if (stat(full_path_buffer, &path_stat) == 0 && S_ISREG(path_stat.st_mode)) {
            FILE* f = fopen(full_path_buffer, "rb"); // Check if it's readable
            if (f) { fclose(f); resolved_path_alloc = strdup(full_path_buffer); }
        }
    }
    #endif
    free(temp_path_ecc);
    if (resolved_path_alloc) return resolved_path_alloc;

    // Try without .ecc extension (if module_name already includes it or is a directory)
    char* temp_path_as_is = join_paths(dir, module_name); // Corrected variable name
    if (!temp_path_as_is) { report_error("System", "Memory allocation failed for path joining (as is).", error_token); }


    #ifdef _WIN32
    if (GetFullPathNameA(temp_path_as_is, PATH_MAX, full_path_buffer, NULL) != 0) {
        FILE* f = fopen(full_path_buffer, "r");
        if (f) { fclose(f); resolved_path_alloc = strdup(full_path_buffer); }
    }
    #else
    if (realpath(temp_path_as_is, full_path_buffer)) {
        struct stat path_stat;
        if (stat(full_path_buffer, &path_stat) == 0 && S_ISREG(path_stat.st_mode)) {
            FILE* f = fopen(full_path_buffer, "rb"); // Check if it's readable
            if (f) { fclose(f); resolved_path_alloc = strdup(full_path_buffer); }
        }
    }
    #endif
    free(temp_path_as_is);
    return resolved_path_alloc; // Will be NULL if not found
}

char* resolve_module_path(Interpreter* interpreter, const char* module_name_or_path, Token* error_token) {
    char* resolved_path_alloc = NULL;
    
    // 1. Relative to Current File's Directory
    if (interpreter->current_executing_file_directory) {
        resolved_path_alloc = search_in_directory(interpreter->current_executing_file_directory, module_name_or_path, error_token);
        if (resolved_path_alloc) return resolved_path_alloc;
    }

    // 2. Standard Library Path
    char* std_lib_dir = get_echoc_executable_directory(); // Needs robust implementation
    if (std_lib_dir) {
        resolved_path_alloc = search_in_directory(std_lib_dir, module_name_or_path, error_token);
        free(std_lib_dir);
        if (resolved_path_alloc) return resolved_path_alloc;
    }

    // 3. ECHOC_PATH
    const char* echoc_path_env = getenv("ECHOC_PATH");
    if (echoc_path_env) {
        char* echoc_path_copy = strdup(echoc_path_env);
        if (!echoc_path_copy) { report_error("System", "Failed to duplicate ECHOC_PATH.", error_token); }

        char *saveptr_env; // For strtok_r
        char* path_token = strtok_r(echoc_path_copy,
                               #ifdef _WIN32
                               ";"
                               #else
                               ":"
                               #endif
                               , &saveptr_env);
        while (path_token != NULL) {
            resolved_path_alloc = search_in_directory(path_token, module_name_or_path, error_token);
            if (resolved_path_alloc) {
                free(echoc_path_copy);
                return resolved_path_alloc;
            }
            path_token = strtok_r(NULL,
                                #ifdef _WIN32
                                ";"
                                #else
                                ":"
                                #endif
                                , &saveptr_env);
        }
        free(echoc_path_copy);
    }

    char err_msg[PATH_MAX + 100];
    snprintf(err_msg, sizeof(err_msg), "Module '%s' not found.", module_name_or_path);
    report_error("Runtime", err_msg, error_token);
    return NULL; // Should not be reached
}

Value load_module_from_path(Interpreter* interpreter, const char* absolute_module_path, Token* error_token) {
    Value cached_val;
    if (dictionary_try_get(interpreter->module_cache, absolute_module_path, &cached_val, true /*DEEP_COPY_CONTENTS for cached module*/)) {
        // cached_val is already a deep copy from dictionary_try_get
        // Check for placeholder indicating circular dependency
        if (cached_val.type == VAL_NULL) { // Using VAL_NULL as placeholder
            // This means we are in a circular import situation. Return the placeholder.
            // The caller (likely another module's execution) will get this VAL_NULL.
            // When the original execution of this module finishes, the placeholder will be replaced.
            DEBUG_PRINTF("Circular dependency detected for module: %s. Returning placeholder.", absolute_module_path);
            return cached_val; // Return the deep copied placeholder
        }
        DEBUG_PRINTF("Module %s found in cache. Returning (deep copy).", absolute_module_path);
        return cached_val; // Return the deep copied module
    }

    // Not in cache. Put placeholder.
    Value placeholder = create_null_value();
    dictionary_set(interpreter->module_cache, absolute_module_path, placeholder, error_token);
    // placeholder is VAL_NULL, dictionary_set makes a copy, no need to free original placeholder value contents.

    Value module_exports_dict = execute_module_file_and_get_exports(interpreter, absolute_module_path, error_token);
    // If execute_module_file_and_get_exports fails, it calls report_error and exits.

    // Replace placeholder with actual module exports
    dictionary_set(interpreter->module_cache, absolute_module_path, module_exports_dict, error_token);
    
    Value return_val = value_deep_copy(module_exports_dict);
    free_value_contents(module_exports_dict); // Free the local one as it's copied for return and cache.
    return return_val;
}

static Value execute_module_file_and_get_exports(Interpreter* interpreter, const char* absolute_module_path, Token* error_token) {
    FILE* file = fopen(absolute_module_path, "rb");
    if (!file) {
        char err_msg[PATH_MAX + 100];
        snprintf(err_msg, sizeof(err_msg), "Could not open module file '%s'. Error: %s", absolute_module_path, strerror(errno));
        report_error("Runtime", err_msg, error_token);
    }

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    // Check for ftell errors or excessively large files
    if (fsize == -1L) {
        char err_msg[PATH_MAX + 200];
        snprintf(err_msg, sizeof(err_msg), "Error determining size of module file '%s': %s", absolute_module_path, strerror(errno));
        fclose(file);
        report_error("System", err_msg, error_token);
    }
    if (fsize < 0) { // Should be caught by -1L, but as a safeguard
        char err_msg[PATH_MAX + 100];
        snprintf(err_msg, sizeof(err_msg), "Module file '%s' reported an invalid negative size: %ld.", absolute_module_path, fsize);
        fclose(file);
        report_error("System", err_msg, error_token);
    }
    if (fsize == LONG_MAX || (unsigned long)fsize >= SIZE_MAX -1 ) { // Check for potential overflow with +1 or if fsize itself is too large
        char err_msg[PATH_MAX + 100];
        snprintf(err_msg, sizeof(err_msg), "Module file '%s' is too large to load (%ld bytes).", absolute_module_path, fsize);
        fclose(file);
        report_error("System", err_msg, error_token);
    }

    fseek(file, 0, SEEK_SET); // Rewind to the beginning of the file
    char* source_code = malloc((size_t)fsize + 1); // Allocate memory for the source code
    if (!source_code) { fclose(file); report_error("System", "Failed to allocate memory for module source.", error_token); } // Check allocation
    fread(source_code, 1, fsize, file);
    fclose(file);
    source_code[fsize] = 0;

    Lexer module_lexer = { source_code, 0, source_code[0], 1, 1, (size_t)fsize };
    Scope* module_scope = malloc(sizeof(Scope));
    if (!module_scope) { free(source_code); report_error("System", "Failed to allocate scope for module.", error_token); }
    module_scope->symbols = NULL;
    module_scope->id = next_scope_id++;
    module_scope->outer = NULL; // Modules have their own independent global scope.
                                // A "builtins" scope could be implicitly outer to this if desired later.

    // Temporarily switch interpreter context for module execution
    Lexer* old_lexer = interpreter->lexer;
    Token* old_token = interpreter->current_token;
    Scope* old_scope = interpreter->current_scope;
    char* old_exec_path = interpreter->current_executing_file_path;
    char* old_exec_dir = interpreter->current_executing_file_directory;

    interpreter->lexer = &module_lexer;
    interpreter->current_token = get_next_token(interpreter->lexer);
    interpreter->current_scope = module_scope;
    interpreter->current_executing_file_path = strdup(absolute_module_path);
    interpreter->current_executing_file_directory = get_directory_from_path(absolute_module_path);

    // Add module_scope to the list of active module scopes
    ScopeListNode* new_scope_node = malloc(sizeof(ScopeListNode));
    if (!new_scope_node) { /* cleanup and report error */ }
    new_scope_node->scope = module_scope;
    new_scope_node->next = interpreter->active_module_scopes_head;
    interpreter->active_module_scopes_head = new_scope_node;
    DEBUG_PRINTF("Added module scope %p to active_module_scopes_head", (void*)module_scope);

    DEBUG_PRINTF("Executing module: %s. Temp exec dir: %s", absolute_module_path, interpreter->current_executing_file_directory);

    while (interpreter->current_token->type != TOKEN_EOF) {
        interpret_statement(interpreter);
        if (interpreter->exception_is_active) break; // Propagate exception from module
    }

    Value exports_dict_val;
    exports_dict_val.type = VAL_DICT;
    exports_dict_val.as.dict_val = dictionary_create(16, error_token);

    if (!interpreter->exception_is_active) { // Only gather exports if no unhandled exception
        SymbolNode* s_node = module_scope->symbols;
        while (s_node) {
            if (s_node->name[0] != '_') { // Export if not starting with underscore
                dictionary_set(exports_dict_val.as.dict_val, s_node->name, s_node->value, error_token);
            }
            s_node = s_node->next;
        }
    }

    // Restore interpreter context
    free_token(interpreter->current_token); // Free EOF of module
    interpreter->lexer = old_lexer;
    interpreter->current_token = old_token;
    interpreter->current_scope = old_scope;
    if (interpreter->current_executing_file_path) free(interpreter->current_executing_file_path);
    interpreter->current_executing_file_path = old_exec_path;
    if (interpreter->current_executing_file_directory) free(interpreter->current_executing_file_directory);
    interpreter->current_executing_file_directory = old_exec_dir;

    // DO NOT free module_scope here. It's now managed by active_module_scopes_head
    // and will be freed during cleanup_module_system.
    free(source_code); // Free the module's source code string

    if (interpreter->exception_is_active) { // If module execution had an unhandled exception
        free_value_contents(exports_dict_val); // Free the partially formed/empty exports dict
        // The exception is already set in the interpreter, it will propagate.
        // We need a way to signal this failure to load_module_from_path so it doesn't cache a bad module.
        // For now, report_error would have exited. If we change report_error, this needs more thought.
        // Let's assume report_error exits.
    }

    return exports_dict_val;
}