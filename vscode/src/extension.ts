// ========================
//
// Created By : miruku_sora2026
//
// File name : extension.ts
//
// Description :
//     Aevix 편집 기능과 신뢰된 작업 공간의 컴파일러 연동을 등록한다.
//     Register Aevix editing features and compiler integration for trusted workspaces.
//
// ========================

import * as vscode from 'vscode';
import * as path from 'node:path';
import { access, constants, realpath } from 'node:fs/promises';
import { analyzeDocument, baseType, buildIndex, builtinDocs, builtinMemberDocumentation, canAccessMember, DocumentAnalysis, importedTypeCompletions, importedTypeDocumentation, keywordDocs, keywords, namespaceAPIs, LanguageSymbol, rebuildReferences, referencesTo, scopeAt, symbolAt, tokenAt, WorkspaceIndex } from './language';
import { byteColumnToCharacter, parseCompilerDiagnostics } from './diagnostics';
import { runCompiler } from './compiler';
import { findWorkspaceCompiler } from './compilerSearch';

const selector: vscode.DocumentSelector = { language: 'aevix' };
const exclude = '**/{.git,node_modules,build,out,dist,.vscode-test,.cache}/**';
function range(document: vscode.TextDocument, start: number, end: number): vscode.Range { return new vscode.Range(document.positionAt(start), document.positionAt(end)); }
function kind(symbol: LanguageSymbol): vscode.SymbolKind {
    const kinds: Record<LanguageSymbol['kind'], vscode.SymbolKind> = {
        struct: vscode.SymbolKind.Struct, enum: vscode.SymbolKind.Enum, interface: vscode.SymbolKind.Interface,
        function: vscode.SymbolKind.Function, method: vscode.SymbolKind.Method, variable: vscode.SymbolKind.Variable,
        constant: vscode.SymbolKind.Constant, field: vscode.SymbolKind.Field, parameter: vscode.SymbolKind.Variable,
        typeParameter: vscode.SymbolKind.TypeParameter, variant: vscode.SymbolKind.EnumMember, typeAlias: vscode.SymbolKind.TypeParameter
    };
    return kinds[symbol.kind];
}
function completionKind(symbol: LanguageSymbol): vscode.CompletionItemKind {
    const kinds: Record<LanguageSymbol['kind'], vscode.CompletionItemKind> = {
        struct: vscode.CompletionItemKind.Struct, enum: vscode.CompletionItemKind.Enum, interface: vscode.CompletionItemKind.Interface,
        function: vscode.CompletionItemKind.Function, method: vscode.CompletionItemKind.Method, variable: vscode.CompletionItemKind.Variable,
        constant: vscode.CompletionItemKind.Constant, field: vscode.CompletionItemKind.Field, parameter: vscode.CompletionItemKind.Variable,
        typeParameter: vscode.CompletionItemKind.TypeParameter, variant: vscode.CompletionItemKind.EnumMember, typeAlias: vscode.CompletionItemKind.TypeParameter
    };
    return kinds[symbol.kind];
}
function markdown(symbol: LanguageSymbol): vscode.MarkdownString {
    const value = new vscode.MarkdownString(); value.appendCodeblock(symbol.signature || `${symbol.type} ${symbol.name}`, 'aevix');
    if (symbol.documentation) { value.appendText('\n' + symbol.documentation); }
    value.isTrusted = false; return value;
}
export function canRunCompiler(trusted: boolean, scheme: string, saved: boolean): boolean { return trusted && scheme === 'file' && saved; }
interface CheckJob { generation: number; timer?: NodeJS.Timeout; controller?: AbortController }
interface SourceRoots { root: string; extras: string[]; packages: Record<string, string> }

class LanguageTools implements vscode.Disposable {
    readonly diagnostics = vscode.languages.createDiagnosticCollection('aevix');
    readonly output = vscode.window.createOutputChannel('Aevix');
    readonly documents = new Map<string, DocumentAnalysis>();
    index: WorkspaceIndex = buildIndex([]);
    private readonly versions = new Map<string, number>();
    private readonly jobs = new Map<string, CheckJob>();
    private readonly checkDiagnostics = new Map<string, Map<string, vscode.Diagnostic[]>>();
    private indexGeneration = 0;
    private reportedMissing = false;
    private disposed = false;

    dispose(): void {
        this.disposed = true; ++this.indexGeneration;
        for (const job of this.jobs.values()) { if (job.timer) { clearTimeout(job.timer); } job.controller?.abort(); }
        this.jobs.clear(); this.diagnostics.dispose(); this.output.dispose();
    }
    roots(document: Pick<vscode.TextDocument, 'uri'>): SourceRoots {
        const folder = vscode.workspace.getWorkspaceFolder(document.uri);
        const base = folder?.uri.fsPath ?? path.dirname(document.uri.fsPath);
        const config = vscode.workspace.getConfiguration('aevix', document.uri);
        const resolve = (value: string) => path.resolve(base, value.replace(/\$\{workspaceFolder\}/g, base));
        let root = resolve(config.get<string>('sourceRoot', 'src'));
        // src 밖의 단독 예제는 자신의 폴더에서 독립 모듈로 검사한다.
        // Standalone examples outside src use their containing directory as a module root.
        if (path.relative(root, document.uri.fsPath).startsWith('..' + path.sep)) {
            let current = path.dirname(document.uri.fsPath); let found: string | undefined;
            while (current !== path.dirname(current)) { if (path.basename(current) === 'src') { found = current; break; } current = path.dirname(current); }
            root = found ?? path.dirname(document.uri.fsPath);
        }
        const extras = config.get<string[]>('moduleRoots', []).filter(value => typeof value === 'string').map(resolve);
        const configured = config.get<Record<string, string>>('packages', {});
        const packages: Record<string, string> = {};
        for (const [name, directory] of Object.entries(configured)) { if (/^[A-Za-z_][A-Za-z_0-9]*$/.test(name) && typeof directory === 'string') { packages[name] = resolve(directory); } }
        return { root, extras, packages };
    }
    moduleName(document: Pick<vscode.TextDocument, 'uri'>): string | undefined {
        if (document.uri.scheme !== 'file') { return undefined; }
        const roots = this.roots(document);
        for (const [prefix, root] of [...Object.entries(roots.packages), ['', roots.root] as const, ...roots.extras.map(value => ['', value] as const)]) {
            const relative = path.relative(root, document.uri.fsPath);
            if (!relative.startsWith('..' + path.sep) && !path.isAbsolute(relative) && relative.endsWith('.avx')) {
                return [prefix, relative.slice(0, -4).split(path.sep).join('.')].filter(Boolean).join('.');
            }
        }
        return undefined;
    }
    private rootBindings(): { prefix: string; root: string }[] {
        const result: { prefix: string; root: string }[] = [];
        for (const folder of vscode.workspace.workspaceFolders ?? []) {
            const config = vscode.workspace.getConfiguration('aevix', folder.uri);
            const resolve = (value: string) => path.resolve(folder.uri.fsPath, value.replace(/\$\{workspaceFolder\}/g, folder.uri.fsPath));
            result.push({ prefix: '', root: resolve(config.get<string>('sourceRoot', 'src')) });
            for (const directory of config.get<string[]>('moduleRoots', [])) { if (typeof directory === 'string') { result.push({ prefix: '', root: resolve(directory) }); } }
            for (const [prefix, directory] of Object.entries(config.get<Record<string, string>>('packages', {}))) {
                if (/^[A-Za-z_][A-Za-z_0-9]*$/.test(prefix) && typeof directory === 'string') { result.push({ prefix, root: resolve(directory) }); }
            }
        }
        return result;
    }
    private rebuildIndex(): void {
        this.index = buildIndex([...this.documents.values()]);
        for (const { prefix, root } of this.rootBindings()) {
            for (const document of this.documents.values()) {
                const uri = vscode.Uri.parse(document.uri); if (uri.scheme !== 'file') { continue; }
                const relative = path.relative(root, uri.fsPath);
                if (relative.startsWith('..' + path.sep) || path.isAbsolute(relative) || !relative.endsWith('.avx')) { continue; }
                const name = [prefix, relative.slice(0, -4).split(path.sep).join('.')].filter(Boolean).join('.');
                const group = this.index.modules.get(name) ?? [];
                if (!group.some(item => item.uri === document.uri)) { group.push(document); this.index.modules.set(name, group); }
            }
        }
        for (const document of this.documents.values()) { rebuildReferences(document, this.index); }
    }
    analysis(document: vscode.TextDocument): DocumentAnalysis {
        const uri = document.uri.toString(); let cached = this.documents.get(uri);
        if (!cached || this.versions.get(uri) !== document.version) {
            cached = analyzeDocument(uri, document.getText(), this.moduleName(document));
            this.documents.set(uri, cached); this.versions.set(uri, document.version); this.rebuildIndex();
        }
        return cached;
    }
    async refreshIndex(): Promise<void> {
        const generation = ++this.indexGeneration;
        const maximum = vscode.workspace.getConfiguration('aevix').get<number>('index.maxFiles', 2000);
        const files = await vscode.workspace.findFiles('**/*.avx', exclude, maximum);
        const paths = new Set(files.map(uri => uri.toString()));
        for (const { root } of this.rootBindings()) {
            if (files.length >= maximum || generation !== this.indexGeneration || this.disposed) { break; }
            try {
                const extra = await vscode.workspace.findFiles(new vscode.RelativePattern(root, '**/*.avx'), exclude, maximum - files.length);
                for (const uri of extra) { if (!paths.has(uri.toString())) { paths.add(uri.toString()); files.push(uri); } }
            } catch { /* 없는 선택적 루트는 컴파일러에서 진단한다. The compiler diagnoses a missing optional root. */ }
        }
        if (this.disposed || generation !== this.indexGeneration) { return; }
        const included = new Set(files.map(uri => uri.toString()));
        for (const document of vscode.workspace.textDocuments.filter(doc => doc.languageId === 'aevix')) { included.add(document.uri.toString()); this.analysis(document); }
        // 열린 문서는 메모리의 최신 내용을 사용하며 디스크 파일을 덮어쓰지 않는다.
        // Open documents supply their current text and are never overwritten on disk.
        for (let begin = 0; begin < files.length; begin += 32) {
            await Promise.all(files.slice(begin, begin + 32).map(async uri => {
                try {
                    const openDocument = vscode.workspace.textDocuments.find(doc => doc.uri.toString() === uri.toString());
                    const text = openDocument?.getText() ?? new TextDecoder('utf-8').decode(await vscode.workspace.fs.readFile(uri));
                    if (!this.disposed && generation === this.indexGeneration) {
                        this.documents.set(uri.toString(), analyzeDocument(uri.toString(), text, this.moduleName({ uri })));
                        this.versions.set(uri.toString(), openDocument?.version ?? -1);
                    }
                } catch { this.documents.delete(uri.toString()); this.versions.delete(uri.toString()); }
            }));
            if (this.disposed || generation !== this.indexGeneration) { return; }
        }
        for (const key of this.documents.keys()) { if (!included.has(key)) { this.documents.delete(key); this.versions.delete(key); } }
        this.rebuildIndex();
        if (files.length >= maximum) { this.output.appendLine(`Workspace index reached its ${maximum}-file limit. Increase aevix.index.maxFiles if needed.`); }
    }
    private cancel(uri: string): CheckJob {
        const prior = this.jobs.get(uri); if (prior?.timer) { clearTimeout(prior.timer); } prior?.controller?.abort();
        const next = { generation: (prior?.generation ?? 0) + 1 }; this.jobs.set(uri, next); return next;
    }
    close(document: vscode.TextDocument): void {
        const uri = document.uri.toString(); this.cancel(uri); this.checkDiagnostics.delete(uri); this.publishDiagnostics();
        this.documents.delete(uri); this.versions.delete(uri); this.rebuildIndex();
    }
    schedule(document: vscode.TextDocument, immediate = false): void {
        if (document.languageId !== 'aevix') { return; }
        this.analysis(document); const job = this.cancel(document.uri.toString());
        const config = vscode.workspace.getConfiguration('aevix', document.uri);
        if (!config.get<boolean>('diagnostics.enabled', true) || !canRunCompiler(vscode.workspace.isTrusted, document.uri.scheme, !document.isUntitled)) {
            this.checkDiagnostics.delete(document.uri.toString()); this.publishDiagnostics(); return;
        }
        job.timer = setTimeout(() => { void this.check(document, false, job); }, immediate ? 0 : config.get<number>('diagnostics.delay', 400));
    }
    async compiler(document: vscode.TextDocument): Promise<string> {
        const config = vscode.workspace.getConfiguration('aevix', document.uri); const chosen = config.get<string>('compilerPath', '').trim();
        const folder = vscode.workspace.getWorkspaceFolder(document.uri)?.uri.fsPath ?? path.dirname(document.uri.fsPath);
        if (chosen) {
            const substituted = chosen.replace(/\$\{workspaceFolder\}/g, folder);
            return substituted.includes('/') || substituted.includes('\\') ? path.resolve(folder, substituted) : substituted;
        }
        return findWorkspaceCompiler([folder, ...vscode.workspace.workspaceFolders?.map(item => item.uri.fsPath) ?? []]);
    }
    args(document: vscode.TextDocument): string[] {
        const roots = this.roots(document); const args = ['--source-root', roots.root];
        for (const extra of roots.extras) { args.push('--module-root', extra); }
        for (const [name, root] of Object.entries(roots.packages)) { args.push('--package', `${name}=${root}`); }
        return args;
    }
    private publishDiagnostics(): void {
        if (this.disposed) { return; }
        const combined = new Map<string, vscode.Diagnostic[]>();
        for (const owner of this.checkDiagnostics.values()) {
            for (const [uri, diagnostics] of owner) {
                const entries = combined.get(uri) ?? [];
                for (const diagnostic of diagnostics) { if (!entries.some(d => d.message === diagnostic.message && d.range.isEqual(diagnostic.range) && d.severity === diagnostic.severity)) { entries.push(diagnostic); } }
                combined.set(uri, entries);
            }
        }
        this.diagnostics.clear(); this.diagnostics.set([...combined].map(([uri, diagnostics]) => [vscode.Uri.parse(uri), diagnostics]));
    }
    async check(document: vscode.TextDocument, explicit = false, scheduled?: CheckJob): Promise<void> {
        if (!canRunCompiler(vscode.workspace.isTrusted, document.uri.scheme, !document.isUntitled)) {
            if (explicit) { void vscode.window.showInformationMessage(vscode.workspace.isTrusted ? 'Save this Aevix file to a local .avx path before checking it.' : 'Trust this workspace to run the Aevix compiler. Editing and navigation remain available.'); }
            return;
        }
        try { await access(document.uri.fsPath, constants.F_OK); }
        catch { if (explicit) { void vscode.window.showInformationMessage('Save this Aevix file before checking it.'); } return; }
        const uri = document.uri.toString(); const job = scheduled ?? this.cancel(uri); const version = document.version;
        if (this.jobs.get(uri) !== job) { return; }
        job.controller = new AbortController(); const executable = await this.compiler(document);
        if (job.controller.signal.aborted || !vscode.workspace.isTrusted || this.disposed) { return; }
        const checked = await runCompiler({ executable, args: ['--check', '--stdin', document.uri.fsPath, ...this.args(document)], input: document.getText(), cwd: vscode.workspace.getWorkspaceFolder(document.uri)?.uri.fsPath ?? path.dirname(document.uri.fsPath), timeoutMs: vscode.workspace.getConfiguration('aevix', document.uri).get<number>('diagnostics.timeout', 15000), signal: job.controller.signal });
        if (this.disposed || checked.cancelled || this.jobs.get(uri) !== job || document.version !== version || !vscode.workspace.isTrusted) { return; }
        if (checked.error || checked.timedOut || checked.outputLimited) {
            const message = checked.error ? `Could not start ${executable}. Set aevix.compilerPath to an Aevix compiler. ${checked.error.message}` : checked.timedOut ? 'Aevix checking exceeded its time limit. Increase aevix.diagnostics.timeout if needed.' : 'Aevix compiler output exceeded the 1 MiB limit.';
            this.output.appendLine(message); this.checkDiagnostics.delete(uri); this.publishDiagnostics();
            if (explicit || !this.reportedMissing) { this.reportedMissing = true; void vscode.window.showWarningMessage(message, 'Show Output').then(action => { if (action) { this.output.show(); } }); }
            return;
        }
        this.reportedMissing = false;
        const diagnostics = parseCompilerDiagnostics(checked.stderr + '\n' + checked.stdout); const grouped = new Map<string, vscode.Diagnostic[]>();
        const canonicalDocument = await realpath(document.uri.fsPath).catch(() => path.resolve(document.uri.fsPath));
        for (const parsed of diagnostics) {
            let target = document;
            if (parsed.path) {
                const parsedPath = path.resolve(path.dirname(document.uri.fsPath), parsed.path);
                const canonicalTarget = await realpath(parsedPath).catch(() => parsedPath);
                if (canonicalTarget !== canonicalDocument) {
                    const alreadyOpen = vscode.workspace.textDocuments.find(doc => doc.uri.scheme === 'file' && path.resolve(doc.uri.fsPath) === parsedPath);
                    try { target = alreadyOpen ?? await vscode.workspace.openTextDocument(vscode.Uri.file(parsedPath)); } catch { target = document; }
                }
            }
            const line = Math.min(parsed.line, Math.max(0, target.lineCount - 1)); const text = target.lineAt(line).text;
            const character = byteColumnToCharacter(text, parsed.byteColumn); const end = Math.min(text.length, character + (text.codePointAt(character)! > 0xffff ? 2 : 1));
            const item = new vscode.Diagnostic(new vscode.Range(line, character, line, end), parsed.message, parsed.severity === 'error' ? vscode.DiagnosticSeverity.Error : parsed.severity === 'warning' ? vscode.DiagnosticSeverity.Warning : vscode.DiagnosticSeverity.Information);
            item.source = 'aevixc'; const key = target.uri.toString(); const entries = grouped.get(key) ?? []; entries.push(item); grouped.set(key, entries);
        }
        if (this.disposed || this.jobs.get(uri) !== job || document.version !== version || !vscode.workspace.isTrusted) { return; }
        if (checked.exitCode !== 0 && !diagnostics.length) {
            const item = new vscode.Diagnostic(new vscode.Range(0, 0, 0, 0), checked.stderr.trim() || `Aevix compiler exited with code ${checked.exitCode}.`, vscode.DiagnosticSeverity.Error); item.source = 'aevixc'; grouped.set(uri, [item]);
        }
        this.checkDiagnostics.set(uri, grouped); this.publishDiagnostics();
        if (explicit && checked.exitCode === 0) { void vscode.window.showInformationMessage('Aevix check succeeded.'); }
    }
    async task(mode: 'build' | 'run'): Promise<void> {
        const document = vscode.window.activeTextEditor?.document;
        if (!document || document.languageId !== 'aevix') { void vscode.window.showInformationMessage('Open an Aevix file first.'); return; }
        if (!canRunCompiler(vscode.workspace.isTrusted, document.uri.scheme, !document.isUntitled)) { void vscode.window.showInformationMessage('Build and run require a trusted workspace and a saved local Aevix file.'); return; }
        if (document.isDirty) { void vscode.window.showInformationMessage('Save the file before building or running it. The current text has not been written to disk.'); return; }
        const executable = await this.compiler(document); if (!vscode.workspace.isTrusted || this.disposed) { return; }
        const executionMode = mode === 'run' && vscode.workspace.getConfiguration('aevix', document.uri).get<string>('executionMode', 'aot') === 'jit' ? 'jit' : mode;
        const args = [`--${executionMode}`, document.uri.fsPath, ...this.args(document)];
        const folder = vscode.workspace.getWorkspaceFolder(document.uri); const cwd = folder?.uri.fsPath ?? path.dirname(document.uri.fsPath);
        if (mode === 'build') {
            const directory = vscode.Uri.file(path.join(cwd, 'build', 'aevix'));
            await vscode.workspace.fs.createDirectory(directory);
            args.push('-o', path.join(directory.fsPath, path.basename(document.uri.fsPath, '.avx')));
        }
        if (!vscode.workspace.isTrusted || this.disposed) { return; }
        // ProcessExecution에 인수를 분리해 공백과 셸 기호를 파일 이름으로 그대로 전달한다.
        // ProcessExecution passes separate arguments, preserving spaces and shell characters in paths.
        const execution = new vscode.ProcessExecution(executable, args, { cwd });
        const task = new vscode.Task({ type: 'aevix', mode }, folder ?? vscode.TaskScope.Global, `Aevix ${mode}`, 'aevix', execution, []);
        task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, panel: vscode.TaskPanelKind.Dedicated, clear: false };
        await vscode.tasks.executeTask(task);
    }
}

export function activate(context: vscode.ExtensionContext): { refreshIndex(): Promise<void> } {
    const tools = new LanguageTools(); context.subscriptions.push(tools);
    const open = (document: vscode.TextDocument) => { if (document.languageId === 'aevix') { tools.schedule(document); } };
    context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(open), vscode.workspace.onDidChangeTextDocument(event => open(event.document)), vscode.workspace.onDidSaveTextDocument(open), vscode.workspace.onDidCloseTextDocument(document => { if (document.languageId === 'aevix') { tools.close(document); } }));
    context.subscriptions.push(vscode.workspace.onDidGrantWorkspaceTrust(() => { for (const document of vscode.workspace.textDocuments) { open(document); } }));
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(event => {
        if (event.affectsConfiguration('aevix')) { void tools.refreshIndex(); for (const document of vscode.workspace.textDocuments) { open(document); } }
    }));
    const watcher = vscode.workspace.createFileSystemWatcher('**/*.avx');
    context.subscriptions.push(watcher, watcher.onDidCreate(() => { void tools.refreshIndex(); }), watcher.onDidDelete(() => { void tools.refreshIndex(); }), watcher.onDidChange(() => { void tools.refreshIndex(); }));
    context.subscriptions.push(
        vscode.commands.registerCommand('aevix.check', () => { const document = vscode.window.activeTextEditor?.document; if (document?.languageId === 'aevix') { return tools.check(document, true); } return undefined; }),
        vscode.commands.registerCommand('aevix.build', () => tools.task('build')),
        vscode.commands.registerCommand('aevix.run', () => tools.task('run')),
        vscode.commands.registerCommand('aevix.restart', async () => { await tools.refreshIndex(); for (const document of vscode.workspace.textDocuments) { tools.schedule(document, true); } })
    );
    context.subscriptions.push(vscode.languages.registerHoverProvider(selector, {
        provideHover(document, position) {
            const analysis = tools.analysis(document); const token = tokenAt(analysis, document.offsetAt(position)); if (!token || token.kind !== 'identifier') { return undefined; }
            const symbol = symbolAt(analysis, token.start, tools.index);
            if (symbol) { return new vscode.Hover(markdown(symbol), range(document, token.start, token.end)); }
            const description = builtinMemberDocumentation(analysis, token.start) ?? importedTypeDocumentation(analysis, token.start) ?? builtinDocs[token.text] ?? keywordDocs[token.text];
            return description ? new vscode.Hover(new vscode.MarkdownString().appendCodeblock(token.text, 'aevix').appendText(description), range(document, token.start, token.end)) : undefined;
        }
    }));
    context.subscriptions.push(vscode.languages.registerCompletionItemProvider(selector, {
        provideCompletionItems(document, position) {
            const analysis = tools.analysis(document); const offset = document.offsetAt(position); const token = tokenAt(analysis, Math.max(0, offset - 1));
            if (token && ['comment', 'string', 'number'].includes(token.kind)) {
                if (token.kind === 'comment' && /@\w*$/.test(document.getText(range(document, token.start, offset)))) {
                    return ['parameter', 'return', 'version', 'deprecated', 'deprecated_at'].map(name => new vscode.CompletionItem(name, vscode.CompletionItemKind.Keyword));
                }
                return [];
            }
            const line = document.lineAt(position.line).text.slice(0, position.character); const importing = /^\s*(?:public\s+)?import\s+([A-Za-z_0-9.]*)$/.exec(line);
            if (importing) {
                const prefix = importing[1]; const segment = prefix.lastIndexOf('.') + 1;
                return [...tools.index.modules.keys()].filter(name => name.startsWith(prefix)).map(name => { const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Module); item.insertText = name.slice(segment); return item; });
            }
            const selections = /^\s*(?:public\s+)?import\s+([A-Za-z_][\w.]*)\s*\{[^}]*$/.exec(line);
            if (selections) {
                return (tools.index.modules.get(selections[1]) ?? []).flatMap(doc => doc.symbols.filter(symbol => symbol.scope === 0 && symbol.public).map(symbol => { const item = new vscode.CompletionItem(symbol.name, completionKind(symbol)); item.detail = symbol.signature; item.documentation = markdown(symbol); return item; }));
            }
            const qualified = /([A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*)\.([A-Za-z_0-9]*)$/.exec(line);
            if (qualified) {
                const imported = analysis.imports.find(item => item.path === qualified[1]);
                if (imported) {
                    return (tools.index.modules.get(imported.path) ?? []).flatMap(doc => doc.symbols.filter(symbol => symbol.scope === 0 && symbol.public && (imported.names === undefined || imported.wildcard || imported.names.includes(symbol.name))).map(symbol => { const item = new vscode.CompletionItem(symbol.name, completionKind(symbol)); item.detail = symbol.signature; item.documentation = markdown(symbol); return item; }));
                }
            }
            const member = /([A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*)\.([A-Za-z_0-9]*)$/.exec(line);
            if (member) {
                const available = namespaceAPIs(analysis, member[1]);
                if (available) {
                    return Object.entries(available).map(([name, description]) => {
                        const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
                        item.documentation = description;
                        return item;
                    });
                }
                const receiverOffset = document.offsetAt(new vscode.Position(position.line, position.character - member[0].length));
                const receiver = symbolAt(analysis, receiverOffset, tools.index);
                const typeName = receiver ? ['struct', 'enum', 'interface'].includes(receiver.kind) ? receiver.name : baseType(receiver.type) : member[1];
                const candidates = [...tools.index.documents.values()].flatMap(doc => doc.symbols.filter(symbol => symbol.name === typeName && ['struct', 'enum', 'interface'].includes(symbol.kind)).map(symbol => ({ doc, symbol })));
                return candidates.flatMap(({ doc, symbol }) => doc.symbols.filter(child => child.scope === symbol.memberScope && canAccessMember(doc, child, analysis, offset)).map(child => { const item = new vscode.CompletionItem(child.name, completionKind(child)); item.detail = child.signature; item.documentation = markdown(child); return item; }));
            }
            const suggestions: vscode.CompletionItem[] = [];
            for (const [name, description] of Object.entries(builtinDocs)) { const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.TypeParameter); item.documentation = description; suggestions.push(item); }
            for (const { name, description } of importedTypeCompletions(analysis)) { const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.TypeParameter); item.documentation = description; suggestions.push(item); }
            for (const name of keywords) { const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Keyword); item.documentation = keywordDocs[name]; suggestions.push(item); }
            const visibleScopes = new Set<number>(); let current: number | undefined = scopeAt(analysis, offset);
            while (current !== undefined) { visibleScopes.add(current); current = analysis.scopes[current].parent; }
            const symbols = analysis.symbols.filter(symbol => visibleScopes.has(symbol.scope) && (symbol.kind !== 'variable' || symbol.start <= offset));
            for (const imported of analysis.imports) {
                for (const doc of tools.index.modules.get(imported.path) ?? []) { symbols.push(...doc.symbols.filter(symbol => symbol.scope === 0 && symbol.public && (imported.wildcard || imported.names?.includes(symbol.name)))); }
            }
            for (const symbol of symbols) { const item = new vscode.CompletionItem(symbol.name, completionKind(symbol)); item.detail = symbol.signature; item.documentation = markdown(symbol); suggestions.push(item); }
            return suggestions;
        }
    }, '.', '@'));
    context.subscriptions.push(vscode.languages.registerSignatureHelpProvider(selector, {
        provideSignatureHelp(document, position) {
            const analysis = tools.analysis(document); const offset = document.offsetAt(position);
            const before = analysis.code.filter(token => token.start < offset);
            let nesting = 0; let opening = -1; let activeParameter = 0;
            for (let i = before.length - 1; i >= 0; --i) {
                if ([')', ']', '}'].includes(before[i].text)) { ++nesting; }
                else if (['(', '[', '{'].includes(before[i].text)) {
                    if (nesting > 0) { --nesting; }
                    else if (before[i].text === '(') { opening = i; break; }
                } else if (before[i].text === ',' && nesting === 0) { ++activeParameter; }
                else if (before[i].text === ';' && nesting === 0) { break; }
            }
            if (opening <= 0) { return undefined; }
            let callee = opening - 1;
            if (before[callee].text === '>' || before[callee].text === '>>') {
                let depth = before[callee].text.length;
                while (--callee >= 0) { if (before[callee].text === '>') { ++depth; } else if (before[callee].text === '<' && --depth === 0) { --callee; break; } }
            }
            if (callee < 0) { return undefined; }
            const ids = analysis.references.get(before[callee].start) ?? [];
            const candidates = [...tools.index.documents.values()].flatMap(doc => doc.symbols.filter(symbol => ids.includes(symbol.id) && ['function', 'method'].includes(symbol.kind)).map(symbol => ({ doc, symbol })));
            if (!candidates.length) { return undefined; }
            const help = new vscode.SignatureHelp(); help.activeParameter = activeParameter; help.activeSignature = 0;
            help.signatures = candidates.map(({ doc, symbol }) => {
                const signature = new vscode.SignatureInformation(symbol.signature, symbol.documentation);
                signature.parameters = doc.symbols.filter(param => param.kind === 'parameter' && param.scope === symbol.memberScope && param.name !== 'self').map(param => new vscode.ParameterInformation(`${param.type} ${param.name}`));
                return signature;
            });
            return help;
        }
    }, '(', ','));
    context.subscriptions.push(vscode.languages.registerDocumentLinkProvider(selector, {
        provideDocumentLinks(document) {
            const analysis = tools.analysis(document); const links: vscode.DocumentLink[] = [];
            for (const imported of analysis.imports) {
                const targets = tools.index.modules.get(imported.path); if (targets?.length !== 1) { continue; }
                const pathStart = document.getText().indexOf(imported.path, imported.start);
                if (pathStart >= imported.start && pathStart < imported.end) { links.push(new vscode.DocumentLink(range(document, pathStart, pathStart + imported.path.length), vscode.Uri.parse(targets[0].uri))); }
            }
            return links;
        }
    }));
    context.subscriptions.push(vscode.languages.registerDocumentSymbolProvider(selector, {
        provideDocumentSymbols(document) {
            const analysis = tools.analysis(document); const nodes = new Map<string, vscode.DocumentSymbol>(); const roots: vscode.DocumentSymbol[] = [];
            for (const symbol of analysis.symbols.filter(s => !['parameter', 'typeParameter'].includes(s.kind))) {
                const item = new vscode.DocumentSymbol(symbol.name, symbol.type, kind(symbol), range(document, symbol.declarationStart, symbol.declarationEnd), range(document, symbol.start, symbol.end));
                nodes.set(symbol.id, item);
            }
            for (const symbol of analysis.symbols) {
                const item = nodes.get(symbol.id); if (!item) { continue; }
                const owner = symbol.owner ?? analysis.scopes[symbol.scope].owner; const parent = owner ? nodes.get(owner) : undefined;
                if (parent && parent.range.contains(item.range)) { parent.children.push(item); } else { roots.push(item); }
            }
            return roots;
        }
    }));
    context.subscriptions.push(vscode.languages.registerWorkspaceSymbolProvider({
        async provideWorkspaceSymbols(query, cancellation) {
            await tools.refreshIndex(); if (cancellation.isCancellationRequested) { return []; }
            const found: vscode.SymbolInformation[] = [];
            for (const doc of tools.index.documents.values()) {
                const document = await vscode.workspace.openTextDocument(vscode.Uri.parse(doc.uri));
                for (const symbol of doc.symbols) {
                    if (symbol.scope === 0 && symbol.name.toLowerCase().includes(query.toLowerCase())) { found.push(new vscode.SymbolInformation(symbol.name, kind(symbol), doc.moduleName ?? '', new vscode.Location(document.uri, range(document, symbol.start, symbol.end)))); }
                }
            }
            return found;
        }
    }));
    context.subscriptions.push(vscode.languages.registerDefinitionProvider(selector, {
        async provideDefinition(document, position) {
            const analysis = tools.analysis(document); const symbol = symbolAt(analysis, document.offsetAt(position), tools.index);
            if (!symbol) { return undefined; }
            const target = await vscode.workspace.openTextDocument(vscode.Uri.parse(symbol.uri)); return new vscode.Location(target.uri, range(target, symbol.start, symbol.end));
        }
    }));
    context.subscriptions.push(vscode.languages.registerReferenceProvider(selector, {
        async provideReferences(document, position, options, cancellation) {
            const analysis = tools.analysis(document); const symbol = symbolAt(analysis, document.offsetAt(position), tools.index); if (!symbol) { return []; }
            const output: vscode.Location[] = [];
            for (const reference of referencesTo(tools.index, symbol.id, options.includeDeclaration)) {
                if (cancellation.isCancellationRequested) { return []; }
                const target = await vscode.workspace.openTextDocument(vscode.Uri.parse(reference.uri)); output.push(new vscode.Location(target.uri, range(target, reference.start, reference.end)));
            }
            return output;
        }
    }));
    context.subscriptions.push(vscode.languages.registerRenameProvider(selector, {
        prepareRename(document, position) {
            const analysis = tools.analysis(document); const token = tokenAt(analysis, document.offsetAt(position)); const symbol = symbolAt(analysis, document.offsetAt(position), tools.index);
            if (!token || !symbol || ['field', 'method', 'variant', 'parameter'].includes(symbol.kind) || symbol.scope === 0 || symbol.uri !== document.uri.toString()) { throw new Error('Rename is available for unambiguous local bindings and type parameters. Public APIs and members require compiler-backed workspace refactoring.'); }
            return { range: range(document, token.start, token.end), placeholder: symbol.name };
        },
        async provideRenameEdits(document, position, newName) {
            if (!/^[A-Za-z_][A-Za-z_0-9]*$/.test(newName) || (keywords as readonly string[]).includes(newName) || builtinDocs[newName]) { throw new Error('Use an Aevix identifier that is not a keyword or built-in type.'); }
            const analysis = tools.analysis(document); const symbol = symbolAt(analysis, document.offsetAt(position), tools.index); if (!symbol || symbol.scope === 0 || symbol.uri !== document.uri.toString() || ['field', 'method', 'variant', 'parameter'].includes(symbol.kind)) { throw new Error('This symbol cannot be renamed safely by the lexical index.'); }
            for (const other of analysis.symbols) {
                if (other.id === symbol.id || other.name !== newName) { continue; }
                const a = analysis.scopes[other.scope], b = analysis.scopes[symbol.scope];
                if (a.start < b.end && b.start < a.end) { throw new Error('The new name would collide with another declaration or create name hiding.'); }
            }
            if (analysis.imports.some(item => item.wildcard || item.names?.includes(newName))) { throw new Error('The new name may collide with an imported declaration.'); }
            const edit = new vscode.WorkspaceEdit();
            for (const reference of referencesTo(buildIndex([analysis]), symbol.id, true)) { edit.replace(document.uri, range(document, reference.start, reference.end), newName); }
            return edit;
        }
    }));
    context.subscriptions.push(vscode.languages.registerDocumentHighlightProvider(selector, {
        provideDocumentHighlights(document, position) {
            const analysis = tools.analysis(document); const symbol = symbolAt(analysis, document.offsetAt(position), tools.index); if (!symbol) { return []; }
            return referencesTo(tools.index, symbol.id).filter(ref => ref.uri === analysis.uri).map(ref => new vscode.DocumentHighlight(range(document, ref.start, ref.end)));
        }
    }));
    context.subscriptions.push(vscode.languages.registerFoldingRangeProvider(selector, {
        provideFoldingRanges(document) {
            const analysis = tools.analysis(document); const ranges: vscode.FoldingRange[] = [];
            for (const scope of analysis.scopes.slice(1)) {
                const begin = document.positionAt(Math.max(0, scope.start - 1)).line; const end = document.positionAt(Math.min(document.getText().length, scope.end)).line;
                if (end > begin) { ranges.push(new vscode.FoldingRange(begin, end - 1)); }
            }
            for (const token of analysis.tokens.filter(t => t.kind === 'comment')) {
                const begin = document.positionAt(token.start).line, end = document.positionAt(token.end).line;
                if (end > begin) { ranges.push(new vscode.FoldingRange(begin, end, vscode.FoldingRangeKind.Comment)); }
            }
            if (analysis.imports.length > 1) { ranges.push(new vscode.FoldingRange(document.positionAt(analysis.imports[0].start).line, document.positionAt(analysis.imports.at(-1)!.end).line, vscode.FoldingRangeKind.Imports)); }
            return ranges;
        }
    }));
    for (const document of vscode.workspace.textDocuments) { open(document); }
    void tools.refreshIndex();
    return { refreshIndex: () => tools.refreshIndex() };
}
export function deactivate(): void { /* 리소스는 context에서 해제한다. The extension context disposes resources. */ }
