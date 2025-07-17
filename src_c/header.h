// src_c/header.h
#ifndef ECHOC_HEADER_H
#define ECHOC_HEADER_H
// Current version
#define ECHOC_VERSION "1.0.0-alpha"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h> // For bool type
#include <stdint.h>  // For SIZE_MAX
#ifndef _WIN32
#include <unistd.h> // For realpath and other POSIX functions
#endif

// --- Unique ID Counters for Debugging ---
extern uint64_t next_scope_id;
extern uint64_t next_dictionary_id;
extern uint64_t next_object_id;
// Add more for Array, Coroutine, etc. as needed

// Token Types Enum
typedef enum {
    TOKEN_INTEGER, TOKEN_FLOAT,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_MUL, TOKEN_DIV,
    TOKEN_POWER,
    TOKEN_MOD, // New for modulo operator
    TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_STRING, TOKEN_COLON,
    TOKEN_ID, TOKEN_LET,
    TOKEN_ASSIGN_KEYWORD,
    TOKEN_TRUE, TOKEN_FALSE, TOKEN_NULL,
    TOKEN_AND, TOKEN_OR, TOKEN_NOT,
    TOKEN_EQ, TOKEN_NEQ,
    TOKEN_LT, TOKEN_GT,
    TOKEN_LTE, TOKEN_GTE,
    TOKEN_QUESTION,
    TOKEN_LBRACE, TOKEN_RBRACE,
    TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_COMMA,
    TOKEN_DOT, // For attribute access like object.property
    TOKEN_ASSIGN, // Moved '=' here, as it's distinct from 'assign:' keyword
    TOKEN_BLUEPRINT, // Keyword 'blueprint' for class definition
    TOKEN_INHERITS,  // Keyword 'inherits' for inheritance
    TOKEN_IS,        // Keyword 'is' for identity comparison
    TOKEN_SUPER,     // Keyword 'super' for parent access
    TOKEN_LOAD,      // Keyword 'load' for module importing
    TOKEN_FUNCT, TOKEN_RETURN, 
    TOKEN_ASYNC,     // Keyword 'async' for async function definition
    TOKEN_AWAIT,     // Keyword 'await' for awaiting coroutines
    TOKEN_TRY, TOKEN_CATCH, TOKEN_AS, TOKEN_FINALLY, // New for try-catch
    TOKEN_RAISE,                                    // New for raise

    TOKEN_IF, TOKEN_ELIF, TOKEN_ELSE,
    TOKEN_LOOP, TOKEN_WHILE, TOKEN_FOR, TOKEN_FROM, TOKEN_TO, TOKEN_STEP, TOKEN_IN, TOKEN_SKIP,
    TOKEN_BREAK,
    TOKEN_CONTINUE, TOKEN_EOF, TOKEN_UNKNOWN // TOKEN_END removed
} TokenType;

// Token Struct
typedef struct {
    TokenType type;
    char* value;
    int line;
    int col;
} Token;

// Value Types Enum
typedef enum {
    VAL_INT, VAL_FLOAT, VAL_STRING, VAL_BOOL,
    VAL_ARRAY, VAL_TUPLE, VAL_DICT, VAL_FUNCTION,
    VAL_BLUEPRINT, // Represents a class/blueprint definition
    VAL_OBJECT,    // Represents an instance of a blueprint
    VAL_BOUND_METHOD, // Represents a method bound to an object instance
    VAL_COROUTINE, // Represents a coroutine object instance
    VAL_GATHER_TASK, // Special coroutine type for gather operations
    VAL_SUPER_PROXY,  // Temporary value for super.method() resolution
    VAL_NULL
} ValueType;

#define COROUTINE_MAGIC 0xDEADBEEF

// Forward declare structs used in Value union
struct Array;
struct Tuple;
struct Dictionary;
struct Function;
struct Scope; // Already forward declared
struct Blueprint;
struct Coroutine; // Forward declare Coroutine
struct Object;
struct BoundMethod;
struct BlueprintListNode; // Forward declare for Interpreter struct
struct InterpreterImpl; // Forward declare the actual struct tag
typedef struct InterpreterImpl Interpreter; // Typedef Interpreter for use

// Value Struct
typedef struct {
    ValueType type;
    union {
        long integer;
        double floating;
        char* string_val;
        int bool_val;
        struct Array* array_val;
        struct Tuple* tuple_val;
        struct Dictionary* dict_val;
        struct Function* function_val;
        struct Blueprint* blueprint_val;
        struct Object* object_val;
        struct Coroutine* coroutine_val; // For VAL_COROUTINE
        struct BoundMethod* bound_method_val;
        // VAL_SUPER_PROXY doesn't need data in the union for now
    } as;
} Value;

// A temporary struct to hold a parsed argument before it's mapped to a parameter.
typedef struct {
    char* name; // NULL for positional arguments, non-NULL for named arguments.
    Value value;
    bool is_fresh; // To track if the value needs to be freed.
} ParsedArgument;

// Array Structure
typedef struct Array {
    Value* elements;
    int count;
    int capacity;
} Array;

// Tuple Structure
typedef struct Tuple {
    Value* elements;
    int count;
} Tuple;

// Dictionary Entry Structure
typedef struct DictEntry {
    char* key;
    Value value;
    struct DictEntry* next;
} DictEntry;

// Dictionary Structure
typedef struct Dictionary {
    DictEntry** buckets;
    uint64_t id; // New: Unique ID for debugging
    int num_buckets;
    int count;
} Dictionary;

// Lexer State
typedef struct {
    int pos;
    char current_char;
    int line;
    int col;
    const char* text;      // Add text pointer to LexerState
    size_t text_length;    // Add text length to LexerState
} LexerState;

// Parameter Structure
typedef struct Parameter {
    char* name;
    Value* default_value;
} Parameter;

// Typedef for C built-in function pointers
typedef Value (*CBuiltinFunction)(Interpreter* interpreter, Value* args, int arg_count, Token* call_site_token);

// Function Structure
typedef struct Function {
    char* name;
    Parameter* params;
    int param_count;
    LexerState body_start_state;
    int definition_col; // Column of the 'funct:' keyword
    int definition_line; // Line of the 'funct:' keyword
    struct Scope* definition_scope;
    bool is_async; // Flag to mark async functions
    CBuiltinFunction c_impl; // If not NULL, this is a C function
    char*  source_text_owned_copy; // Malloc'd copy of the source text of the module of definition
    size_t source_text_length;     // Length of the owned copy
    bool is_source_owner;          // True if this Function struct instance owns source_text_owned_copy
    int body_end_token_original_line; // Line number of the 'end:' token for this function
    int body_end_token_original_col;  // Column number of the 'end:' token for this function
} Function;

// SymbolNode Structure
typedef struct SymbolNode {
    char* name;
    Value value;
    struct SymbolNode* next;
} SymbolNode;

// Scope Structure
typedef struct Scope {
    SymbolNode* symbols;
    uint64_t id; // New: Unique ID for debugging
    struct Scope* outer;
} Scope;

// Node for a list of Scopes (used for managing module scopes)
typedef struct ScopeListNode {
    Scope* scope;
    struct ScopeListNode* next;
} ScopeListNode;


// Blueprint (Class) Structure
typedef struct Blueprint {
    char* name;
    struct Blueprint* parent_blueprint; // For inheritance
    Scope* class_attributes_and_methods; // Stores class 'let' vars and 'funct' (methods)
    int definition_col; // Column of the 'blueprint:' keyword
    Function* init_method_cache; // Cached pointer to the 'init' method for faster instantiation
} Blueprint;

// Object (Instance) Structure
typedef struct Object {
    Blueprint* blueprint; // Points to the class definition
    uint64_t id; // New: Unique ID for debugging
    Scope* instance_attributes; // Stores 'self.x' values
    int ref_count; // Reference count for memory management
} Object;

// Node for a list of Blueprints (used for managing all defined blueprints)
typedef struct BlueprintListNode {
    Blueprint* blueprint;
    struct BlueprintListNode* next;
} BlueprintListNode;

// Enum to distinguish between EchoC functions and C built-in functions
typedef enum {
    FUNC_TYPE_ECHOC,
    FUNC_TYPE_C_BUILTIN
} BoundFunctionType;

typedef struct BoundMethod {
    BoundFunctionType type;
    union {
        Function* echoc_function;
        CBuiltinFunction c_builtin;
    } func_ptr;
    Value self_value;
    int self_is_owned_copy;
    int ref_count; // Reference count for memory management
} BoundMethod;

// Node for a list of coroutines waiting on another coroutine
typedef struct CoroutineWaiterNode {
    struct Coroutine* waiter_coro;
    struct CoroutineWaiterNode* next;
} CoroutineWaiterNode;

// Coroutine State Enum
typedef enum {
    CORO_NEW,      // Just created, not yet run
    CORO_RUNNABLE, // Ready to run or resume
    CORO_RESUMING, // Resuming after an await, in a "fast-forward" state
    CORO_SUSPENDED_AWAIT, // Paused on an await
    CORO_SUSPENDED_TIMER, // Paused for a timer (e.g., async_sleep)
    CORO_DONE,     // Execution finished
    CORO_GATHER_WAIT // Special state for gather() coroutine waiting for children
} CoroutineState;

// Coroutine Structure (instance of an async function)
typedef struct Coroutine {
    uint32_t magic_number;      // Magic number to check for validity
    int creation_line;          // Line where the coroutine was created (for warnings)
    int creation_col;           // Column where the coroutine was created
    Function* function_def;     // Pointer to the async Function definition
    char* name;                 // Name of the coroutine (e.g., function name or "async_sleep")
    Scope* execution_scope;     // Its own local variable scope    
    LexerState statement_resume_state; // Lexer state pointing to the start of the statement that yielded.
    LexerState post_await_resume_state;
    CoroutineState state;
    Value result_value;         // Stores the final return value or await result
    struct Coroutine* awaiting_on_coro; // Coroutine this one is waiting for
    int resumed_with_exception; // Flag: 1 if resumed with an exception from awaited task
    // int is_yielding;         // This flag seems redundant with coroutine_yielded_for_await in Interpreter

    // For timer-based suspension (e.g., async_sleep)
    double wakeup_time_sec;     // Absolute time in seconds (e.g., from time(NULL) + delay)

    // For gather()
    Array* gather_tasks;        // Array of VAL_COROUTINE for children tasks
    Array* gather_results;      // Array of Value for results from children
    int gather_pending_count;   // Number of children gather is still waiting for
    int gather_first_exception_idx; // Index of the first child exception in gather_results, or -1
    bool gather_return_exceptions; // New flag for gather behavior
    struct Coroutine* parent_gather_coro; // Link to parent gather task, if any

    int is_cancelled;           // Flag: 1 if cancellation has been requested
    Value exception_value;      // Stores exception if CORO_DONE due to unhandled exception
    int has_exception;          // Flag: 1 if coro completed with an exception.
    int ref_count;              // Reference count for memory management

    CoroutineWaiterNode* waiters_head; // List of coroutines waiting on this one    
    Value value_from_await;     // Stores the result obtained from an awaited coroutine
    int is_in_ready_queue; // Flag to indicate if the coroutine is currently in the ready queue    
    LexerState yielding_await_state; // The state of the lexer at the 'await' that yielded.
    bool has_yielding_await_state;   // Flag to indicate if the above state is valid.
    Token* yielding_await_token; // The specific 'await' token that caused the yield.
    struct TryCatchFrame* try_catch_stack_top; // For coroutine-specific try-catch stack
} Coroutine;

typedef struct {
    const char* text;
    int pos;
    char current_char;
    int line;
    int col;
    size_t text_length;
} Lexer;

// Node for a queue/list of coroutines
typedef struct CoroutineQueueNode {
    Coroutine* coro;
    struct CoroutineQueueNode* next;
} CoroutineQueueNode;

// Interpreter Struct
// Define the struct with the tag InterpreterImpl
struct InterpreterImpl {
    Lexer* lexer;
    Token* current_token;
    Scope* current_scope;
    int loop_depth;
    int break_flag;
    int continue_flag;
    int function_nesting_level;
    Value current_function_return_value;
    int return_flag;

    // --- Exception Handling ---
    Object* current_self_object;      // For 'self' context in methods
    Value current_exception;          // Stores the active exception value (e.g., a VAL_STRING)
    struct TryCatchFrame* try_catch_stack_top; // Pointer to the top of a stack of try-catch frames
    ScopeListNode* active_module_scopes_head; // List of module scopes to be freed at cleanup
    Dictionary* module_cache;         // Cache for loaded modules (path -> Dictionary of exports)
    char* current_executing_file_directory; // Directory of the currently executing file for relative loads
    int in_try_catch_finally_block_definition; // Flag (0 or 1) if currently parsing inside a T-C-F block
    struct BlueprintListNode* all_blueprints_head; // List of all defined blueprints
    // --- Async fields ---
    CoroutineQueueNode* async_ready_queue_head;
    CoroutineQueueNode* async_ready_queue_tail;
    CoroutineQueueNode* async_sleep_queue_head; // New: Head of the sleep queue
    CoroutineQueueNode* async_sleep_queue_tail; // New: Tail of the sleep queue
    Coroutine* current_executing_coroutine; // The coroutine whose code is currently running
    int async_event_loop_active;
    Token* error_token; // Token associated with the current_exception
    int exception_is_active;        // Flag (0 or 1) indicating if an exception is currently being propagated
    int unhandled_error_occured;     // Flag for unhandled async errors
    int repr_depth_count; // For preventing recursion in value_to_string_representation
    char* current_executing_file_path; // New field for better error reporting
    bool prevent_side_effects; // For true short-circuiting
    int resume_depth; // For preventing side-effects during async resume re-execution
    bool gather_last_return_exceptions_flag; // HACK: To pass option to C function
    bool is_dummy_resume_value; // Flag to signal a dummy value from a mismatched await
}; // The typedef 'Interpreter' is already declared above using the tag

// --- Try-Catch-Finally Structures ---
typedef struct CatchClauseInfo {
    int variable_name_present;      // True if 'as <variable_name>' is used
    char* variable_name;            // strdup'd name of the error variable
    LexerState body_start_state;    // Lexer state to jump to for executing this catch block
    // In a more advanced version, you might store the end of the catch block too.
    struct CatchClauseInfo* next;   // For multiple catch clauses in the future (not used in initial impl)
} CatchClauseInfo;

typedef struct TryCatchFrame {
    CatchClauseInfo* catch_clause; // For now, only one generic catch clause is supported

    int finally_present;
    LexerState finally_body_start_state; // Lexer state for the finally block

    // State to restore if an exception propagates past this try-catch
    // Scope* scope_at_try_entry; // For more complex scope unwinding if needed

    // To handle exceptions raised within catch or finally, or unhandled ones
    Value pending_exception_after_finally; // Exception to be re-raised after finally (if any)
    int pending_exception_active_after_finally;

    struct TryCatchFrame* prev;       // Link to the previous frame on the stack
} TryCatchFrame;

// Function Declarations (Prototypes)
Token* get_next_token(Lexer* lexer);
Token* peek_next_token(Lexer* lexer); // New declaration
void interpret(Interpreter* interpreter);
void free_token(Token* token);
Token* token_deep_copy(Token* original);

void free_value_contents(Value val);
Value value_deep_copy(Value original);

LexerState get_lexer_state(Lexer* lexer);
void set_lexer_state(Lexer* lexer, LexerState state);
LexerState get_lexer_state_for_token_start(Lexer* lexer, int token_line, int token_col, Token* error_context_token_for_report); // Moved from statement_parser.c
void rewind_lexer_and_token(Interpreter* interpreter, LexerState saved_lexer_state, Token* first_token_of_block_for_error_reporting_value);
void free_scope(Scope* scope);
void report_error(const char* type, const char* message, Token* token);
Value create_null_value(); // Moved for consistency

extern Interpreter* g_interpreter_for_error_reporting; // For error reporting

// Debugging Macro
#ifdef DEBUG_ECHOC
extern FILE* echoc_debug_log_file;

// These functions are defined in header.c
void log_debug_message_internal(const char* file, int line, const char* func, const char* format, ...);
void print_recent_logs_to_stderr_internal(void);

#define ECHOC_MAX_LOG_FILE_SIZE (1 * 1024 * 1024) // 1 MiB / MB
#define ECHOC_LOG_TRUNCATE_THRESHOLD (ECHOC_MAX_LOG_FILE_SIZE - (16 * 1024)) // Reset if within x KB of limit
#define BUG_PRINTF(format, ...) do { log_debug_message_internal(__FILE__, __LINE__, __func__, format, ##__VA_ARGS__); } while (0)
#define DEBUG_PRINTF(format, ...) BUG_PRINTF(format, ##__VA_ARGS__)
// A printf that also writes to the debug log file if active.
void debug_aware_printf(const char* format, ...);
#else
#define BUG_PRINTF(format, ...) ((void)0)
#define DEBUG_PRINTF(format, ...) ((void)0)
#define debug_aware_printf printf
#endif

// Statement Execution Status (for async yielding)
typedef enum {
    STATEMENT_EXECUTED_OK,
    STATEMENT_YIELDED_AWAIT, // Indicates the statement (via await) caused a yield
    STATEMENT_PROPAGATE_FLAG // Indicates a break/continue/return/exception flag is active
} StatementExecStatus;

#define CANCELLED_ERROR_MSG "Error: Coroutine cancelled"


// void destroy_coroutine(Coroutine* coro); // Consolidated into coroutine_decref_and_free_if_zero

#endif // ECHOC_HEADER_H