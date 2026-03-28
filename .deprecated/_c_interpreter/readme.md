# Howlang C Interpreter

Single-file C interpreter for Howlang. No dependencies beyond the C standard library.

- howlang.c is written by Claude
- howlang2.c is written by ChatGPT

## Build on macOS

```bash
# With Apple's built-in clang (no install needed):
cc -O2 -o howlang howlang.c -lm

# Or with Homebrew gcc:
gcc-14 -O2 -o howlang howlang.c -lm
```

## Build on Linux

```bash
gcc -O2 -o howlang howlang.c -lm
```

## Run

```bash
# Run a script
./howlang script.how

# Run multiple files concatenated (like the Python interpreter)
./howlang lib.how test.how

# Start the REPL
./howlang
```
