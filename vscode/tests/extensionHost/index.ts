// ========================
//
// Created By : miruku_sora2026
//
// File name : index.ts
//
// Description :
//     실제 VS Code에서 확장의 활성화, 편집 기능과 컴파일러 진단을 확인한다.
//     Verify activation, editor features, and compiler diagnostics in actual VS Code.
//
// ========================

import assert from 'node:assert/strict';
import path from 'node:path';
import fs from 'node:fs';
import * as vscode from 'vscode';

async function waitFor(predicate: () => boolean, label: string, timeout = 20000): Promise<void> {
    const deadline = Date.now() + timeout;
    while (!predicate()) {
        if (Date.now() > deadline) throw new Error(`Timed out: ${label}`);
        await new Promise((resolve) => setTimeout(resolve, 100));
    }
}

export async function run(): Promise<void> {
    const extension = vscode.extensions.getExtension('miruku-sora2026.aevix');
    assert.ok(extension, 'Aevix extension is discoverable');
    const folder = vscode.workspace.workspaceFolders?.[0];
    assert.ok(folder, 'isolated workspace exists');
    const uri = vscode.Uri.joinPath(folder.uri, 'src/main.avx');
    const document = await vscode.workspace.openTextDocument(uri);
    await vscode.window.showTextDocument(document);
    assert.equal(document.languageId, 'aevix', '.avx language association');
    const api = await extension.activate() as { refreshIndex(): Promise<void> };
    await api.refreshIndex();
    const runtime = require(path.join(extension.extensionPath, 'out/src/extension.js')) as { canRunCompiler(trusted: boolean, scheme: string, saved: boolean): boolean };
    for (const trusted of [false, true]) for (const scheme of ['file', 'untitled', 'vscode-vfs']) for (const saved of [false, true]) {
        assert.equal(runtime.canRunCompiler(trusted, scheme, saved), trusted && scheme === 'file' && saved);
    }
    assert.ok(extension.isActive, 'Aevix extension activates');
    const commands = await vscode.commands.getCommands(true);
    for (const command of ['aevix.check', 'aevix.build', 'aevix.run', 'aevix.restart']) {
        assert.ok(commands.includes(command), `${command} registered`);
    }
    const symbols = await vscode.commands.executeCommand<vscode.DocumentSymbol[]>(
        'vscode.executeDocumentSymbolProvider', uri);
    assert.ok(symbols?.some((symbol) => symbol.name === 'main'), 'main document symbol');
    const completions = await vscode.commands.executeCommand<vscode.CompletionList>(
        'vscode.executeCompletionItemProvider', uri, new vscode.Position(0, 0));
    assert.ok(completions?.items.some((item) => typeof item.label === 'string' ? item.label === 'i32' : item.label.label === 'i32'), 'builtin completion');
    const hover = await vscode.commands.executeCommand<vscode.Hover[]>(
        'vscode.executeHoverProvider', uri, new vscode.Position(0, 1));
    assert.ok(hover?.length, 'builtin hover');
    const completionLabel = (item: vscode.CompletionItem): string => typeof item.label === 'string' ? item.label : item.label.label;
    for (const name of ['CollectionError', 'InputError', 'RangeError', 'Vector', 'Range', 'dyn']) {
        assert.ok(completions?.items.some((item) => completionLabel(item) === name), `${name} completion`);
    }
    // 설치된 스니펫을 실제 편집기에 삽입하고 가변 매개변수의 심볼과 호출 도움말을 확인한다.
    // Insert the installed snippet into the editor and inspect variadic symbols and signature help.
    const clear = new vscode.WorkspaceEdit();
    clear.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), '');
    assert.ok(await vscode.workspace.applyEdit(clear));
    vscode.window.activeTextEditor!.selection = new vscode.Selection(0, 0, 0, 0);
    const installedSnippets = JSON.parse(fs.readFileSync(path.join(extension.extensionPath, 'snippets/aevix.json'), 'utf8')) as Record<string, { body: string | string[] }>;
    const variadicBody = installedSnippets['Variadic function'].body;
    const snippetEditor = await vscode.window.showTextDocument(document, { preview: false, preserveFocus: false });
    assert.ok(await snippetEditor.insertSnippet(new vscode.SnippetString(Array.isArray(variadicBody) ? variadicBody.join('\n') : variadicBody), new vscode.Position(0, 0)), 'VS Code accepts installed snippet');
    if (commands.includes('leaveSnippet')) await vscode.commands.executeCommand('leaveSnippet');
    await waitFor(() => document.getText().includes('values[]'), 'installed variadic snippet insertion');
    assert.match(document.getText(), /usize\s+count\s*\(\s*i32\s+values\[\]\)/, `installed variadic snippet is inserted: ${JSON.stringify(document.getText())}`);
    const appendCall = new vscode.WorkspaceEdit();
    appendCall.insert(uri, document.positionAt(document.getText().length), '\ni32 main() { usize length = count(1, 2, 3); return 0; }\n');
    assert.ok(await vscode.workspace.applyEdit(appendCall));
    const variadicSymbols = await vscode.commands.executeCommand<vscode.DocumentSymbol[]>('vscode.executeDocumentSymbolProvider', uri);
    assert.ok(variadicSymbols?.some((symbol) => symbol.name === 'count'), 'variadic function is indexed');
    const variadicHelp = await vscode.commands.executeCommand<vscode.SignatureHelp>('vscode.executeSignatureHelpProvider', uri,
        document.positionAt(document.getText().lastIndexOf('count(1, 2') + 'count(1, '.length), ',');
    assert.ok(variadicHelp?.signatures.some((signature) => /values\[\]/.test(signature.label)), 'variadic signature retains declarator spelling');
    await document.save();
    await vscode.commands.executeCommand('aevix.check');
    assert.equal(vscode.languages.getDiagnostics(uri).length, 0, 'installed variadic snippet passes the real compiler');
    const promotedSource = `interface Readable { public i32 read(&Self self); };
struct Reading implements Readable {
    public i32 read(&Reading self) { return 7; }
    public T identity<T>(&Reading self, T value) { return value; }
};
InputError<i32> recover_input() { return InputError<i32>.SizeOverflow { input = 4 }; }
RangeError invalid_step() { return RangeError.CountOverflow; }
Result<i32, CollectionError> promoted() {
    i32 values[] = [1, 2, 3]?;
    Range<i32> span = Range.new(0, 3);
    mut i32 total = 0;
    for (i32 index in span) { total += values[index]; }
    Reading reading = Reading {};
    &dyn Readable view = &reading;
    i32 observed = view.read();
    return Result.ok(total + reading.identity(observed));
}
i32 main() { return 42; }
`;
    const promoted = new vscode.WorkspaceEdit();
    promoted.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), promotedSource);
    assert.ok(await vscode.workspace.applyEdit(promoted));
    assert.equal(document.getText(), promotedSource, 'promoted editor source is exact after snippet session');
    await document.save();
    await api.refreshIndex();
    for (const [name, expected] of [
        ['CollectionError', /AllocationFailure.*SizeOverflow/],
        ['InputError', /preserves.*input owner/],
        ['RangeError', /InvalidStep.*CountOverflow/],
        ['Vector', /CollectionError Result/],
        ['dyn', /dynamic interface view/],
    ] as const) {
        // Vector는 타입 별칭 이름이 아니라 동적 선언의 문서에서 별도로 조회한다.
        // Query Vector on a temporary type annotation instead of claiming inferred semantic hover.
        const source = name === 'Vector' ? promotedSource.replace('i32 values[]', 'Vector<i32> values') : promotedSource;
        if (name === 'Vector') {
            const explicit = new vscode.WorkspaceEdit();
            explicit.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), source);
            assert.ok(await vscode.workspace.applyEdit(explicit));
        }
        const at = source.indexOf(name);
        assert.ok(at >= 0, `${name} hover token exists`);
        const result = await vscode.commands.executeCommand<vscode.Hover[]>('vscode.executeHoverProvider', uri, document.positionAt(at + 1));
        const text = result?.flatMap((item) => item.contents.map((content) => typeof content === 'string' ? content : content.value)).join('\n') ?? '';
        const normalized = text.replace(/&nbsp;|&#160;|\u00a0/g, ' ');
        assert.match(normalized, expected, `${name} hover carries the new minimum contract: ${JSON.stringify(text)}`);
        if (name === 'Vector') {
            const restore = new vscode.WorkspaceEdit();
            restore.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), promotedSource);
            assert.ok(await vscode.workspace.applyEdit(restore));
        }
    }
    await vscode.commands.executeCommand('aevix.check');
    assert.equal(vscode.languages.getDiagnostics(uri).length, 0, 'Vector Result, Range, dyn and generic method source passes the real compiler');
    const promotedFailure = new vscode.WorkspaceEdit();
    promotedFailure.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)),
        promotedSource.replace('i32 values[] = [1, 2, 3]?;', 'i32 values[] = [1, 2, 3];'));
    assert.ok(await vscode.workspace.applyEdit(promotedFailure));
    await vscode.commands.executeCommand('aevix.check');
    assert.ok(vscode.languages.getDiagnostics(uri).some((diagnostic) => diagnostic.severity === vscode.DiagnosticSeverity.Error), 'missing allocation Result handling diagnoses unsaved source');
    const definitionsSource = 'import helper { Point };\nimport pixel.image { Color };\nimport tool { twice };\ni32 answer(i32 value, i32 step = 2) { return value + step; }\ni32 main() { Point point = Point { x = 1 }; Color color = Color { value = 1 }; return answer(40, 2); }\n';
    const edit = new vscode.WorkspaceEdit();
    edit.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), definitionsSource);
    assert.ok(await vscode.workspace.applyEdit(edit));
    await document.save();
    const definitions = await vscode.commands.executeCommand<Array<vscode.Location | vscode.LocationLink>>(
        'vscode.executeDefinitionProvider', uri, document.positionAt(definitionsSource.lastIndexOf('answer(') + 2));
    assert.ok(definitions?.length, 'local function definition');
    await api.refreshIndex();
    const links = await vscode.commands.executeCommand<vscode.DocumentLink[]>('vscode.executeLinkProvider', uri);
    assert.ok(links?.some((link) => link.target?.fsPath.endsWith(path.join('pixel', 'src', 'image.avx'))), 'external package import link');
    assert.ok(links?.some((link) => link.target?.fsPath.endsWith(path.join('shared', 'tool.avx'))), 'additional module root import link');
    const externalDefinition = await vscode.commands.executeCommand<Array<vscode.Location | vscode.LocationLink>>(
        'vscode.executeDefinitionProvider', uri, document.positionAt(definitionsSource.lastIndexOf('Color') + 1));
    assert.ok(externalDefinition?.length, 'external package type definition');
    const help = await vscode.commands.executeCommand<vscode.SignatureHelp>('vscode.executeSignatureHelpProvider', uri,
        document.positionAt(definitionsSource.indexOf('40, 2') + 4), ',');
    assert.ok(help?.signatures.some((signature) => signature.label.includes('answer')), 'call signature help');
    assert.equal(help?.activeParameter, 1, 'signature parameter follows source argument position');

    const folding = await vscode.commands.executeCommand<vscode.FoldingRange[]>('vscode.executeFoldingRangeProvider', uri);
    assert.ok(Array.isArray(folding), 'folding provider runs');
    await vscode.commands.executeCommand('aevix.check');
    await waitFor(() => vscode.languages.getDiagnostics(uri).length === 0, 'valid program has no diagnostics');
    const invalid = new vscode.WorkspaceEdit();
    invalid.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), 'i32 main() { return true; }\n');
    assert.ok(await vscode.workspace.applyEdit(invalid));
    await document.save();
    await vscode.commands.executeCommand('aevix.check');
    await waitFor(() => vscode.languages.getDiagnostics(uri).some((diagnostic) => diagnostic.severity === vscode.DiagnosticSeverity.Error), 'real compiler type error');
    const diagnostics = vscode.languages.getDiagnostics(uri);
    assert.ok(diagnostics.some((diagnostic) => diagnostic.source?.toLowerCase().includes('aevix')), 'compiler diagnostics tagged');
    const fix = new vscode.WorkspaceEdit();
    fix.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), 'i32 main() { return 42; }\n');
    assert.ok(await vscode.workspace.applyEdit(fix));
    await document.save();
    await vscode.commands.executeCommand('aevix.check');
    await waitFor(() => vscode.languages.getDiagnostics(uri).length === 0, 'fixed program clears stale diagnostics');
    // 늦게 끝나는 이전 검사가 최신 편집의 진단을 덮어쓰지 않는지 확인한다.
    // Ensure an older slow check cannot overwrite diagnostics for the latest edit.
    const fake = path.join(folder.uri.fsPath, 'slow compiler.js');
    const started = path.join(folder.uri.fsPath, 'slow-check-started');
    fs.writeFileSync(fake, `#!${process.env.AEVIX_TEST_NODE}
const fs = require('node:fs');
let text = ''; process.stdin.setEncoding('utf8'); process.stdin.on('data', value => text += value);
process.stdin.on('end', () => { if (text.includes('stale')) { fs.writeFileSync(${JSON.stringify(started)}, 'started'); setTimeout(() => { process.stderr.write(process.argv.find(value => value.endsWith('.avx')) + ':1:1: error: stale diagnostic\\n'); process.exitCode = 1; }, 650); } else { setTimeout(() => process.exit(0), 20); } });
`, { mode: 0o755 });
    const configuration = vscode.workspace.getConfiguration('aevix', uri);
    const previousCompiler = configuration.get<string>('compilerPath');
    await configuration.update('compilerPath', fake, vscode.ConfigurationTarget.Workspace);
    try {
        const oldEdit = new vscode.WorkspaceEdit();
        oldEdit.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), 'i32 main() { return stale; }\n');
        assert.ok(await vscode.workspace.applyEdit(oldEdit));
        const oldCheck = vscode.commands.executeCommand('aevix.check');
        await waitFor(() => fs.existsSync(started), 'slow compiler started');
        const currentEdit = new vscode.WorkspaceEdit();
        currentEdit.replace(uri, new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length)), 'i32 main() { return 42; }\n');
        assert.ok(await vscode.workspace.applyEdit(currentEdit));
        await vscode.commands.executeCommand('aevix.check');
        await oldCheck;
        await new Promise((resolve) => setTimeout(resolve, 800));
        assert.equal(vscode.languages.getDiagnostics(uri).length, 0, 'cancelled stale output is never published');
    } finally {
        await configuration.update('compilerPath', previousCompiler, vscode.ConfigurationTarget.Workspace);
        fs.rmSync(fake, { force: true }); fs.rmSync(started, { force: true });
    }
    await document.save();
    const outcomes: Array<{ name: string; exitCode: number | undefined }> = [];
    const listener = vscode.tasks.onDidEndTaskProcess((event) => {
        if (event.execution.task.source === 'aevix') outcomes.push({ name: event.execution.task.name, exitCode: event.exitCode });
    });
    try {
        await vscode.commands.executeCommand('aevix.build');
        await waitFor(() => outcomes.some((outcome) => outcome.name === 'Aevix build'), 'build task finishes', 30000);
        assert.equal(outcomes.find((outcome) => outcome.name === 'Aevix build')?.exitCode, 0, 'build task succeeds');
        assert.ok((await vscode.workspace.fs.stat(vscode.Uri.joinPath(folder.uri, 'build/aevix/main'))).size > 0, 'build emits an executable');
        await vscode.commands.executeCommand('aevix.run');
        await waitFor(() => outcomes.some((outcome) => outcome.name === 'Aevix run'), 'run task finishes', 30000);
        assert.equal(outcomes.find((outcome) => outcome.name === 'Aevix run')?.exitCode, 42, 'run task preserves program exit status');
    } finally { listener.dispose(); }
    await vscode.commands.executeCommand('aevix.restart');
    console.log(`Aevix extension-host checks passed on VS Code ${vscode.version}; compiler=${path.basename(process.env.AEVIX_TEST_COMPILER ?? 'aevixc')}`);
}
