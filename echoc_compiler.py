import subprocess
import sys
import os

# The list of our C source files, in the correct order
# Ensure all necessary .c files are listed here.
C_SOURCE_FILES = [
    "src_c/header.c",
    "src_c/lexer.c",
    "src_c/parser_utils.c",
    "src_c/scope.c",
    "src_c/dictionary.c",
    "src_c/value_utils.c",
    "src_c/modules/builtins.c",
    "src_c/module_loader.c", # Added module loader
    "src_c/expression_parser.c",
    "src_c/statement_parser.c",
    "src_c/interpreter.c",  # Should be the 'clean' version after stubs are removed
    "src_c/main.c",
]

# --- Build Configuration ---
DEBUG_MODE = True  # Set to False for release builds

def main():
    executable_name = "EchoC"
    if sys.platform == "win32":
        executable_name += ".exe"

    print(f"--- Building EchoC ---")
    
    object_files = []

    # Compile each C source file into its own object file.
    for c_file in C_SOURCE_FILES:
        obj_file = c_file.replace(".c", ".o")
        compile_command = ["gcc", "-std=c11", "-c", c_file, "-o", obj_file, "-I", "src_c", "-lm", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE"]
        if DEBUG_MODE:
            print(f"    -> Compiling {c_file} in DEBUG mode.")
            compile_command.extend(["-g", "-DDEBUG_ECHOC", "-Wall", "-Wextra", "-Wpedantic"])
            # Uncomment to treat warnings as errors
            # compile_command.append("-Werror")
        try:
            subprocess.run(compile_command, check=True)
            print(f"    -> Successfully compiled {c_file} into {obj_file}.")
            object_files.append(obj_file)
        except subprocess.CalledProcessError:
            print(f"Error: Compilation failed for {c_file}.")
            sys.exit(1)

    # Link all object files into the final executable.
    print(f"[2] Linking object files into '{executable_name}'...")
    link_command = ["gcc", "-std=c11"] + object_files + ["-o", executable_name, "-lm"]
    try:
        subprocess.run(link_command, check=True)
        print(f"    -> Success! '{executable_name}' is ready.")
    except subprocess.CalledProcessError:
        print("Error: Linking failed.")
        sys.exit(1)

    # Cleanup: remove object files.
    for obj in object_files:
        if os.path.exists(obj):
            os.remove(obj)

    print("\n--- Build complete ---")
    print(f"To use EchoC, run: ./{executable_name} your_file.echoc")

if __name__ == "__main__":
    main()