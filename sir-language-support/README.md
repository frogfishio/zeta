# SIR Language Support (VS Code)

Syntax highlighting + basic editor support for:
- `.sir` (SIRC source)
- `.sir.jsonl` (SIR JSONL streams)

## Features

- TextMate syntax highlighting for `.sir`
- Language configuration (brackets + `;;` line comments)
- Snippets (unit header, `fn`, `extern fn`, CFG blocks, `term.*`, `sem.*`)
- JSON highlighting for `.sir.jsonl` (via embedded `source.json`)
- Command: `SIR: Verify current file` (runs `sirc` + `sircc --verify-only` and reports JSON diagnostics in Problems)

## File extensions

- `*.sir` → language id `sir`
- `*.sir.jsonl` → language id `sirjsonl`

## Diagnostics / verification

The `SIR: Verify current file` command expects `sirc` and/or `sircc` to be available on your `PATH`
(or configured via settings).

Settings:
- `sirLanguageSupport.sircPath` (default: `sirc`)
- `sirLanguageSupport.sirccPath` (default: `sircc`)
- `sirLanguageSupport.useStrictSirc` (default: `true`)
- `sirLanguageSupport.useStrictSircc` (default: `true`)

## Known limitations

- The `.sir` grammar is intentionally lightweight; it does not validate programs.
- `.sir.jsonl` uses generic JSON highlighting (no schema-aware semantics yet).

## Release Notes

### 0.0.2

- Add `.sir` and `.sir.jsonl` languages
- Add syntax highlighting, snippets, and language configuration
