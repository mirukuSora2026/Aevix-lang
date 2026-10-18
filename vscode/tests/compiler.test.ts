// ========================
//
// Created By : miruku_sora2026
//
// File name : compiler.test.ts
//
// Description :
//     셸 없는 프로세스 실행, 취소, 시간 제한과 진단의 문자 위치를 검사한다.
//     Verify shell-free process execution, cancellation, time limits, and diagnostic positions.
//
// ========================

import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { runCompiler } from '../src/compiler';
import { byteColumnToCharacter, parseCompilerDiagnostics } from '../src/diagnostics';

const runNode = (source: string, extra: Partial<Parameters<typeof runCompiler>[0]> = {}) =>
    runCompiler({ executable: process.execPath, args: ['-e', source], ...extra });

test('UTF-8 compiler columns convert to UTF-16 character positions including supplementary scalars', () => {
    const line = '한😀a';
    for (const [column, expected] of [[1, 0], [2, 0], [4, 1], [5, 1], [8, 3], [9, 4], [900, 4], [0, 0], [-2, 0]]) {
        assert.equal(byteColumnToCharacter(line, column), expected, `byte column ${column}`);
    }
    assert.equal(byteColumnToCharacter('', 4), 0);
    assert.equal(byteColumnToCharacter('a\tb', 3), 2);
});

test('diagnostic parsing preserves colons, spaces, imported paths and ANSI-cleaned messages', () => {
    const messages = parseCompilerDiagnostics('\u001b[31m/private/work space/math:vector.avx:12:7: error: wrong: type\u001b[0m\r\nC:\\src\\main.avx:3:2: warning: caution\nhelper.avx:1:1: note: related\nerror: no compiler\nprogress 100%');
    assert.deepEqual(messages, [
        { path: '/private/work space/math:vector.avx', line: 11, byteColumn: 7, severity: 'error', message: 'wrong: type' },
        { path: 'C:\\src\\main.avx', line: 2, byteColumn: 2, severity: 'warning', message: 'caution' },
        { path: 'helper.avx', line: 0, byteColumn: 1, severity: 'information', message: 'related' },
        { line: 0, byteColumn: 1, severity: 'error', message: 'no compiler' },
    ]);
});

test('runCompiler transports stdin and returns stdout, stderr and exact exit status', async () => {
    const result = await runNode('process.stdin.setEncoding("utf8"); let text="";process.stdin.on("data",d=>text+=d);process.stdin.on("end",()=>{process.stdout.write(text);process.stderr.write("warning: example");process.exitCode=7;});', { input: '한글 😀\n' });
    assert.equal(result.stdout, '한글 😀\n');
    assert.equal(result.stderr, 'warning: example');
    assert.equal(result.exitCode, 7);
    assert.equal(result.cancelled || result.timedOut || result.outputLimited, false);
});

test('argv is literal even with spaces and shell metacharacters', async () => {
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'aevix compiler argv '));
    try {
        const script = path.join(temporary, 'test script.js');
        const marker = path.join(temporary, 'must not exist');
        fs.writeFileSync(script, 'process.stdout.write(JSON.stringify(process.argv.slice(2)));');
        const args = [script, 'with spaces', `$(touch "${marker}")`, `; touch "${marker}"`, '`literal`'];
        const result = await runCompiler({ executable: process.execPath, args });
        assert.equal(result.exitCode, 0);
        assert.deepEqual(JSON.parse(result.stdout), args.slice(1));
        assert.equal(fs.existsSync(marker), false);
    } finally { fs.rmSync(temporary, { recursive: true, force: true }); }
});

test('a pre-cancelled operation never launches a compiler', async () => {
    const controller = new AbortController();
    controller.abort();
    const result = await runCompiler({ executable: '/this/compiler/does/not/exist', args: [], signal: controller.signal });
    assert.equal(result.cancelled, true);
    assert.equal(result.error, undefined);
    assert.equal(result.exitCode, null);
});

test('running compilers are cancelled promptly', async () => {
    const controller = new AbortController();
    const pending = runNode('setInterval(()=>{},1000);', { signal: controller.signal });
    const timer = setTimeout(() => controller.abort(), 50);
    try {
        const result = await pending;
        assert.equal(result.cancelled, true);
        assert.equal(result.timedOut, false);
        assert.equal(result.exitCode, null);
    } finally { clearTimeout(timer); }
});

test('compiler hangs time out with a distinct reason', async () => {
    const result = await runNode('setInterval(()=>{},1000);', { timeoutMs: 50 });
    assert.equal(result.timedOut, true);
    assert.equal(result.cancelled, false);
    assert.equal(result.exitCode, null);
});

test('combined stdout and stderr are bounded before publication', async () => {
    const result = await runNode('process.stdout.write("x".repeat(1000000));process.stderr.write("y".repeat(1000000));', { maxOutputBytes: 128 });
    assert.equal(result.outputLimited, true);
    assert.ok(Buffer.byteLength(result.stdout) + Buffer.byteLength(result.stderr) <= 128);
});

test('UTF-8 sequences split across child output chunks remain intact', async () => {
    const result = await runNode('const b=Buffer.from("😀한");process.stdout.write(b.subarray(0,2));setTimeout(()=>process.stdout.write(b.subarray(2)),20);');
    assert.equal(result.stdout, '😀한');
    assert.equal(result.exitCode, 0);
});

test('missing compiler and early stdin close return errors without crashing the host', async () => {
    const missing = await runCompiler({ executable: '/this/compiler/does/not/exist', args: [] });
    assert.ok(missing.error);
    const closed = await runNode('process.exit(0);', { input: 'x'.repeat(2000000) });
    assert.equal(closed.exitCode, 0);
});

test('real compiler checks current unsaved entry text through stdin', async (context) => {
    const compiler = process.env.AEVIX_TEST_COMPILER ?? path.resolve(__dirname, '../../../build/aevixc');
    if (!fs.existsSync(compiler)) return context.skip('aevixc must be built');
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'aevix-unsaved-'));
    try {
        const input = path.join(temporary, 'main.avx');
        fs.writeFileSync(input, 'i32 main() { return 42; }');
        const checked = await runCompiler({ executable: compiler, args: ['--check', '--stdin', input], input: 'i32 main() { return "invalid"; }' });
        assert.equal(checked.exitCode, 1);
        const diagnostics = parseCompilerDiagnostics(checked.stderr);
        assert.ok(diagnostics.some((diagnostic) => diagnostic.path === fs.realpathSync(input) && diagnostic.severity === 'error'));
        assert.equal(fs.readFileSync(input, 'utf8'), 'i32 main() { return 42; }', 'editor validation does not overwrite disk');
    } finally { fs.rmSync(temporary, { recursive: true, force: true }); }
});
