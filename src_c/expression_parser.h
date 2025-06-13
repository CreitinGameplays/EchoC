// src_c/expression_parser.h
#ifndef ECHOC_EXPRESSION_PARSER_H
#define ECHOC_EXPRESSION_PARSER_H

#include "header.h" // Provides Interpreter, Value, Token, Function

// Result structure for expression parsing functions, indicating if a returned container is fresh.
typedef struct {
    Value value;
    // True if 'value' is VAL_OBJECT, VAL_ARRAY, or VAL_DICT and was freshly created
    // (e.g., instance creation, literal, result of an operation like '+')
    // rather than retrieved from an existing variable.
    // Also true for VAL_STRING if it's a new string (literal, interpolation, op result).
    bool is_freshly_created_container;
    // True if the expression was solely a primary identifier lookup without any
    // subsequent operations (call, index, attribute access, arithmetic, etc.).
    bool is_standalone_primary_id;
} ExprResult;


// Entry point for parsing any expression (lowest precedence is ternary).
ExprResult interpret_ternary_expr(Interpreter* interpreter); // Added forward declaration
// ExprResult interpret_ternary_expr(Interpreter* interpreter); // Replaced by await
ExprResult interpret_await_expr(Interpreter* interpreter); // New entry point

// Expression parsing functions by precedence (lowest to highest)
ExprResult interpret_logical_or_expr(Interpreter* interpreter);
ExprResult interpret_logical_and_expr(Interpreter* interpreter);
ExprResult interpret_equality_expr(Interpreter* interpreter);
ExprResult interpret_comparison_expr(Interpreter* interpreter);
ExprResult interpret_additive_expr(Interpreter* interpreter);
ExprResult interpret_multiplicative_expr(Interpreter* interpreter);
ExprResult interpret_unary_expr(Interpreter* interpreter);
ExprResult interpret_power_expr(Interpreter* interpreter);
ExprResult interpret_postfix_expr(Interpreter* interpreter); // For primary, calls, and indexing
ExprResult interpret_primary_expr(Interpreter* interpreter); // For literals, identifiers, grouped expr, etc.

// The actual lowest precedence expression is now await or ternary if await is not present.
// For simplicity, let's make interpret_ternary_expr call interpret_await_expr if await is higher,
// or make interpret_await_expr the new entry point.
#define interpret_expression interpret_await_expr // Define the main entry for expression parsing
// Specific literal/construct parsers called by the above
Value interpret_dictionary_literal(Interpreter* interpreter);
// Value interpret_array_literal(Interpreter* interpreter); // Called by primary_expr
// Value interpret_tuple_literal(Interpreter* interpreter); // Called by primary_expr

// Function call parsing
Value interpret_any_function_call(Interpreter* interpreter, const char* func_name_str_or_null_for_bound, Token* func_name_token_for_error_reporting, Value* bound_method_val_or_null);

// Instance creation (made non-static)
Value interpret_instance_creation(Interpreter* interpreter, Blueprint* bp_to_instantiate, Token* call_site_token);

// Helper to execute an EchoC function (not C builtins) with pre-evaluated arguments.
// If self_obj is NULL, it's a regular function call.
Value execute_echoc_function(Interpreter* interpreter, Function* func_to_call, Object* self_obj, Value* call_args, bool* call_args_freshness, int arg_count, Token* call_site_token);

#endif // ECHOC_EXPRESSION_PARSER_H