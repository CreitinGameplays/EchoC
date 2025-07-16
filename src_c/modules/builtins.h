// src_c/modules/builtins.h
#ifndef ECHOC_BUILTINS_H
#define ECHOC_BUILTINS_H

#include "../header.h" // To get Value, Interpreter, Token, report_error definitions

// Built-in for show()
Value builtin_show(Interpreter* interpreter, ParsedArgument* args, int arg_count, Token* call_site_token);

// Built-in for slice()
Value builtin_slice(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// builtin for .len support
Value builtin_len(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// Add built-in for .append (for array-type values)
Value builtin_append(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// Built-in for type()
Value builtin_type(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// Add other built-in function declarations here as they are created
// e.g. Value builtin_to_upper(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);


#endif // ECHOC_BUILTINS_H