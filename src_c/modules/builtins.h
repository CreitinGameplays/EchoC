// src_c/modules/builtins.h
#ifndef ECHOC_BUILTINS_H
#define ECHOC_BUILTINS_H

#include "../header.h" // To get Value, Interpreter, Token, report_error definitions

// Forward declare built-in functions
Value builtin_slice(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// builtin for .len support
Value builtin_len(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// Add built-in for .append (for array-type values)
Value builtin_append(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// Async built-ins
Value builtin_async_sleep_create_coro(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);
Value builtin_gather_create_coro(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);
Value builtin_cancel_coro(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// Add other built-in function declarations here as they are created
// e.g. Value builtin_to_upper(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);


#endif // ECHOC_BUILTINS_H