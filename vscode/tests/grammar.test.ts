// ========================
//
// Created By : miruku_sora2026
//
// File name : grammar.test.ts
//
// Description :
//     실제 TextMate 엔진으로 Aevix 문법과 배포 설정의 일치를 검사한다.
//     Verify Aevix grammar and distribution metadata with the actual TextMate engine.
//
// ========================

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { Registry, parseRawGrammar, INITIAL, IGrammar, StateStack } from 'vscode-textmate';
import { loadWASM, OnigScanner, OnigString } from 'vscode-oniguruma';

const root = path.resolve(__dirname, '../..');
const manifest = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8'));
let grammarPromise: Promise<IGrammar> | undefined;

function loadGrammar(): Promise<IGrammar> {
    grammarPromise ??= (async () => {
        const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm'));
        await loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));
        const registry = new Registry({
            onigLib: Promise.resolve({
                createOnigScanner: (patterns) => new OnigScanner(patterns),
                createOnigString: (text) => new OnigString(text),
            }),
            loadGrammar: async (scope) => scope === 'source.aevix'
                ? parseRawGrammar(fs.readFileSync(path.join(root, manifest.contributes.grammars[0].path), 'utf8'), 'aevix.json')
                : null,
        });
        const loaded = await registry.loadGrammar('source.aevix');
        assert.ok(loaded, 'Aevix TextMate grammar loads');
        return loaded;
    })();
    return grammarPromise;
}

async function scopeAt(source: string, needle: string, occurrence = 0): Promise<string[]> {
    const grammar = await loadGrammar();
    let stack: StateStack = INITIAL;
    let seen = 0;
    for (const line of source.split('\n')) {
        const result = grammar.tokenizeLine(line, stack);
        stack = result.ruleStack;
        let search = -1;
        while ((search = line.indexOf(needle, search + 1)) >= 0) {
            if (seen++ === occurrence) {
                const token = result.tokens.find((item) => item.startIndex <= search && search < item.endIndex);
                assert.ok(token, `token exists at ${needle}`);
                return token.scopes;
            }
        }
    }
    throw new Error(`Missing test token ${needle}`);
}

async function assertScope(source: string, needle: string, expected: RegExp): Promise<void> {
    assert.match((await scopeAt(source, needle)).join(' '), expected, `${needle} in ${source}`);
}

test('manifest registers .avx, portable runtime and trusted compiler execution', () => {
    assert.equal(manifest.version, '0.3.0');
    assert.equal(manifest.license, 'Apache-2.0');
    assert.equal(manifest.name, 'aevix');
    assert.equal(manifest.contributes.languages[0].id, 'aevix');
    assert.ok(manifest.contributes.languages[0].extensions.includes('.avx'));
    assert.equal(manifest.contributes.grammars[0].scopeName, 'source.aevix');
    assert.equal(manifest.capabilities.untrustedWorkspaces.supported, 'limited');
    assert.ok(manifest.capabilities.untrustedWorkspaces.restrictedConfigurations.includes('aevix.compilerPath'));
    assert.ok(!manifest.dependencies || Object.keys(manifest.dependencies).length === 0, 'runtime is dependency-free');
    for (const contribution of ['languages', 'grammars', 'snippets']) {
        for (const item of manifest.contributes[contribution]) {
            const relative = item.configuration ?? item.path;
            if (relative) assert.ok(fs.existsSync(path.join(root, relative)), relative);
        }
    }
    for (const command of ['aevix.check', 'aevix.build', 'aevix.run', 'aevix.restart']) {
        assert.ok(manifest.contributes.commands.some((item: { command: string }) => item.command === command));
    }
});

test('every compiler keyword receives keyword or storage highlighting', async () => {
    const keywords = 'type import public private struct enum interface implements where override mut const persist rsn dyn extern if else while for in switch case default break continue return move'.split(' ');
    for (const keyword of keywords) await assertScope(`${keyword} name;`, keyword, /\b(?:keyword|storage)\./);
    for (const literal of ['true', 'false']) await assertScope(`bool value = ${literal};`, literal, /constant\.language/);
});

test('all builtin scalar types and aliases receive type highlighting', async () => {
    const types = 'void never unit bool i8 i16 i32 i64 u8 u16 u32 u64 isize usize f16 bf16 f32 f64 char byte string string_view int float Self'.split(' ');
    for (const type of types) await assertScope(`${type} value;`, type, /\b(?:storage\.type|support\.type|entity\.name\.type)\b/);
});

test('integer bases, separators and decimal exponents tokenize as numeric constants', async () => {
    for (const value of ['0', '42', '1_000', '0xff', '0XCA_FE', '0b1010_0011', '0o755', '1.5', '1.0e-3', '2e10']) {
        await assertScope(`rsn value = ${value};`, value, /constant\.numeric/);
    }
});

test('ranges remain operators instead of being swallowed into decimal literals', async () => {
    for (const operator of ['..', '..=']) {
        await assertScope(`for (i32 i in 0${operator}10) { continue; }`, operator, /\b(?:keyword\.operator|punctuation)\b/);
    }
});

test('string and Unicode character content keeps comments inert', async () => {
    await assertScope('string text = "// not a comment";', '//', /string\./);
    await assertScope("char value = '한';", '한', /string\.|constant\.character/);
    await assertScope('string text = "\\n";', '\\n', /constant\.character\.escape/);
    await assertScope('string text = "\\u{1f600}";', '\\u{1f600}', /constant\.character\.escape/);
});

test('block comment state spans lines and ends before the next statement', async () => {
    const source = '/* documentation\n @return value\n @since 0.3\n @deprecated_at none\n @removed_at none\n */\nreturn 42;';
    await assertScope(source, '@return', /comment\.|storage\.type|entity\.name\.tag/);
    await assertScope(source, '@deprecated_at', /comment\.|storage\.type|entity\.name\.tag/);
    for (const tag of ['@since', '@removed_at'])
        await assertScope(source, tag, /storage\.type\.annotation/);
    await assertScope(source, 'return 42', /keyword\.control/);
    await assertScope('// struct Item {};\ni32 value = 0;', 'struct', /comment\./);
    await assertScope('// struct Item {};\ni32 value = 0;', 'i32', /storage\.type|support\.type/);
});

test('compound operators and generic closers retain operator scopes', async () => {
    for (const operator of ['++', '--', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '==', '!=', '<=', '>=', '&&', '||', '<<', '>>', '?', ':', '&mut']) {
        const needle = operator === '&mut' ? '&' : operator;
        await assertScope(`a ${operator} b;`, needle, /keyword\.operator|punctuation/);
    }
    await assertScope('Pair<Pair<i32>> value;', '>>', /keyword\.operator|punctuation/);
});

test('all snippet JSON is valid and insertion bodies have no legacy variable syntax', () => {
    const snippets = JSON.parse(fs.readFileSync(path.join(root, manifest.contributes.snippets[0].path), 'utf8'));
    assert.ok(Object.keys(snippets).length >= 10, 'meaningful language snippet set');
    for (const [name, value] of Object.entries(snippets)) {
        const snippet = value as { prefix: string | string[]; body: string | string[]; description?: string };
        assert.ok(snippet.prefix, name);
        assert.ok(typeof snippet.body === 'string' || Array.isArray(snippet.body), name);
        const text = Array.isArray(snippet.body) ? snippet.body.join('\n') : snippet.body;
        assert.ok(!/\b(?:let|var|auto)\s+[A-Za-z_]\w*\s*:/.test(text), `${name}: declarations stay type-first`);
    }
});

test('typed numeric suffixes receive numeric highlighting', async () => {
    for (const value of ['42i32', '255u8', '0xffu8', '0b101i16', '0o755usize', '1.0f32', '1f32', '1.25bf16', '1e-3f64', '42i128', '340282366920938463463374607431768211455u128']) {
        await assertScope(`rsn value = ${value};`, value, /constant\.numeric/);
    }
});

test('unsupported numeric suffixes and malformed bases remain visibly invalid', async () => {
    for (const value of ['42i256', '1.0f128', '42_i32', '0xG0', '0b102', '0o89']) {
        await assertScope(`rsn value = ${value};`, value, /invalid\.illegal/);
    }
});

test('snippet defaults are accepted by the real Aevix parser', async (context) => {
    const compiler = process.env.AEVIX_TEST_COMPILER ?? path.resolve(root, '../build/aevixc');
    if (!fs.existsSync(compiler)) return context.skip('aevixc not built; run the compiler CMake build first');
    const os = await import('node:os');
    const { execFileSync } = await import('node:child_process');
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'aevix-snippet-tests-'));
    const snippets = JSON.parse(fs.readFileSync(path.join(root, manifest.contributes.snippets[0].path), 'utf8'));
    const declarations = new Set(['Function', 'Void function', 'Main', 'Struct', 'Enum', 'Interface', 'Generic function', 'Variadic function', 'Result propagation function',
        'Import', 'Selective import', 'Re-export', 'Documentation comment', 'Deprecated documentation', 'C external declaration', 'Type alias', 'Owned string function', 'Bench function', 'Generic fixed buffer', 'CPU SIMD import', 'CPU parallel import', 'Complex import']);
    try {
        for (const [name, value] of Object.entries(snippets)) {
            const item = value as { body: string | string[] };
            let body = Array.isArray(item.body) ? item.body.join('\n') : item.body;
            body = body.replace(/\$\{\d+:([^}]*)\}/g, '$1').replace(/\$\{\d+\}|\$\d+/g, '');
            let source = declarations.has(name) ? body : `void test() { ${body} return; }`;
            if (name === 'Read method' || name === 'Mutable method') source = `struct Type { ${body} };`;
            if (name === 'Enum case') source = `void test() { switch (value) { ${body} default: break; } return; }`;
            const input = path.join(temporary, 'snippet.avx');
            fs.writeFileSync(input, `${source}\n`);
            try {
                execFileSync(compiler, ['--ast', input], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], timeout: 10000 });
            } catch (error) {
                const failure = error as { stderr?: Buffer | string };
                assert.fail(`${name}: ${String(failure.stderr ?? error)}\n${source}`);
            }
        }
    } finally {
        fs.rmSync(temporary, { recursive: true, force: true });
    }
});


test('core error carriers and dynamic views use type and storage scopes', async () => {
    for (const type of ['CollectionError', 'InputError', 'RangeError', 'Vector', 'Slice', 'Range', 'Box']) {
        await assertScope(`${type} value;`, type, /support\.type/);
    }
    await assertScope('&mut dyn Readable view = &mut value;', 'dyn', /storage\.modifier/);
});

test('CPU specialist types and bench annotation have distinct scopes', async () => {
    for (const type of ['Simd', 'SimdMask', 'Fn', 'FnMut', 'FnOnce', 'Complex', 'complex']) {
        await assertScope(`${type} value;`, type, /support\.type/);
    }
    await assertScope('@bench i32 main() { return 0; }', '@bench', /annotation/);
});
