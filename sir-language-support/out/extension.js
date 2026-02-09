"use strict";
var __createBinding = (this && this.__createBinding) || (Object.create ? (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    var desc = Object.getOwnPropertyDescriptor(m, k);
    if (!desc || ("get" in desc ? !m.__esModule : desc.writable || desc.configurable)) {
      desc = { enumerable: true, get: function() { return m[k]; } };
    }
    Object.defineProperty(o, k2, desc);
}) : (function(o, m, k, k2) {
    if (k2 === undefined) k2 = k;
    o[k2] = m[k];
}));
var __setModuleDefault = (this && this.__setModuleDefault) || (Object.create ? (function(o, v) {
    Object.defineProperty(o, "default", { enumerable: true, value: v });
}) : function(o, v) {
    o["default"] = v;
});
var __importStar = (this && this.__importStar) || (function () {
    var ownKeys = function(o) {
        ownKeys = Object.getOwnPropertyNames || function (o) {
            var ar = [];
            for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) ar[ar.length] = k;
            return ar;
        };
        return ownKeys(o);
    };
    return function (mod) {
        if (mod && mod.__esModule) return mod;
        var result = {};
        if (mod != null) for (var k = ownKeys(mod), i = 0; i < k.length; i++) if (k[i] !== "default") __createBinding(result, mod, k[i]);
        __setModuleDefault(result, mod);
        return result;
    };
})();
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = __importStar(require("vscode"));
const child_process = __importStar(require("node:child_process"));
const fs = __importStar(require("node:fs/promises"));
const os = __importStar(require("node:os"));
const path = __importStar(require("node:path"));
async function runTool(cmd, args, cwd) {
    return await new Promise((resolve) => {
        const child = child_process.spawn(cmd, args, { cwd, stdio: ['ignore', 'pipe', 'pipe'] });
        let stdout = '';
        let stderr = '';
        child.stdout.setEncoding('utf8');
        child.stderr.setEncoding('utf8');
        child.stdout.on('data', (d) => (stdout += d));
        child.stderr.on('data', (d) => (stderr += d));
        child.on('close', (code) => resolve({ code: code ?? 0, stdout, stderr }));
        child.on('error', () => resolve({ code: 127, stdout, stderr }));
    });
}
function asString(v) {
    return typeof v === 'string' ? v : undefined;
}
function asNumber(v) {
    return typeof v === 'number' && Number.isFinite(v) ? v : undefined;
}
function asRecord(v) {
    return typeof v === 'object' && v !== null ? v : undefined;
}
function normalizeDiag(v) {
    // Accept multiple diagnostic JSON shapes:
    // - sirc: {"k":"diag","tool":"sirc","code":"...","path":"...","line":N,"col":N,"msg":"..."}
    // - sircc: {"k":"diag","level":"error","msg":"...","code":"...","loc":{"unit":"...","line":N,"col":N}}
    // - sem: {"tool":"sem","code":"...","message":"...","path":"...","line":N,"col":N}
    const k = asString(v.k);
    const tool = asString(v.tool);
    const code = asString(v.code);
    const level = asString(v.level) ?? asString(v.severity);
    const msg = asString(v.message) ?? asString(v.msg);
    let p = asString(v.path);
    let line = asNumber(v.line);
    let col = asNumber(v.col);
    const loc = asRecord(v.loc);
    if (!p && loc)
        p = asString(loc.unit) ?? asString(loc.path) ?? asString(loc.file);
    if (line === undefined && loc)
        line = asNumber(loc.line);
    if (col === undefined && loc)
        col = asNumber(loc.col);
    if (msg && (k === 'diag' || tool || code || p || line !== undefined)) {
        return { tool, code, level, path: p, line, col, message: msg };
    }
    // If it's a JSON object but doesn't match a diag schema, ignore it.
    return undefined;
}
function parseDiagJsonLines(stderr) {
    const out = [];
    for (const line of stderr.split(/\r?\n/)) {
        const t = line.trim();
        if (!t.startsWith('{') || !t.endsWith('}'))
            continue;
        try {
            const v = JSON.parse(t);
            const rec = asRecord(v);
            if (!rec)
                continue;
            const nd = normalizeDiag(rec);
            if (nd)
                out.push(nd);
        }
        catch {
            // ignore non-json / partial lines
        }
    }
    return out;
}
function diagSeverity(level) {
    const l = (level ?? '').toLowerCase();
    if (l === 'warn' || l === 'warning')
        return vscode.DiagnosticSeverity.Warning;
    if (l === 'info' || l === 'note')
        return vscode.DiagnosticSeverity.Information;
    return vscode.DiagnosticSeverity.Error;
}
function diagToVscodeDiagnostic(d) {
    const line = typeof d.line === 'number' && d.line > 0 ? d.line - 1 : 0;
    const col = typeof d.col === 'number' && d.col > 0 ? d.col - 1 : 0;
    const range = new vscode.Range(new vscode.Position(line, col), new vscode.Position(line, col + 1));
    const code = d.code ?? 'sir.diag';
    const msg = d.message || 'diagnostic';
    const tool = d.tool ? `[${d.tool}] ` : '';
    const diag = new vscode.Diagnostic(range, `${tool}${msg}`, diagSeverity(d.level));
    diag.code = code;
    return diag;
}
function diagUriForPath(p, fallback) {
    if (!p || p === '<input>')
        return fallback;
    if (fallback.scheme === 'file') {
        const bn = path.basename(fallback.fsPath);
        if (p === bn || p.endsWith('/' + bn) || p.endsWith('\\' + bn))
            return fallback;
        if (path.isAbsolute(p))
            return vscode.Uri.file(p);
        const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        if (ws)
            return vscode.Uri.file(path.join(ws, p));
    }
    return fallback;
}
async function ensureDir(p) {
    await fs.mkdir(p, { recursive: true });
}
function isSirDoc(doc) {
    return doc.languageId === 'sir' || doc.uri.path.endsWith('.sir');
}
function isSirJsonlDoc(doc) {
    return doc.languageId === 'sirjsonl' || doc.uri.path.endsWith('.sir.jsonl');
}
async function writeDocToTemp(doc, tmpRoot) {
    const inputBase = path.basename(doc.uri.fsPath || 'untitled.sir');
    const tmpInput = path.join(tmpRoot, inputBase);
    await ensureDir(tmpRoot);
    await fs.writeFile(tmpInput, doc.getText(), 'utf8');
    return { tmpInput };
}
function activate(context) {
    const diags = vscode.languages.createDiagnosticCollection('sir');
    context.subscriptions.push(diags);
    context.subscriptions.push(vscode.commands.registerCommand('sir-language-support.clearDiagnostics', () => {
        diags.clear();
        vscode.window.setStatusBarMessage('SIR: diagnostics cleared', 1500);
    }));
    // Lightweight lint-on-save / lint-on-type for .sir documents (sirc only).
    const lintSeq = new Map();
    const lintTimers = new Map();
    const runLint = async (doc) => {
        if (!isSirDoc(doc))
            return;
        const cfg = vscode.workspace.getConfiguration('sirLanguageSupport');
        const sircPath = cfg.get('sircPath', 'sirc') || 'sirc';
        const useStrictSirc = cfg.get('useStrictSirc', true);
        const cwd = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
        const key = doc.uri.toString();
        const seq = (lintSeq.get(key) ?? 0) + 1;
        lintSeq.set(key, seq);
        // Prefer linting the on-disk file when available; otherwise fall back to a temp copy.
        let tmpRoot;
        let inputPath;
        try {
            if (doc.uri.scheme === 'file' && doc.uri.fsPath) {
                inputPath = doc.uri.fsPath;
            }
            else {
                tmpRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'sir-vscode-lint-'));
                const { tmpInput } = await writeDocToTemp(doc, tmpRoot);
                inputPath = tmpInput;
            }
            const args = ['--lint', '--diagnostics', 'json', '--all'];
            if (useStrictSirc)
                args.push('--strict');
            args.push(inputPath);
            const r = await runTool(sircPath, args, cwd);
            if ((lintSeq.get(key) ?? 0) !== seq)
                return; // stale run
            const dj = parseDiagJsonLines(r.stderr);
            const vd = dj.map(diagToVscodeDiagnostic);
            diags.set(doc.uri, vd);
        }
        finally {
            if (tmpRoot)
                await fs.rm(tmpRoot, { recursive: true, force: true });
        }
    };
    context.subscriptions.push(vscode.workspace.onDidSaveTextDocument((doc) => {
        const cfg = vscode.workspace.getConfiguration('sirLanguageSupport');
        const lintOnSave = cfg.get('lintOnSave', true);
        if (!lintOnSave)
            return;
        void runLint(doc);
    }));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument((e) => {
        const doc = e.document;
        if (!isSirDoc(doc))
            return;
        const cfg = vscode.workspace.getConfiguration('sirLanguageSupport');
        const lintOnType = cfg.get('lintOnType', false);
        if (!lintOnType)
            return;
        const debounceMs = cfg.get('lintDebounceMs', 400);
        const key = doc.uri.toString();
        const prev = lintTimers.get(key);
        if (prev)
            clearTimeout(prev);
        lintTimers.set(key, setTimeout(() => {
            void runLint(doc);
        }, Math.max(0, debounceMs)));
    }));
    context.subscriptions.push(vscode.commands.registerCommand('sir-language-support.verifyCurrentFile', async () => {
        const editor = vscode.window.activeTextEditor;
        if (!editor)
            return;
        const doc = editor.document;
        const fallbackUri = doc.uri;
        const cfg = vscode.workspace.getConfiguration('sirLanguageSupport');
        const sircPath = cfg.get('sircPath', 'sirc') || 'sirc';
        const sirccPath = cfg.get('sirccPath', 'sircc') || 'sircc';
        const useStrictSirc = cfg.get('useStrictSirc', true);
        const useStrictSircc = cfg.get('useStrictSircc', true);
        await vscode.window.withProgress({ location: vscode.ProgressLocation.Notification, title: 'SIR: Verifying…', cancellable: false }, async () => {
            const tmpRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'sir-vscode-'));
            try {
                const inputBase = path.basename(doc.uri.fsPath || 'untitled.sir');
                const tmpInput = path.join(tmpRoot, inputBase);
                const tmpJsonl = tmpInput.endsWith('.sir') ? tmpInput + '.jsonl' : tmpInput + '.sir.jsonl';
                await ensureDir(tmpRoot);
                await fs.writeFile(tmpInput, doc.getText(), 'utf8');
                // Clear old diagnostics for this document.
                diags.delete(fallbackUri);
                const collected = new Map();
                const addDiag = (uri, d) => {
                    const key = uri.toString();
                    const entry = collected.get(key) ?? { uri, diagnostics: [] };
                    entry.diagnostics.push(d);
                    collected.set(key, entry);
                };
                const isSir = isSirDoc(doc);
                const isSirJsonl = isSirJsonlDoc(doc);
                if (!isSir && !isSirJsonl) {
                    vscode.window.showWarningMessage('SIR: open a .sir or .sir.jsonl file to verify');
                    return;
                }
                const cwd = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
                if (isSir) {
                    const sircArgs = ['--diagnostics', 'json', '--all', '--emit-src', 'both', '--ids', 'stable'];
                    if (useStrictSirc)
                        sircArgs.push('--strict');
                    sircArgs.push(tmpInput, '-o', tmpJsonl);
                    const r = await runTool(sircPath, sircArgs, cwd);
                    if (r.code !== 0) {
                        for (const dj of parseDiagJsonLines(r.stderr))
                            addDiag(fallbackUri, diagToVscodeDiagnostic(dj));
                        for (const it of collected.values())
                            diags.set(it.uri, it.diagnostics);
                        vscode.window.showErrorMessage('SIR: sirc failed (see Problems)');
                        return;
                    }
                }
                // Verify JSONL with sircc.
                const jsonlPath = isSirJsonl ? tmpInput : tmpJsonl;
                const sirccArgs = ['--verify-only', '--diagnostics', 'json'];
                if (useStrictSircc)
                    sirccArgs.push('--verify-strict');
                sirccArgs.push(jsonlPath);
                const vr = await runTool(sirccPath, sirccArgs, cwd);
                const di = parseDiagJsonLines(vr.stderr);
                for (const dj of di)
                    addDiag(diagUriForPath(dj.path, fallbackUri), diagToVscodeDiagnostic(dj));
                for (const it of collected.values())
                    diags.set(it.uri, it.diagnostics);
                if (vr.code === 0) {
                    vscode.window.setStatusBarMessage('SIR: verified OK', 2000);
                }
                else if (di.length) {
                    vscode.window.showErrorMessage('SIR: verification failed (see Problems)');
                }
                else {
                    vscode.window.showErrorMessage(`SIR: sircc failed:\n${vr.stderr || vr.stdout}`.slice(0, 2000));
                }
            }
            finally {
                await fs.rm(tmpRoot, { recursive: true, force: true });
            }
        });
    }));
    context.subscriptions.push(vscode.commands.registerCommand('sir-language-support.helloWorld', () => {
        vscode.window.showInformationMessage('Hello World from SIR Language Support!');
    }));
}
// This method is called when your extension is deactivated
function deactivate() { }
//# sourceMappingURL=extension.js.map