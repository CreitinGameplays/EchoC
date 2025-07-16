// src_c/interpreter.h
#ifndef ECHOC_INTERPRETER_H
#define ECHOC_INTERPRETER_H

#ifdef _WIN32
#include <windows.h> // For QueryPerformanceCounter, QueryPerformanceFrequency
#endif
#include <time.h>    // For struct timespec, clock_gettime (POSIX), time_t (fallback)

#include "header.h" // For Interpreter, Coroutine types

// Add declaration for a high-resolution monotonic timer.
double get_monotonic_time_sec(void);

// Main interpret function: process statements until EOF.
void interpret(Interpreter* interpreter);

// Async Event Loop and Coroutine Management (declarations for functions used by other modules)
void add_to_ready_queue(Interpreter* interpreter, Coroutine* coro);
void add_to_sleep_queue(Interpreter* interpreter, Coroutine* coro);
void run_event_loop(Interpreter* interpreter);
// destroy_coroutine is already declared in header.h
StatementExecStatus interpret_coroutine_body(Interpreter* interpreter, Coroutine* coro_to_run); 

#endif // ECHOC_INTERPRETER_H