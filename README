# EchoC, a Python-inspired interpreter language written and compiled with C (work-in-progress)
*I had nothing to do so...*

EchoC is a dynamically-typed, object-oriented, and asynchronous interpreter language written from scratch in C with the help of Google Gemini. It is designed to be a learning project and a playground for language design concepts, drawing heavy inspiration from Python's syntax and semantics.

# Features

EchoC supports a growing number of modern language features:

*   **Rich Data Types**: `integer`, `float`, `string`, `boolean`, `null`, and container types like `array`, `tuple`, and `dictionary`.
*   **Control Flow**:
    *   `if:/elif:/else:` conditional statements.
    *   Flexible looping with `loop: while condition:`, `loop: for i from start to end step s:`, and `loop: for item in collection:`.
    *   Loop control with `break:`, `continue:`, and `skip:` (a no-op similar to Python's `pass`).
*   **Functions**:
    *   First-class functions with `funct:`.
    *   Support for parameters with default values.
    *   Lexical scoping (closures).
*   **Object-Oriented Programming**:
    *   Class-like structures using the `blueprint:` keyword.
    *   Single inheritance with `inherits`.
    *   Instance attributes and methods via `self`.
    *   Parent method access with `super`.
    *   Customizable string representation with `op_str` and operator overloading with `op_add`.
*   **Asynchronous Programming**:
    *   Define asynchronous functions with `async funct:`.
    *   Pause execution and wait for results with `await`.
    *   A built-in `weaver` module provides a cooperative multitasking event loop.
    *   `weaver.weave(coro)`: Runs the main coroutine and the event loop.
    *   `weaver.gather([...])`: Runs multiple coroutines concurrently.
    *   `weaver.rest(ms)`: A non-blocking sleep.
*   **Modules and Imports**:
    *   Organize code into separate files.
    *   Import modules with `load: module as alias:` or `load: (item1, item2) from module:`.
    *   Built-in `weaver` module for async operations.
*   **Error Handling**: Robust `try:/catch:/finally:` blocks for exception management.
*   **Expressive Syntax**:
    *   String interpolation: `"Hello, %{name}!"`.
    *   Ternary expressions: `let: x = "big" if a > 10 else "small":`.
    *   Full suite of arithmetic, logical, and comparison operators.

# Examples

### Basic "Hello World"
```echoc
-- This is a comment --
show("Hello, World!"):
```

# Functions
```echoc
funct: greet(name="there"):
    return: "Hello, %{name}!":

show(greet("Alice")):
show(greet()):
```

# Object-Oriented Programming with `blueprint`
```echoc
blueprint: Dog:
    funct: init(self, name):
        let: self.name = name:

    funct: op_str(self):
        return: "Dog named %{self.name}":

    funct: bark(self):
        show("%{self.name} says Woof!"):

let: my_dog = Dog("Rex"):
show(my_dog): -- "Dog named Rex" --
my_dog.bark(): -- "Rex says Woof!" --
```

# Asynchronous Example
```echoc
load: (weave, rest, gather) from weaver:
load: binary_search_sqrt from "src_c/echoc_features/sqrt.ecc":

async funct: fetch_data(source, delay):
    show("Fetching data from %{source}..."):
    await rest(delay):
    let: result = binary_search_sqrt(delay):
    show("...finished fetching from %{source}, result is ~%{result}"):
    return: result:

async funct: main():
    show("Starting async operations."):
    let: task1 = fetch_data("Source A", 100):
    let: task2 = fetch_data("Source B", 150):

    let: results = await gather([task1, task2]):
    show("All tasks completed. Results: %{results}"):

-- Run the main async function --
weave(main()):
```

# Prerequisites
- A C compiler (GCC is recommended).
- Python 3.10+ (used for the simple build script)

# Building
The project includes a Python script that invokes the C compiler with the correct source files.
```bash
python3 echoc_compiler.py
```
This will produce an executable named EchoC in the root directory.

# Running a Script
To run an EchoC script, pass the filename as an argument to the interpreter:
```bash
./EchoC my_script.echoc
```

Optionally, you can debug errors with valgrind:
```
valgrind -s --leak-check=full --track-origins=yes --show-leak-kinds=all ./EchoC <script>.echoc
```

Syntax Highlight for EchoC (VSCode): [EchoC Syntax Highlight](https://github.com/CreitinGameplays/EchoC-Syntax-Highlight)

EchoC is a personal project and a work in progress. While it supports a growing set of features, you may encounter bugs or limitations.
Contributions, bug reports, and suggestions are highly appreciated! Feel free to open an issue or submit a pull request on the project's repository.

Thank you for using EchoC! <3
