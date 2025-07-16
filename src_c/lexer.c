// src_c/lexer.c
#include "header.h"
#include "parser_utils.h" // For token_type_to_string

Token* make_token(TokenType type, char* value, int line, int col) {
    // Use calloc to ensure all fields are zero-initialized.
    Token* token = calloc(1, sizeof(Token)); 
    if (!token) report_error("System", "Failed to allocate memory for token", NULL);
    // Enhanced Debugging for make_token
    DEBUG_PRINTF("MAKE_TOKEN: Addr=%p, Type=%s (%d), Value='%s', Line=%d, Col=%d",
                 (void*)token, token_type_to_string(type), type, value ? value : "NULL", line, col);
    token->type = type;
    token->value = value;
    token->line = line;
    token->col = col;
    return token;
}

void free_token(Token* token) {
    if (token) {
        // Enhanced Debugging for free_token
        DEBUG_PRINTF("FREE_TOKEN: Addr=%p, Type=%s (%d), Value='%s', Line=%d, Col=%d", (void*)token, (token->type == TOKEN_EOF ? "EOF" : token_type_to_string(token->type)), token->type,
                     token->value ? token->value : "NULL", token->line, token->col);
        // Free the token's value only if it's a type that dynamically allocates its value string.
        // Single-character tokens (PLUS, MINUS, etc.) use string literals for their value,
        // which should not be freed.
        // Keywords also get their string from lexer_get_identifier, which mallocs.
        switch (token->type) {
            case TOKEN_INTEGER:
            case TOKEN_FLOAT:
            case TOKEN_STRING:
            case TOKEN_ID:        
            case TOKEN_LET:   // "let" is processed as an ID first
            case TOKEN_TRUE:  // "true" is processed as an ID first
            case TOKEN_FALSE: // "false" is processed as an ID first
            case TOKEN_AND:
            case TOKEN_OR:
            case TOKEN_NOT:
            case TOKEN_IF:
            case TOKEN_ELIF:
            case TOKEN_ELSE:
            case TOKEN_LOOP:
            case TOKEN_WHILE:
            case TOKEN_FOR:
            case TOKEN_FROM:
            case TOKEN_TO:
            case TOKEN_SKIP:
            case TOKEN_STEP:
            case TOKEN_IN:
            case TOKEN_BREAK:
            case TOKEN_CONTINUE:
            case TOKEN_FUNCT:
            case TOKEN_RETURN:
            case TOKEN_NULL:
            //case TOKEN_END: deprecated
            case TOKEN_TRY:
            case TOKEN_CATCH:
            case TOKEN_AS:
            case TOKEN_ASSIGN_KEYWORD:
            case TOKEN_FINALLY:
            case TOKEN_IS:
            case TOKEN_BLUEPRINT:
            case TOKEN_INHERITS:
            case TOKEN_SUPER:
            case TOKEN_RAISE:
            case TOKEN_LOAD:
            case TOKEN_ASYNC:
            case TOKEN_AWAIT:
            case TOKEN_EQ:  // "=="
            case TOKEN_NEQ: // "!="
            case TOKEN_LTE: // "<="
            case TOKEN_GTE: // ">="
            // Note: Comparison operators like TOKEN_EQ might use string literals if single char,
            // or malloc'd if multi-char. Let's assume multi-char ops get malloc'd values for now.
            // For simplicity, we'll handle their values like IDs for freeing.
                if (token->value) free(token->value);
                break;
            default:
                // For other token types, token->value is usually a literal or not set.
                break;
        }
        free(token); // Free the token struct itself
    } else {
        DEBUG_PRINTF("FREE_TOKEN: Attempt to free NULL token pointer.%s", "");
    }
}

void lexer_advance(Lexer* lexer) {
    // DEBUG_PRINTF("LEXER_ADVANCE_START: Pos=%d, Line=%d, Col=%d, Char='%c'(%d)", lexer->pos, lexer->line, lexer->col, lexer->current_char, lexer->current_char);
    // First, check for newlines to update position correctly
    if (lexer->current_char == '\n') {
        lexer->line++;
        lexer->col = 0; // Reset column BEFORE advancing
        //DEBUG_PRINTF("  LEXER_ADVANCE_NEWLINE: Line incremented to %d, Col reset to 0", lexer->line);
    }

    lexer->pos++;
    lexer->col++;

    if (lexer->pos >= (int)lexer->text_length) { // Use text_length and >=
        lexer->current_char = '\0';
    } else {
        lexer->current_char = lexer->text[lexer->pos]; // current_char is char at new pos
    }
    // DEBUG_PRINTF("LEXER_ADVANCE_END: Pos=%d, Line=%d, Col=%d, NewChar='%c'(%d)", lexer->pos, lexer->line, lexer->col, lexer->current_char, lexer->current_char);
}

// Renamed from lexer_get_integer_str to be more generic
Token* lexer_get_number(Lexer* lexer) {
    size_t capacity = 32;
    char* result_str = malloc(capacity);
    if (!result_str) {
        report_error("System", "Failed to allocate memory for number string", NULL);
    }
    size_t i = 0;
    TokenType type = TOKEN_INTEGER; // Assume integer unless we see a dot

    while(lexer->current_char != '\0' && (isdigit(lexer->current_char) || lexer->current_char == '.')) {
        if (i >= capacity - 1) { // -1 for null terminator
            capacity *= 2;
            char* new_result_str = realloc(result_str, capacity);
            if (!new_result_str) {
                free(result_str);
                report_error("System", "Failed to reallocate memory for number string", NULL);
            }
            result_str = new_result_str;
        }
        if (lexer->current_char == '.') {
            if (type == TOKEN_FLOAT) break; // Can't have two decimals
            type = TOKEN_FLOAT;
        }
        result_str[i++] = lexer->current_char;
        lexer_advance(lexer);
    }
    result_str[i] = '\0';
    return make_token(type, result_str, lexer->line, lexer->col); // Pass line and col, though they might be updated later
}

// Helper for lexer_get_string to manage buffer capacity
static void ensure_string_capacity(char** buffer_ptr, size_t* capacity_ptr, size_t current_length, size_t chars_to_add, Lexer* lexer_for_error_reporting, int start_line, int start_col) {
    if (current_length + chars_to_add + 1 > *capacity_ptr) { // +1 for null terminator
        size_t new_capacity = *capacity_ptr;
        if (new_capacity == 0) new_capacity = 64; // Should be initialized before first call
        while (current_length + chars_to_add + 1 > new_capacity) {
            new_capacity *= 2;
        }
        char* new_buffer = realloc(*buffer_ptr, new_capacity);
        if (!new_buffer) {
            free(*buffer_ptr);
            Token temp_token = {TOKEN_UNKNOWN, NULL, lexer_for_error_reporting ? lexer_for_error_reporting->line : start_line, lexer_for_error_reporting ? lexer_for_error_reporting->col : start_col};
            report_error("System", "Failed to reallocate memory for string literal buffer", &temp_token);
        }
        *buffer_ptr = new_buffer;
        *capacity_ptr = new_capacity;
    }
}

char* lexer_get_string(Lexer* lexer, char quote_char, int start_line_for_error, int start_col_for_error) {
    size_t capacity = 64;
    char* result = malloc(capacity);
    if (!result) {
        Token temp_token = {TOKEN_UNKNOWN, NULL, start_line_for_error, start_col_for_error};
        report_error("System", "Failed to allocate memory for string literal buffer", &temp_token);
    }
    size_t i = 0;
    lexer_advance(lexer); // Skip the opening quote

    int brace_level = 0; // To track nesting inside %{...}

    while (lexer->current_char != '\0') {
        // Check for string termination condition FIRST.
        if (lexer->current_char == quote_char && brace_level == 0) {
            break; // Found the end of the string.
        }

        // Handle escape sequences
        if (lexer->current_char == '\\') {
            ensure_string_capacity(&result, &capacity, i, 1, lexer, start_line_for_error, start_col_for_error);
            lexer_advance(lexer); // Consume backslash
            switch (lexer->current_char) {
                case 'n': result[i++] = '\n'; break;
                case 't': result[i++] = '\t'; break;
                case '\\': result[i++] = '\\'; break;
                case '"': result[i++] = '"'; break;
                case '\'': result[i++] = '\''; break;
                case '%': result[i++] = '%'; break; // Allow escaping '%' itself
                default:
                    // For unknown escapes, just copy the character literally.
                    // This means '\c' becomes 'c' in the string.
                    result[i++] = lexer->current_char;
                    break;
            }
            lexer_advance(lexer); // Consume the character after backslash
            continue; // Go to next loop iteration
        }

        // Handle interpolation start '%{' as a single, atomic unit
        if (lexer->current_char == '%' && lexer->pos + 1 < (int)lexer->text_length && lexer->text[lexer->pos + 1] == '{') {
            brace_level++;
            // Append both '%' and '{' to the result string, advancing the lexer twice
            ensure_string_capacity(&result, &capacity, i, 2, lexer, start_line_for_error, start_col_for_error);
            result[i++] = lexer->current_char; // Append '%'
            lexer_advance(lexer);
            result[i++] = lexer->current_char; // Append '{'
            lexer_advance(lexer);
            continue; // Skip the rest of this loop iteration to avoid double-processing
        }

        // Handle nested braces if we are already inside an interpolation
        if (brace_level > 0) {
            if (lexer->current_char == '{') {
                brace_level++;
            } else if (lexer->current_char == '}') {
                brace_level--;
            }
        }

        // Append the current character to the result string.
        ensure_string_capacity(&result, &capacity, i, 1, lexer, start_line_for_error, start_col_for_error);
        result[i++] = lexer->current_char;
        lexer_advance(lexer);
    }

    if (lexer->current_char != quote_char) {
        free(result);
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Unterminated string literal starting at line %d, col %d.", start_line_for_error, start_col_for_error);
        Token temp_token = {TOKEN_UNKNOWN, NULL, start_line_for_error, start_col_for_error};
        report_error("Lexical", err_msg, &temp_token);
    }
    
    if (brace_level != 0) {
        free(result);
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Mismatched braces in string interpolation starting at line %d, col %d.", start_line_for_error, start_col_for_error);
        Token temp_token = {TOKEN_UNKNOWN, NULL, start_line_for_error, start_col_for_error};
        report_error("Lexical", err_msg, &temp_token);
    }

    lexer_advance(lexer); // Skip the final closing quote

    result[i] = '\0';
    return result;
}


char* lexer_get_identifier(Lexer* lexer) {
    size_t capacity = 32; // Initial capacity
    char* result = malloc(capacity);
    if (!result) report_error("System", "Failed to allocate memory for identifier string", NULL); // Token context might be hard here
    size_t i = 0;

    while (lexer->current_char != '\0' &&
           (isalnum((unsigned char)lexer->current_char) || lexer->current_char == '_')) {
        if (i >= capacity - 1) { // -1 for null terminator
            capacity *= 2;
            char* new_result = realloc(result, capacity);
            if (!new_result) {
                free(result);
                report_error("System", "Failed to reallocate memory for identifier string", NULL);
            }
            result = new_result;
        }
        result[i++] = lexer->current_char;
        lexer_advance(lexer);
    }
    result[i] = '\0';
    return result;
}


// Helper function to parse multiline strings starting with """
char* lexer_get_multiline_string(Lexer* lexer, int start_line_for_error, int start_col_for_error) {
    // Consume opening """
    lexer_advance(lexer); // "
    lexer_advance(lexer); // ""
    lexer_advance(lexer); // """

    size_t capacity = 1024; // Initial buffer capacity
    char* buffer = malloc(capacity);
    if (!buffer) {
        // In a real scenario, make_token for context might be better if available
        Token temp_token = {TOKEN_UNKNOWN, NULL, start_line_for_error, start_col_for_error};
        report_error("System", "Failed to allocate memory for multiline string buffer", &temp_token);
        return NULL; // Should not be reached
    }
    size_t length = 0;

    while (1) { // Loop indefinitely until EOF or closing delimiter
        if (lexer->current_char == '\0') {
            // Unterminated multiline string
            free(buffer);
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "Unterminated multiline string (\"\"\") starting at line %d, col %d.", start_line_for_error, start_col_for_error);
            Token temp_token = {TOKEN_UNKNOWN, NULL, start_line_for_error, start_col_for_error};
            report_error("Lexical", err_msg, &temp_token);
            return NULL; // Should not be reached
        }

        if (lexer->current_char == '"' &&
            lexer->pos + 2 < (int)lexer->text_length &&
            lexer->text[lexer->pos + 1] == '"' &&
            lexer->text[lexer->pos + 2] == '"') {
            // Found closing """
            lexer_advance(lexer); // "
            lexer_advance(lexer); // ""
            lexer_advance(lexer); // """
            break; // Exit loop
        }

        if (length + 1 >= capacity) { // +1 for potential null terminator
            capacity *= 2;
            char* new_buffer = realloc(buffer, capacity);
            if (!new_buffer) {
                free(buffer);
                Token temp_token = {TOKEN_UNKNOWN, NULL, lexer->line, lexer->col}; // Current pos for realloc error
                report_error("System", "Failed to reallocate memory for multiline string buffer", &temp_token);
                return NULL; // Should not be reached
            }
            buffer = new_buffer;
        }
        buffer[length++] = lexer->current_char;
        lexer_advance(lexer); // lexer_advance handles line/col updates for newlines
    }
    buffer[length] = '\0';
    return buffer;
}

// New function to peek at the next token without consuming it from the main lexer stream.
// The returned token is a new allocation and must be freed by the caller.
Token* peek_next_token(Lexer* lexer) {
    // Create a temporary lexer to advance without affecting the main one.
    Lexer temp_lexer = *lexer; 
    
    Token* next_token = get_next_token(&temp_lexer);
    
    // No need to restore state on the main lexer, as we used a copy.
    // The caller is responsible for freeing the returned token.
    return next_token;
}

// --- Lexer State Management Functions ---
LexerState get_lexer_state(Lexer* lexer) {
    LexerState state;
    state.pos = lexer->pos;
    state.current_char = lexer->current_char;
    state.line = lexer->line;
    state.col = lexer->col;
    state.text = lexer->text;               // Save text pointer
    state.text_length = lexer->text_length; // Save text length
    DEBUG_PRINTF("GET_LEXER_STATE: For Lexer ADDR=%p. Captured: Pos=%d, Line=%d, Col=%d, TextPtr=%p, TextLen=%zu, CurrentCharRelevantToPos='%c'",
                 (void*)lexer, // Log address of lexer being snapshotted
                 state.pos, state.line, state.col, (void*)state.text, state.text_length, (state.pos < (int)state.text_length && state.pos >= 0 ? state.text[state.pos] : '?'));
    return state;
}

void set_lexer_state(Lexer* lexer, LexerState state) {
    // Restore text and text_length first, as they are needed for pos validation and line/col recalc.
    lexer->text = state.text;
    lexer->text_length = state.text_length;
    lexer->pos = state.pos;

    // Validate pos against the (potentially new) text_length
    if (lexer->pos < 0) lexer->pos = 0; // Basic sanity
    if ((size_t)lexer->pos > lexer->text_length) lexer->pos = lexer->text_length; // Cap pos at end

    // Recalculate line and col from the restored pos and text, ignoring state.line and state.col
    // as they might be corrupted.
    int current_l = 1;
    int current_c = 1;
    for (int i = 0; i < lexer->pos; ++i) {
        // Ensure we don't read past the buffer if pos was capped but text is shorter than original state.pos
        if (i >= (int)lexer->text_length) break; 
        if (lexer->text[i] == '\n') {
            current_l++;
            current_c = 1;
        } else {
            current_c++;
        }
    }
    lexer->line = current_l;
    lexer->col = current_c;

    if ((size_t)lexer->pos >= lexer->text_length) {
        lexer->current_char = '\0';
    } else {
        lexer->current_char = lexer->text[lexer->pos];
    }

    DEBUG_PRINTF("SET_LEXER_STATE (Recalculated): For Lexer ADDR=%p. Input State (Pos=%d, Line=%d, Col=%d). Effective: Pos=%d, Line=%d, Col=%d, TextPtr=%p, TextLen=%zu, CurrentChar='%c'",
                 (void*)lexer,
                 state.pos, state.line, state.col, // Log original input state for comparison
                 lexer->pos, lexer->line, lexer->col, (void*)lexer->text, lexer->text_length, lexer->current_char);
}

// Rewinds the lexer to a saved state and fetches the token at that position.
// The `first_token_of_block_for_error_reporting_value` is a bit of a misnomer here;
// it's more about managing the `interpreter->current_token` before replacing it.
void rewind_lexer_and_token(Interpreter* interpreter, LexerState saved_lexer_state, Token* first_token_of_block_for_error_reporting_value) {
    // Store the current token to free it *after* getting the new one,
    // to handle cases where get_next_token might return the same pointer (though unlikely with current make_token).
    (void)first_token_of_block_for_error_reporting_value; // Mark as unused to suppress warning
    Token* old_current_token = interpreter->current_token;

    set_lexer_state(interpreter->lexer, saved_lexer_state);
    
    // Get the token that should be at the rewound position.
    interpreter->current_token = get_next_token(interpreter->lexer);

    // Free the old current_token if it's different from the new one.
    if (old_current_token != interpreter->current_token) {
        free_token(old_current_token);
    }
}

// Moved from statement_parser.c - made non-static
// Helper function to get LexerState corresponding to the start of a token (given its line and col)
LexerState get_lexer_state_for_token_start(Lexer* lexer, int token_line, int token_col, Token* error_context_token_for_report) {
    LexerState state;
    state.line = token_line;
    state.col = token_col;
    state.text = lexer->text; // Capture the text pointer from the lexer
    state.text_length = lexer->text_length; // Capture the text length

    int p = 0;
    int current_l = 1;
    int current_c = 1;
    while (lexer->text[p] != '\0') {
        if (current_l == token_line && current_c == token_col) {
            break;
        }
        if (lexer->text[p] == '\n') {
            current_l++;
            current_c = 1;
        } else {
            current_c++;
        }
        p++;
    }
    // Ensure we didn't run past the end of the text without finding the position,
    // unless the found position is exactly at text_length (e.g. for an EOF token).
    if ((size_t)p > lexer->text_length || (lexer->text[p] == '\0' && !(current_l == token_line && current_c == token_col))) {
         report_error("Internal", "Could not find token start position in get_lexer_state_for_token_start", error_context_token_for_report);
    }

    state.pos = p;
    state.current_char = (size_t)p < lexer->text_length ? lexer->text[p] : '\0';
    return state;
}

Token* get_next_token(Lexer* lexer) {
    int line_at_token_start;
    int col_at_token_start;
    DEBUG_PRINTF("GET_NEXT_TOKEN_TOP: Pos=%d, Line=%d, Col=%d, Char='%c'(%d)", lexer->pos, lexer->line, lexer->col, lexer->current_char, lexer->current_char);

    DEBUG_PRINTF("GET_NEXT_TOKEN_LOOP_START: Pos=%d, Line=%d, Col=%d, Char='%c'(%d)", lexer->pos, lexer->line, lexer->col, lexer->current_char, lexer->current_char);
    while (lexer->current_char != '\0') {
        // int skipped_something = 0;  unused

        // 0. Check indentation (only if not already in the middle of skipping)
        // The line and col for the token should be captured *after* all skipping.
        if (lexer->col == 1 && lexer->current_char != '\n' && lexer->current_char != '\0' ) {
            if (lexer->current_char == ' ') { // Starts with space
                int leading_spaces = 0;
                int indentation_error_line = lexer->line;
                int indentation_error_col = lexer->col; // Should be 1 at this point

                while (lexer->current_char == ' ') {
                    leading_spaces++;
                    lexer_advance(lexer); // Consumes the space
                }

                // If, after consuming leading spaces, we find content (not newline, not EOF)
                // and the indentation count is not a multiple of 4, it's an error.
                if (lexer->current_char != '\n' && lexer->current_char != '\0' && (leading_spaces % 4 != 0)) {
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), "Invalid indentation: %d spaces. Must be a multiple of 4.", leading_spaces);
                    Token temp_error_token = {TOKEN_UNKNOWN, NULL, indentation_error_line, indentation_error_col};
                    report_error("Lexical", err_msg, &temp_error_token);
                }
            } else if (isspace((unsigned char)lexer->current_char) && lexer->current_char != ' ') { // Starts with non-space whitespace
                // This block is entered if the line starts with a non-space whitespace character.
                // We only report an error if actual content follows this invalid indentation character.
                // Peek ahead to see if this line has actual content after this initial non-space whitespace.
                int peek_pos = lexer->pos + 1; // Start peeking after the current char
                char peek_char;
                if (peek_pos >= (int)lexer->text_length) {
                    peek_char = '\0';
                } else {
                    peek_char = lexer->text[peek_pos];
                }

                // If the character immediately following the non-space whitespace is NOT a newline or EOF,
                // it means there's content on the line starting with an invalid indent character.
                if (peek_char != '\n' && peek_char != '\0') {
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), "Invalid character ('%c') used for indentation at line %d, col %d. Only spaces are allowed when content follows.", lexer->current_char, lexer->line, lexer->col);
                    Token temp_error_token = {TOKEN_UNKNOWN, NULL, lexer->line, lexer->col};
                    report_error("Lexical", err_msg, &temp_error_token);
                }
                // If peek_char IS \n or \0, the line was effectively "empty" or "whitespace-only" (e.g. "\t\n").
                // No error is reported here; the main lexer loop's whitespace skipping will handle it.
            }
            // If char at col 1 is not whitespace, indentation is 0, which is valid.
            // The lexer will now proceed to regular whitespace/comment skipping or token parsing.
        }

        // 1. Skip whitespace
        if (isspace((unsigned char)lexer->current_char)) {
            DEBUG_PRINTF("  GET_NEXT_TOKEN_SKIP_WHITESPACE: Char='%c'(%d) at L%d C%d", lexer->current_char, lexer->current_char, lexer->line, lexer->col);
            lexer_advance(lexer);
            DEBUG_PRINTF("  GET_NEXT_TOKEN_AFTER_SKIP_WHITESPACE_ADVANCE: New Char='%c'(%d) at L%d C%d", lexer->current_char, lexer->current_char, lexer->line, lexer->col);
            continue;
        }

        // 2. Handle ''' block comments '''
        if (lexer->current_char == '\'' &&
            lexer->pos + 2 < (int)lexer->text_length &&
            lexer->text[lexer->pos + 1] == '\'' &&
            lexer->text[lexer->pos + 2] == '\'') {
            DEBUG_PRINTF("  GET_NEXT_TOKEN_BLOCK_COMMENT_START at L%d C%d", lexer->line, lexer->col);
            
            // Consume the opening '''
            lexer_advance(lexer); 
            lexer_advance(lexer); 
            lexer_advance(lexer); 
            bool found_closing_delimiter = false; // Flag to track if closing delimiter is found

            int comment_start_line = lexer->line; // For error reporting

            // Loop to find the closing "'''"
            while (lexer->current_char != '\0') {
                if (lexer->current_char == '\'' &&
                    lexer->pos + 2 < (int)lexer->text_length &&
                    lexer->text[lexer->pos + 1] == '\'' &&
                    lexer->text[lexer->pos + 2] == '\'') {
                    
                    // Consume the closing '''
                    lexer_advance(lexer); 
                    lexer_advance(lexer); 
                    lexer_advance(lexer);
                    found_closing_delimiter = true; // Set flag
                    break; // Exit inner while (comment content loop)
                }
                lexer_advance(lexer); // Advance through comment content
            }
            if (!found_closing_delimiter) { // Check if loop exited due to EOF *without* finding delimiter
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "Unterminated \"'''\" block comment that started on line %d.", comment_start_line);
                Token temp_error_token = {TOKEN_UNKNOWN, NULL, lexer->line, lexer->col};
                report_error("Lexical", err_msg, &temp_error_token);
            }
            DEBUG_PRINTF("  GET_NEXT_TOKEN_BLOCK_COMMENT_END at L%d C%d", lexer->line, lexer->col);
            continue; 
        }

        // 3. Handle -- inline comments --
        DEBUG_PRINTF("Checking for inline comment. Line: %d, Col: %d, Char: '%c' (%d)", lexer->line, lexer->col, lexer->current_char, lexer->current_char);
        if (lexer->pos + 1 < (int)lexer->text_length) {
            DEBUG_PRINTF("Next char peek: '%c' (%d)", lexer->text[lexer->pos+1], lexer->text[lexer->pos+1]);
        } else {
            DEBUG_PRINTF("Next char peek: EOF or out of bounds%s", "");
        }
        
        if (lexer->current_char == '-' && lexer->pos + 1 < (int)lexer->text_length && lexer->text[lexer->pos + 1] == '-') {
            DEBUG_PRINTF("  GET_NEXT_TOKEN_INLINE_COMMENT_START: Char='%c' at L%d C%d. Skipping.", lexer->current_char, lexer->line, lexer->col);
            lexer_advance(lexer); // Consume the first '-'
            int comment_start_line = lexer->line; // Line where -- started
            int comment_start_col = lexer->col -1; // Column of the first '-'
            lexer_advance(lexer); // Consume the second '-'
            
            // Consume characters until newline or EOF
            bool found_closing_delimiter = false;
            // Consume characters until the closing '--' or EOF
            while (lexer->current_char != '\0') {
                if (lexer->current_char == '-' && lexer->pos + 1 < (int)lexer->text_length && lexer->text[lexer->pos + 1] == '-') {
                    lexer_advance(lexer); // Consume the first '-' of closing delimiter
                    lexer_advance(lexer); // Consume the second '-' of closing delimiter
                    found_closing_delimiter = true;
                    break;
                }
                lexer_advance(lexer); 
            }
            if (!found_closing_delimiter) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "Unterminated inline comment '--' that started on line %d, col %d.", comment_start_line, comment_start_col);
                Token temp_error_token = {TOKEN_UNKNOWN, NULL, lexer->line, lexer->col};
                report_error("Lexical", err_msg, &temp_error_token);
            }
            
            DEBUG_PRINTF("  GET_NEXT_TOKEN_INLINE_COMMENT_END: Char='%c'(%d) at L%d C%d", lexer->current_char, lexer->current_char, lexer->line, lexer->col);
            continue; // Restart token search from current position
        }


        // If we reach here, it means current_char is part of a token
        line_at_token_start = lexer->line; // Capture line/col *after* skipping
        col_at_token_start = lexer->col;
        DEBUG_PRINTF("  GET_NEXT_TOKEN_TOKEN_START_CAPTURE: Line=%d, Col=%d, Char='%c'(%d)", line_at_token_start, col_at_token_start, lexer->current_char, lexer->current_char);

        // --- Token Parsing Logic ---
        // This section should only be reached if no whitespace or comment was skipped in this iteration.
        { 
            if (isalpha((unsigned char)lexer->current_char) || lexer->current_char == '_') {
                char* id_str = lexer_get_identifier(lexer);
                if (strcmp(id_str, "let") == 0) return make_token(TOKEN_LET, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "true") == 0) return make_token(TOKEN_TRUE, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "false") == 0) return make_token(TOKEN_FALSE, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "and") == 0) return make_token(TOKEN_AND, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "or") == 0) return make_token(TOKEN_OR, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "not") == 0) return make_token(TOKEN_NOT, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "if") == 0) return make_token(TOKEN_IF, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "elif") == 0) return make_token(TOKEN_ELIF, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "else") == 0) return make_token(TOKEN_ELSE, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "loop") == 0) return make_token(TOKEN_LOOP, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "null") == 0) return make_token(TOKEN_NULL, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "while") == 0) return make_token(TOKEN_WHILE, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "for") == 0) return make_token(TOKEN_FOR, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "from") == 0) return make_token(TOKEN_FROM, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "to") == 0) return make_token(TOKEN_TO, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "step") == 0) return make_token(TOKEN_STEP, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "skip") == 0) return make_token(TOKEN_SKIP, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "in") == 0) return make_token(TOKEN_IN, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "break") == 0) return make_token(TOKEN_BREAK, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "continue") == 0) return make_token(TOKEN_CONTINUE, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "funct") == 0) return make_token(TOKEN_FUNCT, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "return") == 0) return make_token(TOKEN_RETURN, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "try") == 0) return make_token(TOKEN_TRY, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "catch") == 0) return make_token(TOKEN_CATCH, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "is") == 0) return make_token(TOKEN_IS, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "as") == 0) return make_token(TOKEN_AS, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "finally") == 0) return make_token(TOKEN_FINALLY, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "blueprint") == 0) return make_token(TOKEN_BLUEPRINT, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "inherits") == 0) return make_token(TOKEN_INHERITS, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "super") == 0) return make_token(TOKEN_SUPER, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "raise") == 0) return make_token(TOKEN_RAISE, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "load") == 0) return make_token(TOKEN_LOAD, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "async") == 0) return make_token(TOKEN_ASYNC, id_str, line_at_token_start, col_at_token_start);
                if (strcmp(id_str, "await") == 0) return make_token(TOKEN_AWAIT, id_str, line_at_token_start, col_at_token_start);
                DEBUG_PRINTF("  GET_NEXT_TOKEN_RETURNING_TOKEN: Type=IDENTIFIER, Value='%s', Line=%d, Col=%d", id_str, line_at_token_start, col_at_token_start);
                return make_token(TOKEN_ID, id_str, line_at_token_start, col_at_token_start);
            }
            if (isdigit((unsigned char)lexer->current_char)) {
                Token* num_token = lexer_get_number(lexer);
                num_token->line = line_at_token_start; // Ensure correct line/col
                num_token->col = col_at_token_start;
                DEBUG_PRINTF("  GET_NEXT_TOKEN_RETURNING_TOKEN: Type=%s, Value='%s', Line=%d, Col=%d", token_type_to_string(num_token->type), num_token->value, line_at_token_start, col_at_token_start);
                return num_token;
            }
            // Check for multiline string delimiter """ FIRST
            if (lexer->current_char == '"' &&
                lexer->pos + 2 < (int)lexer->text_length &&
                lexer->text[lexer->pos + 1] == '"' &&
                lexer->text[lexer->pos + 2] == '"') {
                char* ml_str = lexer_get_multiline_string(lexer, line_at_token_start, col_at_token_start);
                DEBUG_PRINTF("  GET_NEXT_TOKEN_RETURNING_TOKEN: Type=STRING (multiline), Value_len=%zu, Line=%d, Col=%d", ml_str ? strlen(ml_str) : 0, line_at_token_start, col_at_token_start);
                return make_token(TOKEN_STRING, ml_str, line_at_token_start, col_at_token_start);
            }
            if (lexer->current_char == '"') {
                char* s_str = lexer_get_string(lexer, '"', line_at_token_start, col_at_token_start);
                DEBUG_PRINTF("  GET_NEXT_TOKEN_RETURNING_TOKEN: Type=STRING (double-quoted), Value_len=%zu, Line=%d, Col=%d", s_str ? strlen(s_str) : 0, line_at_token_start, col_at_token_start);
                return make_token(TOKEN_STRING, s_str, line_at_token_start, col_at_token_start);
            }
            if (lexer->current_char == '\'') {
                char* s_str = lexer_get_string(lexer, '\'', line_at_token_start, col_at_token_start);
                DEBUG_PRINTF("  GET_NEXT_TOKEN_RETURNING_TOKEN: Type=STRING (single-quoted), Value_len=%zu, Line=%d, Col=%d", s_str ? strlen(s_str) : 0, line_at_token_start, col_at_token_start);
                return make_token(TOKEN_STRING, s_str, line_at_token_start, col_at_token_start);
            }

            if (lexer->current_char == '+') { lexer_advance(lexer); return make_token(TOKEN_PLUS, "+", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '-') { lexer_advance(lexer); return make_token(TOKEN_MINUS, "-", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '*') { lexer_advance(lexer); return make_token(TOKEN_MUL, "*", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '/') { lexer_advance(lexer); return make_token(TOKEN_DIV, "/", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '%') { lexer_advance(lexer); return make_token(TOKEN_MOD, "%", line_at_token_start, col_at_token_start); } // New for modulo operator
            if (lexer->current_char == '^') { lexer_advance(lexer); return make_token(TOKEN_POWER, "^", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '(') { lexer_advance(lexer); return make_token(TOKEN_LPAREN, "(", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == ')') { lexer_advance(lexer); return make_token(TOKEN_RPAREN, ")", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == ':') { lexer_advance(lexer); return make_token(TOKEN_COLON, ":", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '{') { lexer_advance(lexer); return make_token(TOKEN_LBRACE, "{", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '}') { lexer_advance(lexer); return make_token(TOKEN_RBRACE, "}", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '?') { lexer_advance(lexer); return make_token(TOKEN_QUESTION, "?", line_at_token_start, col_at_token_start); } // Value is string literal
            if (lexer->current_char == ',') { lexer_advance(lexer); return make_token(TOKEN_COMMA, ",", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '[') { lexer_advance(lexer); return make_token(TOKEN_LBRACKET, "[", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '.') { lexer_advance(lexer); return make_token(TOKEN_DOT, ".", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == ']') { lexer_advance(lexer); return make_token(TOKEN_RBRACKET, "]", line_at_token_start, col_at_token_start); }
            if (lexer->current_char == '=') {
                lexer_advance(lexer);
                if (lexer->current_char == '=') { lexer_advance(lexer); return make_token(TOKEN_EQ, strdup("=="), line_at_token_start, col_at_token_start); } // strdup for "=="
                return make_token(TOKEN_ASSIGN, "=", line_at_token_start, col_at_token_start);
            }
            if (lexer->current_char == '!') {
                lexer_advance(lexer);
                if (lexer->current_char == '=') { lexer_advance(lexer); return make_token(TOKEN_NEQ, strdup("!="), line_at_token_start, col_at_token_start); } // strdup for "!="
            }
            if (lexer->current_char == '<') {
                lexer_advance(lexer);
                if (lexer->current_char == '=') { lexer_advance(lexer); return make_token(TOKEN_LTE, strdup("<="), line_at_token_start, col_at_token_start); } // strdup for "<="
                return make_token(TOKEN_LT, "<", line_at_token_start, col_at_token_start);
            }
            if (lexer->current_char == '>') {
                lexer_advance(lexer);
                if (lexer->current_char == '=') { lexer_advance(lexer); return make_token(TOKEN_GTE, strdup(">="), line_at_token_start, col_at_token_start); } // strdup for ">="
                return make_token(TOKEN_GT, ">", line_at_token_start, col_at_token_start);
            }

            // If no token matched, it's an invalid character
            printf("[EchoC Lexical Error] at line %d, col %d: Invalid character '%c'\n", line_at_token_start, col_at_token_start, lexer->current_char);
            exit(1);
        }
    }
    DEBUG_PRINTF("GET_NEXT_TOKEN_EOF: Line=%d, Col=%d", lexer->line, lexer->col);
    return make_token(TOKEN_EOF, "", lexer->line, lexer->col);
}
