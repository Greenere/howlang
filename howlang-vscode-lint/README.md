# Howlang Lint

A lightweight VS Code extension for `.how` files.

## Features

- Registers the `howlang` language for `.how` files
- Adds syntax highlighting for keywords, strings, numbers, operators, comments, function calls, and class-style instantiation
- Adds lightweight lint diagnostics for:
  - unmatched `()`, `[]`, `{}`
  - unterminated strings
  - unexpected characters
  - malformed `var` declarations
  - malformed `how moduleName` imports
  - suspicious `:` usage
  - `break` used outside likely loop contexts

## Development

```bash
npm install
```

Press `F5` in VS Code to launch an Extension Development Host.

## Packaging

```bash
npm install -g @vscode/vsce
vsce package
```

## Notes

This is intentionally a lightweight linter, not a full parser or language server. It should catch common mistakes quickly while editing Howlang code.
