# Howlang Lint

A VS Code extension for [Howlang](https://github.com/haoyangli/howlang) `.how` files —
syntax highlighting and lightweight lint diagnostics.

**Author:** Haoyang Li

---

## Features

- Registers the `howlang` language for `.how` files
- Syntax highlighting for:
  - Keywords: `var`, `how`, `where`, `as`, `break`, `continue`, `and`, `or`, `not`
  - Operators including parallel loop marker `^` and tensor matmul `@`
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
  | HL105 | Warning | `break` or `continue` used outside a likely loop context |
  | HL106 | Error | `break` inside a parallel loop `^{...}` (not allowed) |
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

Add `^` between `)` and `{` to run iterations in parallel:

```
# Parallel map — all iterations run concurrently, results in index order
var squares = (i=0:len(nums))^{ :: nums(i) * nums(i) }()

# par(list, fn) is sugar for the parallel-map pattern
var doubled = par(nums, (x){ :: x * 2 })
```

Parallel loop rules:
- Reading outer variables is allowed
- Writing to outer variables is a runtime error
- `break` is not allowed; `continue` works normally
- Returns a list if any iteration used `::`, otherwise `none`

The unbounded loop has two valid forms:

- `(:)={ }` for the auto-call form
- `(:){ }()` for the explicit-call form

```
var i = 0
(:)={
    i > 4: break,
    i += 1
}

var j = 0
(:){
    j > 4: break,
    j += 1
}()
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
