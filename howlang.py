#!/usr/bin/env python3
"""
howlang - interpreter entry point.

Usage:
    python howlang.py                  # interactive REPL
    python howlang.py <file.how>       # run a file
    python howlang.py -e "code"        # eval a string
"""
import sys
import os

# Make sure our modules are on the path
sys.path.insert(0, os.path.dirname(__file__))

from lexer import Lexer, LexError
from parser import Parser, ParseError
from interpreter import Interpreter, HowError, HowReturn, HowQuit

def run_source(source: str, interp: Interpreter, filename: str = "<input>") -> None:
    lines = source.splitlines()
    def show_context(line_no: int, col_no: int = None):
        """Print a few lines of context around the error."""
        if 1 <= line_no <= len(lines):
            print(f"  {filename}:{line_no}", file=sys.stderr)
            print(f"    {lines[line_no-1]}", file=sys.stderr)
            if col_no and col_no > 0:
                print(f"    {' ' * (col_no-1)}^", file=sys.stderr)

    try:
        tokens = Lexer(source).tokenize()
        ast = Parser(tokens).parse()
        interp.run(ast)
    except LexError as e:
        print(f"\033[31m[LexError] {e}\033[0m", file=sys.stderr)
    except ParseError as e:
        # Extract line/col from error message if present
        import re
        m = re.search(r"at (\d+):(\d+)", str(e))
        print(f"\033[31m[ParseError] {e}\033[0m", file=sys.stderr)
        if m:
            show_context(int(m.group(1)), int(m.group(2)))
    except HowReturn:
        pass   # top-level :: — ignore
    except HowQuit:
        raise   # let the REPL catch it
    except HowError as e:
        print(f"\033[31m[RuntimeError] {e}\033[0m", file=sys.stderr)
    except RecursionError:
        print(f"\033[31m[RuntimeError] Maximum recursion depth exceeded\033[0m", file=sys.stderr)

def repl():
    interp = Interpreter()
    print("Howlang 0.1  |  type quit() or Ctrl+D to exit")
    buf = []
    while True:
        try:
            prompt = "... " if buf else ">>> "
            line = input(prompt)
        except EOFError:
            print()
            break
        except KeyboardInterrupt:
            print()
            buf = []
            continue

        if line.strip() == "exit":
            break

        buf.append(line)
        source = "\n".join(buf)

        # Simple heuristic: if the line ends with { or , assume continuation
        stripped = line.rstrip()
        if stripped.endswith("{") or stripped.endswith(","):
            continue

        try:
            run_source(source, interp)
        except HowQuit:
            print()
            break
        buf = []

def run_file(path: str):
    with open(path, "r", encoding="utf-8") as f:
        source = f.read()
    interp = Interpreter()
    # Add the file's own directory as the first import search path
    file_dir = os.path.abspath(os.path.dirname(path))
    if file_dir not in interp._import_dirs:
        interp._import_dirs.insert(0, file_dir)
    run_source(source, interp, filename=path)

def main():
    args = sys.argv[1:]
    if not args:
        repl()
    elif args[0] == "-e" and len(args) >= 2:
        interp = Interpreter()
        run_source(" ".join(args[1:]), interp)
    else:
        run_file(args[0])

if __name__ == "__main__":
    main()
