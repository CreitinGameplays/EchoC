// src_c/parser_utils.c
#include "parser_utils.h"
#include <stdio.h> // For sprintf

const char* token_type_to_string(TokenType type) {
    switch (type) {
        case TOKEN_INTEGER: return "INTEGER";
        case TOKEN_FLOAT: return "FLOAT";
        case TOKEN_PLUS: return "PLUS ('+')";
        case TOKEN_MINUS: return "MINUS ('-')";
        case TOKEN_MUL: return "MUL ('*')";
        case TOKEN_DIV: return "DIV ('/')";
        case TOKEN_POWER: return "POWER ('^')";
        case TOKEN_MOD: return "MOD ('%')";
        case TOKEN_LPAREN: return "LPAREN ('(')";
        case TOKEN_RPAREN: return "RPAREN (')')";
        case TOKEN_STRING: return "STRING";
        case TOKEN_COLON: return "COLON (':')";
        case TOKEN_ID: return "IDENTIFIER";        
        case TOKEN_LET: return "LET_KEYWORD ('let')";
        case TOKEN_ASSIGN: return "ASSIGN ('=')";
        case TOKEN_TRUE: return "TRUE_KEYWORD ('true')";
        case TOKEN_FALSE: return "FALSE_KEYWORD ('false')";
        case TOKEN_AND: return "AND_KEYWORD ('and')";
        case TOKEN_OR: return "OR_KEYWORD ('or')";
        case TOKEN_NOT: return "NOT_KEYWORD ('not')";
        case TOKEN_EQ: return "EQ ('==')";
        case TOKEN_NEQ: return "NEQ ('!=')";
        case TOKEN_LT: return "LT ('<')";
        case TOKEN_GT: return "GT ('>')";
        case TOKEN_LTE: return "LTE ('<=')";
        case TOKEN_GTE: return "GTE ('>=')";
        case TOKEN_QUESTION: return "QUESTION_MARK ('?')";
        case TOKEN_LBRACE: return "LBRACE ('{')";
        case TOKEN_RBRACE: return "RBRACE ('}')";
        case TOKEN_LBRACKET: return "LBRACKET ('[')";
        case TOKEN_RBRACKET: return "RBRACKET (']')";
        case TOKEN_COMMA: return "COMMA (',')";
        case TOKEN_IF: return "IF_KEYWORD ('if')";
        case TOKEN_ELIF: return "ELIF_KEYWORD ('elif')";
        case TOKEN_ELSE: return "ELSE_KEYWORD ('else')";
        case TOKEN_LOOP: return "LOOP_KEYWORD ('loop')";
        case TOKEN_WHILE: return "WHILE_KEYWORD ('while')";
        case TOKEN_FOR: return "FOR_KEYWORD ('for')";
        case TOKEN_FROM: return "FROM_KEYWORD ('from')";
        case TOKEN_TO: return "TO_KEYWORD ('to')";
        case TOKEN_IN: return "IN_KEYWORD ('in')";
        case TOKEN_SKIP: return "SKIP_KEYWORD ('skip')";
        case TOKEN_BREAK: return "BREAK_KEYWORD ('break')";
        case TOKEN_CONTINUE: return "CONTINUE_KEYWORD ('continue')";
        case TOKEN_FUNCT: return "FUNCT_KEYWORD ('funct')";
        case TOKEN_RETURN: return "RETURN_KEYWORD ('return')";
        case TOKEN_NULL: return "NULL_KEYWORD ('null')";
        case TOKEN_STEP: return "STEP_KEYWORD ('step')";
        case TOKEN_TRY: return "TRY_KEYWORD ('try')";
        case TOKEN_CATCH: return "CATCH_KEYWORD ('catch')";
        case TOKEN_AS: return "AS_KEYWORD ('as')";
        case TOKEN_FINALLY: return "FINALLY_KEYWORD ('finally')";
        case TOKEN_RAISE: return "RAISE_KEYWORD ('raise')";
        case TOKEN_BLUEPRINT: return "BLUEPRINT_KEYWORD ('blueprint')";
        case TOKEN_INHERITS: return "INHERITS_KEYWORD ('inherits')";
        case TOKEN_IS: return "IS_KEYWORD ('is')";
        case TOKEN_SUPER: return "SUPER_KEYWORD ('super')";
        case TOKEN_LOAD: return "LOAD_KEYWORD ('load')";
        case TOKEN_ASYNC: return "ASYNC_KEYWORD ('async')";
        case TOKEN_AWAIT: return "AWAIT_KEYWORD ('await')";
        case TOKEN_DOT: return "DOT ('.')";
        case TOKEN_EOF: return "EOF";
        case TOKEN_UNKNOWN: return "UNKNOWN";
        default: return "INVALID_TOKEN_TYPE_IN_SWITCH"; // Should not happen
    }
}

void interpreter_eat(Interpreter* interpreter, TokenType expected_type) {
    if (interpreter->current_token->type == expected_type) {
        Token* old_token = interpreter->current_token;
        interpreter->current_token = get_next_token(interpreter->lexer);
        free_token(old_token); // Free the consumed token
    } else {
        char error_message[512]; // Keep the larger buffer for more detailed messages
        snprintf(error_message, sizeof(error_message), "Expected token %s, but got %s (value: '%s')",
                token_type_to_string(expected_type),
                token_type_to_string(interpreter->current_token->type),
                interpreter->current_token->value ? interpreter->current_token->value : "N/A");
        report_error("Syntax", error_message, interpreter->current_token);
    }
}

void report_error_unexpected_token(Interpreter* interpreter, const char* expected_description) {
    char error_message[512];
    snprintf(error_message, sizeof(error_message),
             "Expected %s, but got %s (value: '%s').",
             expected_description,
             token_type_to_string(interpreter->current_token->type),
             interpreter->current_token->value ? interpreter->current_token->value : "N/A");
    report_error("Syntax", error_message, interpreter->current_token);
}