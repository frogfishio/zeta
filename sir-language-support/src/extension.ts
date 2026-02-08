import * as vscode from 'vscode';
import * as child_process from 'node:child_process';
import * as fs from 'node:fs/promises';
import * as os from 'node:os';
import * as path from 'node:path';

type DiagJson = {
	k: string;
	tool?: string;
	code?: string;
	path?: string;
	line?: number;
	col?: number;
	msg?: string;
};

async function runTool(cmd: string, args: string[], cwd?: string): Promise<{ code: number; stdout: string; stderr: string }> {
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

function parseDiagJsonLines(stderr: string): DiagJson[] {
	const out: DiagJson[] = [];
	for (const line of stderr.split(/\r?\n/)) {
		const t = line.trim();
		if (!t.startsWith('{') || !t.endsWith('}')) continue;
		try {
			const v = JSON.parse(t) as unknown;
			if (typeof v === 'object' && v !== null) {
				const dj = v as DiagJson;
				if (dj.k === 'diag') out.push(dj);
			}
		} catch {
			// ignore non-json / partial lines
		}
	}
	return out;
}

function diagToVscodeDiagnostic(d: DiagJson): vscode.Diagnostic {
	const line = typeof d.line === 'number' && d.line > 0 ? d.line - 1 : 0;
	const col = typeof d.col === 'number' && d.col > 0 ? d.col - 1 : 0;
	const range = new vscode.Range(new vscode.Position(line, col), new vscode.Position(line, col + 1));
	const code = d.code ?? 'sir.diag';
	const msg = d.msg ?? 'diagnostic';
	const tool = d.tool ? `[${d.tool}] ` : '';
	const diag = new vscode.Diagnostic(range, `${tool}${msg}`, vscode.DiagnosticSeverity.Error);
	diag.code = code;
	return diag;
}

function diagUriForPath(p: string | undefined, fallback: vscode.Uri): vscode.Uri {
	if (!p || p === '<input>') return fallback;
	if (fallback.scheme === 'file') {
		const bn = path.basename(fallback.fsPath);
		if (p === bn || p.endsWith('/' + bn) || p.endsWith('\\' + bn)) return fallback;
		if (path.isAbsolute(p)) return vscode.Uri.file(p);
		const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
		if (ws) return vscode.Uri.file(path.join(ws, p));
	}
	return fallback;
}

async function ensureDir(p: string): Promise<void> {
	await fs.mkdir(p, { recursive: true });
}

export function activate(context: vscode.ExtensionContext) {
	const diags = vscode.languages.createDiagnosticCollection('sir');
	context.subscriptions.push(diags);

	context.subscriptions.push(
		vscode.commands.registerCommand('sir-language-support.clearDiagnostics', () => {
			diags.clear();
			vscode.window.setStatusBarMessage('SIR: diagnostics cleared', 1500);
		})
	);

	context.subscriptions.push(
		vscode.commands.registerCommand('sir-language-support.verifyCurrentFile', async () => {
			const editor = vscode.window.activeTextEditor;
			if (!editor) return;
			const doc = editor.document;
			const fallbackUri = doc.uri;

			const cfg = vscode.workspace.getConfiguration('sirLanguageSupport');
			const sircPath = cfg.get<string>('sircPath', 'sirc') || 'sirc';
			const sirccPath = cfg.get<string>('sirccPath', 'sircc') || 'sircc';
			const useStrictSirc = cfg.get<boolean>('useStrictSirc', true);
			const useStrictSircc = cfg.get<boolean>('useStrictSircc', true);

			await vscode.window.withProgress(
				{ location: vscode.ProgressLocation.Notification, title: 'SIR: Verifying…', cancellable: false },
				async () => {
					const tmpRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'sir-vscode-'));
					try {
						const inputBase = path.basename(doc.uri.fsPath || 'untitled.sir');
						const tmpInput = path.join(tmpRoot, inputBase);
						const tmpJsonl = tmpInput.endsWith('.sir') ? tmpInput + '.jsonl' : tmpInput + '.sir.jsonl';
						await ensureDir(tmpRoot);
						await fs.writeFile(tmpInput, doc.getText(), 'utf8');

						// Clear old diagnostics for this document.
						diags.delete(fallbackUri);

						const collected = new Map<string, { uri: vscode.Uri; diagnostics: vscode.Diagnostic[] }>();
						const addDiag = (uri: vscode.Uri, d: vscode.Diagnostic) => {
							const key = uri.toString();
							const entry = collected.get(key) ?? { uri, diagnostics: [] };
							entry.diagnostics.push(d);
							collected.set(key, entry);
						};

						const isSir = doc.languageId === 'sir' || doc.uri.path.endsWith('.sir');
						const isSirJsonl = doc.languageId === 'sirjsonl' || doc.uri.path.endsWith('.sir.jsonl');
						if (!isSir && !isSirJsonl) {
							vscode.window.showWarningMessage('SIR: open a .sir or .sir.jsonl file to verify');
							return;
						}

						const cwd = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;

						if (isSir) {
							const sircArgs = ['--diagnostics', 'json', '--all', '--emit-src', 'both', '--ids', 'string'];
							if (useStrictSirc) sircArgs.push('--strict');
							sircArgs.push(tmpInput, '-o', tmpJsonl);
							const r = await runTool(sircPath, sircArgs, cwd);
							if (r.code !== 0) {
								for (const dj of parseDiagJsonLines(r.stderr)) addDiag(fallbackUri, diagToVscodeDiagnostic(dj));
								for (const it of collected.values()) diags.set(it.uri, it.diagnostics);
								vscode.window.showErrorMessage('SIR: sirc failed (see Problems)');
								return;
							}
						}

						// Verify JSONL with sircc.
						const jsonlPath = isSirJsonl ? tmpInput : tmpJsonl;
						const sirccArgs = ['--verify-only', '--diagnostics', 'json'];
						if (useStrictSircc) sirccArgs.push('--verify-strict');
						sirccArgs.push(jsonlPath);
						const vr = await runTool(sirccPath, sirccArgs, cwd);

						const di = parseDiagJsonLines(vr.stderr);
						for (const dj of di) addDiag(diagUriForPath(dj.path, fallbackUri), diagToVscodeDiagnostic(dj));
						for (const it of collected.values()) diags.set(it.uri, it.diagnostics);

						if (vr.code === 0) {
							vscode.window.setStatusBarMessage('SIR: verified OK', 2000);
						} else if (di.length) {
							vscode.window.showErrorMessage('SIR: verification failed (see Problems)');
						} else {
							vscode.window.showErrorMessage(`SIR: sircc failed:\n${vr.stderr || vr.stdout}`.slice(0, 2000));
						}
					} finally {
						await fs.rm(tmpRoot, { recursive: true, force: true });
					}
				}
			);
		})
	);

	context.subscriptions.push(
		vscode.commands.registerCommand('sir-language-support.helloWorld', () => {
			vscode.window.showInformationMessage('Hello World from SIR Language Support!');
		})
	);
}

// This method is called when your extension is deactivated
export function deactivate() {}
