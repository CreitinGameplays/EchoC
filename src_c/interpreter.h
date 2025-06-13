// src_c/interpreter.h
#ifndef ECHOC_INTERPRETER_H
#define ECHOC_INTERPRETER_H

#include "header.h" // For Interpreter, Coroutine types

// Main interpret function: process statements until EOF.
void interpret(Interpreter* interpreter);

// Async Event Loop and Coroutine Management (declarations for functions used by other modules)
void add_to_ready_queue(Interpreter* interpreter, Coroutine* coro);
void run_event_loop(Interpreter* interpreter); 
StatementExecStatus interpret_coroutine_body(Interpreter* interpreter, Coroutine* coro_to_run); 

#endif // ECHOC_INTERPRETER_H