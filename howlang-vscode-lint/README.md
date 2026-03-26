# Howlang Lint

A VS Code extension for [Howlang](https://github.com/haoyangli/howlang) `.how` files —
syntax highlighting and lightweight lint diagnostics.

**Author:** Haoyang Li

---

## Features

- Registers the `howlang` language for `.how` files
- Syntax highlighting for:
  - Keywords: `var`, `how`, `where`, `as`, `break`, `and`, `or`, `not`
  - Constants: `true`, `false`, `none`
  - Strings (single and double-quoted), numbers, operators, comments
  - Function calls and class instantiation
- Lint diagnostics:
  | Code | Severity | Description |
  |------|----------|-------------|
  | HL001 | Error | Unterminated string literal |
  | HL002 | Error | Mismatched bracket |
  | HL003 | Error | Unexpected character |
  | HL004 | Error | Unclosed bracket |
  | HL101 | Error | `var` not followed by an identifier |
  | HL102 | Error | `var` declaration missing `=` |
  | HL103 | Error | `how` not followed by a module identifier |
  | HL104 | Warning | Extra tokens after `how moduleName` (beyond `as alias`) |
  | HL105 | Warning | `break` used outside a likely loop context |
  | HL201 | Info | Suspicious `:` (did you mean `::` or a slice?) |

---

## Loop Syntax

The for-range loop uses the `(var=start:stop){ }` form:

```
var total = 0
(i=0:5){
    total += i
}
# total == 10
```

The unbounded auto-loop uses `(:)={ }`:

```
var i = 0
(:)={
    i > 4: break,
    i += 1
}
```

---

## Imports

```
how lru_cache               # binds as "lru_cache"
how graph as g              # binds as "g"
how samples/lru_cache as c  # path import
where samples               # add directory to search path
```

---

## Adding a Logo

Place a **128×128 px PNG** at `images/icon.png` inside this extension folder.
The `package.json` `"icon"` field already points there. This icon appears in
the VS Code Marketplace and the Extensions panel.

To link your publisher identity on the Marketplace:

1. Create a publisher at [marketplace.visualstudio.com/manage](https://marketplace.visualstudio.com/manage)
   and set the publisher ID to match the `"publisher"` field in `package.json` (currently `"haoyangli"`).
2. Generate a Personal Access Token in Azure DevOps and run `vsce login haoyangli`.

---

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

This produces a `.vsix` file you can install locally with
`code --install-extension howlang-lint-*.vsix` or publish with `vsce publish`.
