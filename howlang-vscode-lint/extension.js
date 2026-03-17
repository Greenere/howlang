const vscode = require('vscode');

const DIAG_COLLECTION = vscode.languages.createDiagnosticCollection('howlang');

function activate(context) {
  context.subscriptions.push(DIAG_COLLECTION);

  const runLintCommand = vscode.commands.registerCommand('howlangLint.run', () => {
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.languageId === 'howlang') {
      lintDocument(editor.document);
      vscode.window.setStatusBarMessage('Howlang lint complete', 1500);
    }
  });

  context.subscriptions.push(runLintCommand);

  if (vscode.window.activeTextEditor?.document.languageId === 'howlang') {
    lintDocument(vscode.window.activeTextEditor.document);
  }

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => {
      if (doc.languageId === 'howlang') lintDocument(doc);
    }),
    vscode.workspace.onDidChangeTextDocument((evt) => {
      if (evt.document.languageId === 'howlang') lintDocument(evt.document);
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      DIAG_COLLECTION.delete(doc.uri);
    })
  );
}

function deactivate() {
  DIAG_COLLECTION.dispose();
}

function lintDocument(document) {
  const text = document.getText();
  const diagnostics = [];
  const scan = scanTokens(text, document, diagnostics);
  runStructuralChecks(scan, document, diagnostics);
  DIAG_COLLECTION.set(document.uri, diagnostics);
}

function addDiagnostic(document, diagnostics, line, startCol, endCol, message, severity = vscode.DiagnosticSeverity.Error, code = 'HL000') {
  const safeLine = Math.max(0, Math.min(line, document.lineCount - 1));
  const lineText = document.lineAt(safeLine).text;
  const start = Math.max(0, Math.min(startCol, lineText.length));
  const end = Math.max(start + 1, Math.min(endCol, lineText.length || start + 1));
  const range = new vscode.Range(safeLine, start, safeLine, end);
  const d = new vscode.Diagnostic(range, message, severity);
  d.code = code;
  d.source = 'howlang-lint';
  diagnostics.push(d);
}

function scanTokens(text, document, diagnostics) {
  const tokens = [];
  const bracketStack = [];

  let i = 0;
  let line = 0;
  let col = 0;
  let lastSignificantToken = null;

  function pushToken(type, value, startLine, startCol, endLine, endCol) {
    const token = { type, value, line: startLine, col: startCol, endLine, endCol };
    tokens.push(token);
    if (type !== 'newline') {
      lastSignificantToken = token;
    }
    return token;
  }

  function advance(ch) {
    i += 1;
    if (ch === '\n') {
      line += 1;
      col = 0;
    } else {
      col += 1;
    }
  }

  while (i < text.length) {
    const ch = text[i];

    if (ch === ' ' || ch === '\t' || ch === '\r') {
      advance(ch);
      continue;
    }

    if (ch === '\n') {
      pushToken('newline', '\n', line, col, line, col + 1);
      advance(ch);
      continue;
    }

    if (ch === '#') {
      while (i < text.length && text[i] !== '\n') advance(text[i]);
      continue;
    }

    if (ch === '"' || ch === "'") {
      const quote = ch;
      const startLine = line;
      const startCol = col;
      let escaped = false;
      advance(ch);
      let closed = false;
      while (i < text.length) {
        const cur = text[i];
        if (cur === '\n' && !escaped) break;
        if (!escaped && cur === quote) {
          advance(cur);
          closed = true;
          break;
        }
        if (!escaped && cur === '\\') {
          escaped = true;
          advance(cur);
          continue;
        }
        escaped = false;
        advance(cur);
      }
      if (!closed) {
        addDiagnostic(document, diagnostics, startLine, startCol, startCol + 1, 'Unterminated string literal.', vscode.DiagnosticSeverity.Error, 'HL001');
      } else {
        pushToken('string', quote, startLine, startCol, line, col);
      }
      continue;
    }

    if (/[0-9]/.test(ch)) {
      const startLine = line;
      const startCol = col;
      let value = ch;
      advance(ch);
      while (i < text.length && /[0-9]/.test(text[i])) {
        value += text[i];
        advance(text[i]);
      }
      if (i < text.length && text[i] === '.' && /[0-9]/.test(text[i + 1] || '')) {
        value += text[i];
        advance(text[i]);
        while (i < text.length && /[0-9]/.test(text[i])) {
          value += text[i];
          advance(text[i]);
        }
      }
      pushToken('number', value, startLine, startCol, line, col);
      continue;
    }

    if (/[A-Za-z_]/.test(ch)) {
      const startLine = line;
      const startCol = col;
      let value = ch;
      advance(ch);
      while (i < text.length && /[A-Za-z0-9_]/.test(text[i])) {
        value += text[i];
        advance(text[i]);
      }
      const keywords = new Set(['var', 'how', 'break', 'and', 'or', 'not', 'true', 'false', 'none']);
      pushToken(keywords.has(value) ? 'keyword' : 'ident', value, startLine, startCol, line, col);
      continue;
    }

    const startLine = line;
    const startCol = col;
    const two = text.slice(i, i + 2);
    const twoCharOps = new Set(['::', '+=', '-=', '*=', '/=', '==', '!=', '<=', '>=', '&&', '||']);
    if (twoCharOps.has(two)) {
      pushToken('op', two, startLine, startCol, line, col + 2);
      advance(text[i]);
      advance(text[i]);
      continue;
    }

    const oneCharOps = new Set(['+', '-', '*', '/', '%', '=', '<', '>', ':', '.', ',', ';']);
    const openBrackets = new Map([['(', ')'], ['[', ']'], ['{', '}']]);
    const closeBrackets = new Map([[')', '('], [']', '['], ['}', '{']]);

    if (openBrackets.has(ch)) {
      const token = pushToken('open', ch, startLine, startCol, line, col + 1);
      bracketStack.push(token);
      advance(ch);
      continue;
    }

    if (closeBrackets.has(ch)) {
      pushToken('close', ch, startLine, startCol, line, col + 1);
      const expectedOpen = closeBrackets.get(ch);
      const actualOpen = bracketStack.pop();
      if (!actualOpen || actualOpen.value !== expectedOpen) {
        addDiagnostic(
          document,
          diagnostics,
          startLine,
          startCol,
          startCol + 1,
          actualOpen
            ? `Mismatched closing '${ch}'. Expected closing '${openBrackets.get(actualOpen.value)}' for '${actualOpen.value}'.`
            : `Unexpected closing '${ch}'.`,
          vscode.DiagnosticSeverity.Error,
          'HL002'
        );
      }
      advance(ch);
      continue;
    }

    if (oneCharOps.has(ch)) {
      pushToken('op', ch, startLine, startCol, line, col + 1);
      advance(ch);
      continue;
    }

    addDiagnostic(document, diagnostics, startLine, startCol, startCol + 1, `Unexpected character '${ch}'.`, vscode.DiagnosticSeverity.Error, 'HL003');
    advance(ch);
  }

  for (const open of bracketStack) {
    addDiagnostic(
      document,
      diagnostics,
      open.line,
      open.col,
      open.col + 1,
      `Unclosed '${open.value}'. Expected '${matchingCloser(open.value)}'.`,
      vscode.DiagnosticSeverity.Error,
      'HL004'
    );
  }

  return { tokens, lastSignificantToken };
}

function matchingCloser(ch) {
  return ({ '(': ')', '[': ']', '{': '}' })[ch] || '';
}

function runStructuralChecks(scan, document, diagnostics) {
  const { tokens } = scan;

  for (let i = 0; i < tokens.length; i += 1) {
    const token = tokens[i];
    if (token.type !== 'keyword') continue;

    if (token.value === 'var') {
      const next = nextMeaningful(tokens, i + 1);
      const afterNext = nextMeaningful(tokens, next ? tokens.indexOf(next) + 1 : i + 1);

      if (!next || next.type !== 'ident') {
        addDiagnostic(document, diagnostics, token.line, token.col, token.col + 3, "'var' must be followed by an identifier.", vscode.DiagnosticSeverity.Error, 'HL101');
        continue;
      }
      if (!afterNext || afterNext.type !== 'op' || afterNext.value !== '=') {
        addDiagnostic(document, diagnostics, next.line, next.col, next.endCol, "Variable declarations must use 'var name = value'.", vscode.DiagnosticSeverity.Error, 'HL102');
      }
    }

    if (token.value === 'how') {
      const next = nextMeaningful(tokens, i + 1);
      const afterNext = next ? nextMeaningful(tokens, tokens.indexOf(next) + 1) : null;
      if (!next || next.type !== 'ident') {
        addDiagnostic(document, diagnostics, token.line, token.col, token.col + 3, "'how' must be followed by a module identifier.", vscode.DiagnosticSeverity.Error, 'HL103');
        continue;
      }
      if (afterNext && afterNext.line === token.line) {
        addDiagnostic(document, diagnostics, afterNext.line, afterNext.col, afterNext.endCol, "Import syntax is 'how moduleName' with no trailing tokens on the same line.", vscode.DiagnosticSeverity.Warning, 'HL104');
      }
    }

    if (token.value === 'break') {
      if (!isInsideLoop(tokens, i)) {
        addDiagnostic(document, diagnostics, token.line, token.col, token.endCol, "'break' is usually only valid inside '(:)={...}' or range loops.", vscode.DiagnosticSeverity.Warning, 'HL105');
      }
    }
  }

  for (let i = 0; i < tokens.length; i += 1) {
    const token = tokens[i];
    if (token.type === 'op' && token.value === ':' && !looksLikeKnownColonContext(tokens, i)) {
      addDiagnostic(document, diagnostics, token.line, token.col, token.col + 1, "Suspicious ':'. In Howlang, ':' is usually used for slices, branch conditions, or map/class fields; '::' is the unconditional branch operator.", vscode.DiagnosticSeverity.Information, 'HL201');
    }
  }
}

function nextMeaningful(tokens, startIndex) {
  for (let i = startIndex; i < tokens.length; i += 1) {
    if (tokens[i].type !== 'newline') return tokens[i];
  }
  return null;
}

function looksLikeKnownColonContext(tokens, index) {
  const prev = previousMeaningful(tokens, index - 1);
  const next = nextMeaningful(tokens, index + 1);
  if (!prev || !next) return false;
  if (prev.type === 'ident' || prev.type === 'string' || prev.type === 'number' || prev.type === 'close') return true;
  if (prev.type === 'open' && prev.value === '(') return true;   // (:)
  if (next.type === 'close') return true;                         // (x:)
  return false;
}

function previousMeaningful(tokens, startIndex) {
  for (let i = startIndex; i >= 0; i -= 1) {
    if (tokens[i].type !== 'newline') return tokens[i];
  }
  return null;
}

function isInsideLoop(tokens, index) {
  for (let i = index - 1; i >= 0; i -= 1) {
    const t = tokens[i];
    const t1 = tokens[i + 1];
    const t2 = tokens[i + 2];
    const t3 = tokens[i + 3];
    if (t?.type === 'open' && t.value === '(' && t1?.type === 'op' && t1.value === ':' && t2?.type === 'close' && t2.value === ')' && t3?.type === 'op' && t3.value === '=') {
      return true;
    }
    if (t?.type === 'open' && t.value === '(') {
      // also allow (start:stop) = i { ... }
      let seenColon = false;
      let seenClose = false;
      for (let j = i + 1; j < Math.min(tokens.length, i + 12); j += 1) {
        if (tokens[j].type === 'op' && tokens[j].value === ':') seenColon = true;
        if (tokens[j].type === 'close' && tokens[j].value === ')') seenClose = true;
        if (seenClose) {
          const maybeEq = nextMeaningful(tokens, j + 1);
          const maybeVar = maybeEq ? nextMeaningful(tokens, tokens.indexOf(maybeEq) + 1) : null;
          const maybeBrace = maybeVar ? nextMeaningful(tokens, tokens.indexOf(maybeVar) + 1) : null;
          if (seenColon && maybeEq?.type === 'op' && maybeEq.value === '=' && maybeVar?.type === 'ident' && maybeBrace?.type === 'open' && maybeBrace.value === '{') {
            return true;
          }
          break;
        }
      }
    }
  }
  return false;
}

module.exports = {
  activate,
  deactivate
};
