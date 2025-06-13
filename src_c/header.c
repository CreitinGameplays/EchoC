#ifndef HEADER_C_FUNCTIONS
#define HEADER_C_FUNCTIONS

#include <stdarg.h> // For va_list, va_start, va_end, vsnprintf
#include "header.h" // Include the shared header

#ifdef DEBUG_ECHOC
// Circular buffer for recent logs
#define MAX_RECENT_LOGS 50 // Number of recent logs to keep
#define MAX_LOG_MESSAGE_LEN 512 // Max length of a single log message

static char recent_logs[MAX_RECENT_LOGS][MAX_LOG_MESSAGE_LEN];
static int recent_log_next_index = 0;
static int recent_log_current_count = 0;
static int logs_initialized = 0; // To ensure buffer is clean on first use

// Helper to initialize/clear the log buffer
static void initialize_log_buffer() {
    if (!logs_initialized) {
        for (int i = 0; i < MAX_RECENT_LOGS; ++i) {
            recent_logs[i][0] = '\0';
        }
        recent_log_next_index = 0;
        recent_log_current_count = 0;
        logs_initialized = 1;
    }
}

void log_debug_message_internal(const char* file, int line, const char* func, const char* format, ...) {
    initialize_log_buffer(); // Ensure buffer is ready

    char formatted_message[MAX_LOG_MESSAGE_LEN];
    char temp_buffer[MAX_LOG_MESSAGE_LEN - 128]; // Buffer for user message part, leave space for prefix
    va_list args;

    // Format user message
    va_start(args, format);
    vsnprintf(temp_buffer, sizeof(temp_buffer), format, args);
    va_end(args);

    // Prepend file, line, func for the detailed log message
    snprintf(formatted_message, MAX_LOG_MESSAGE_LEN, "[ECHOC_DBG] %s:%d:%s(): %s", file, line, func, temp_buffer);

    // Store in circular buffer
    strncpy(recent_logs[recent_log_next_index], formatted_message, MAX_LOG_MESSAGE_LEN - 1);
    recent_logs[recent_log_next_index][MAX_LOG_MESSAGE_LEN - 1] = '\0'; // Ensure null termination
    recent_log_next_index = (recent_log_next_index + 1) % MAX_RECENT_LOGS;
    if (recent_log_current_count < MAX_RECENT_LOGS) {
        recent_log_current_count++;
    }

    // Write to log file (with existing truncation logic, now part of this function)
    if (echoc_debug_log_file) {
        fprintf(echoc_debug_log_file, "%s\n", formatted_message); // Add newline for file log
        fflush(echoc_debug_log_file);
    }
}

void print_recent_logs_to_stderr_internal(void) {
    if (!logs_initialized || recent_log_current_count == 0) {
        return;
    }
    fprintf(stderr, "\n--- Recent Logs Leading to Error ---\n");
    int start_index;
    if (recent_log_current_count < MAX_RECENT_LOGS) {
        start_index = 0; // Buffer not full yet, start from beginning
    } else {
        start_index = recent_log_next_index; // Buffer is full, next_index is the oldest
    }

    for (int i = 0; i < recent_log_current_count; ++i) {
        fprintf(stderr, "%s\n", recent_logs[(start_index + i) % MAX_RECENT_LOGS]);
    }
    fprintf(stderr, "--- End of Recent Logs ---\n\n");
}
#endif // DEBUG_ECHOC

void report_error(const char* type, const char* message, Token* token) {
#ifdef DEBUG_ECHOC
    // Truncate log file if needed (moved from DEBUG_PRINTF macro)
    if (echoc_debug_log_file) {
        fflush(echoc_debug_log_file); 
        long current_pos = ftell(echoc_debug_log_file);
        if (current_pos != -1 && current_pos > ECHOC_LOG_TRUNCATE_THRESHOLD) {
            fclose(echoc_debug_log_file);
            echoc_debug_log_file = fopen("echoc_runtime_log.txt", "w");
            if (echoc_debug_log_file) {
                fprintf(echoc_debug_log_file, "[ECHOC_LOG_INFO] Log file reached threshold (%ld bytes), truncated.\n", current_pos);
                setvbuf(echoc_debug_log_file, NULL, _IOLBF, 0);
                fflush(echoc_debug_log_file);
            } else { 
                fprintf(stderr, "[ECHOC_CRITICAL_LOG_ERROR] Failed to reopen log file after truncation attempt.\n");
            }
        }
    }
    print_recent_logs_to_stderr_internal(); // Print recent logs before the error message
    // Also log the error itself to the debug file if open
    if (echoc_debug_log_file) {
        if (token) {
            fprintf(echoc_debug_log_file, "[ECHOC %s Error] at line %d, col %d: %s\n", type, token->line, token->col, message);
        } else {
            fprintf(echoc_debug_log_file, "[ECHOC %s Error] (unknown location): %s\n", type, message);
        }
        fflush(echoc_debug_log_file);
    }
#endif
    if (token) {
        fprintf(stderr, "[EchoC %s Error] at line %d, col %d: %s\n", type, token->line, token->col, message);
    } else {
        fprintf(stderr, "[EchoC %s Error] (unknown location): %s\n", type, message);
    }
    exit(1);
}

Value create_null_value() {
    Value val;
    val.type = VAL_NULL;
    // No .as field needs to be set for VAL_NULL
    return val;
}

Token* token_deep_copy(Token* original) {
    if (!original) return NULL;
    Token* copy = malloc(sizeof(Token));
    if (!copy) {
        fprintf(stderr, "[EchoC System Error] Critical: Failed to allocate memory for token copy in token_deep_copy.\n");
        exit(1);
    }
    *copy = *original; // Shallow copy members like type, line, col

    // Deep copy the 'value' string if it's a type that owns its string
    // Keywords and identifiers get their values from strdup in the lexer.
    // Literals (numbers, strings) also get strdup'd values from the lexer.
    // Multi-character operators (==, !=, <=, >=) also get strdup'd values.
    // Single-character operators (+, -, *, /, (, ), :, etc.) use string literals and should NOT be strdup'd here.
    switch (original->type) {
        // Types whose 'value' is dynamically allocated by the lexer and needs deep copy
        case TOKEN_ID: // Explicitly handle TOKEN_ID
            if (original->value) {
                copy->value = strdup(original->value);
                if (!copy->value) {
                    free(copy);
                    fprintf(stderr, "[EchoC System Error] Critical: Failed to strdup token value in token_deep_copy for TOKEN_ID.\n");
                    exit(1);
                }
            } else {
                copy->value = NULL;
            }
            break;
        case TOKEN_INTEGER:
        case TOKEN_FLOAT:
        case TOKEN_STRING:
        // Keywords are initially lexed as TOKEN_ID then converted; their value is from strdup in the lexer.
        // All keywords that have a ->value need to be deep copied.
        case TOKEN_SHOW: case TOKEN_LET: case TOKEN_ASSIGN_KEYWORD: case TOKEN_TRUE:
        case TOKEN_FALSE: case TOKEN_AND: case TOKEN_OR: case TOKEN_NOT: case TOKEN_IF:
        case TOKEN_ELIF: case TOKEN_ELSE: case TOKEN_LOOP: case TOKEN_WHILE: case TOKEN_FOR:
        case TOKEN_FROM: case TOKEN_TO: case TOKEN_STEP: case TOKEN_IN: case TOKEN_BREAK:
        case TOKEN_CONTINUE: case TOKEN_END: case TOKEN_FUNCT: case TOKEN_RETURN:
        case TOKEN_NULL: case TOKEN_BLUEPRINT: case TOKEN_INHERITS: case TOKEN_SUPER:
        case TOKEN_TRY: case TOKEN_CATCH: case TOKEN_AS: case TOKEN_FINALLY: case TOKEN_RAISE:
        // Multi-character operators that get strdup'd values
        case TOKEN_EQ:  // "=="
        case TOKEN_NEQ: // "!="
        case TOKEN_LTE: // "<="
        case TOKEN_GTE: // ">="
            if (original->value) {
                copy->value = strdup(original->value);
                if (!copy->value) {
                    free(copy);
                    fprintf(stderr, "[EchoC System Error] Critical: Failed to strdup token value in token_deep_copy for type %d.\n", original->type);
                    exit(1);
                }
            } else {
                // This case should ideally not happen for these token types if lexer is correct
                copy->value = NULL;
            }
            break;
        default:
            // For single-char operators and other types that use string literals (e.g., "+", "(", ":")
            // or have NULL value (TOKEN_EOF), just copy the pointer.
            // No deep copy needed as these are static string literals or NULL.
            copy->value = original->value; // Pointer copy is fine
            break;
    }
    return copy;
}

#endif // HEADER_C_FUNCTIONS