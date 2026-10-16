// ========================
//
// Created By : miruku_sora2026
//
// File name : verifyVsix.ts
//
// Description :
//     VSIX의 ZIP 디렉터리와 런타임·소스·문법·스니펫 구성을 검증한다.
//     Verify VSIX ZIP entries and runtime, source, grammar, and snippet contents.
//
// ========================

import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { inflateRawSync } from 'node:zlib';

export function readVsix(filename: string): Map<string, Buffer> {
    const bytes = fs.readFileSync(filename);
    let end = bytes.length - 22;
    while (end >= Math.max(0, bytes.length - 65557) && bytes.readUInt32LE(end) !== 0x06054b50) --end;
    assert.ok(end >= 0 && bytes.readUInt32LE(end) === 0x06054b50, 'ZIP end directory exists');
    const count = bytes.readUInt16LE(end + 10);
    const files = new Map<string, Buffer>();
    let entry = bytes.readUInt32LE(end + 16);
    for (let i = 0; i < count; ++i) {
        assert.equal(bytes.readUInt32LE(entry), 0x02014b50, 'ZIP central header');
        const method = bytes.readUInt16LE(entry + 10);
        const compressed = bytes.readUInt32LE(entry + 20);
        const size = bytes.readUInt32LE(entry + 24);
        const nameLength = bytes.readUInt16LE(entry + 28);
        const extraLength = bytes.readUInt16LE(entry + 30);
        const commentLength = bytes.readUInt16LE(entry + 32);
        const local = bytes.readUInt32LE(entry + 42);
        const name = bytes.toString('utf8', entry + 46, entry + 46 + nameLength);
        assert.ok(!files.has(name), `no duplicate ZIP entry ${name}`);
        assert.ok(!name.split('/').includes('..') && !name.startsWith('/'), `safe ZIP path ${name}`);
        assert.equal(bytes.readUInt32LE(local), 0x04034b50, 'ZIP local header');
        const offset = local + 30 + bytes.readUInt16LE(local + 26) + bytes.readUInt16LE(local + 28);
        const content = bytes.subarray(offset, offset + compressed);
        const inflated = method === 0 ? content : method === 8 ? inflateRawSync(content) : undefined;
        assert.ok(inflated, `supported ZIP compression ${method}`);
        assert.equal(inflated.length, size, `ZIP uncompressed size ${name}`);
        files.set(name, inflated);
        entry += 46 + nameLength + extraLength + commentLength;
    }
    return files;
}

export function verifyVsix(filename: string): void {
    const files = readVsix(filename);
    for (const required of ['[Content_Types].xml', 'extension.vsixmanifest', 'extension/package.json',
        'extension/language-configuration.json', 'extension/syntaxes/aevix.tmLanguage.json',
        'extension/snippets/aevix.json', 'extension/src/extension.ts',
        'extension/src/language.ts', 'extension/src/compiler.ts', 'extension/src/compilerSearch.ts', 'extension/src/diagnostics.ts']) {
        assert.ok(files.has(required), `package contains ${required}`);
    }
    // 로컬에 있는 선택적 문서는 포함 여부를 검사하고, 문서 없는 checkout도 허용한다.
    // Require optional local documentation when present, while allowing documentation-free checkouts.
    for (const [source, packaged] of [['README.md', 'extension/readme.md'], ['CHANGELOG.md', 'extension/changelog.md']]) {
        if (fs.existsSync(path.resolve(__dirname, '../..', source))) {
            assert.ok(files.has(packaged), `package contains ${packaged}`);
        }
    }
    const manifest = JSON.parse(files.get('extension/package.json')!.toString('utf8'));
    const sourceManifest = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../../package.json'), 'utf8'));
    assert.equal(manifest.version, sourceManifest.version);
    assert.equal(manifest.license, 'Apache-2.0');
    const license = files.get('extension/LICENSE.txt') ?? files.get('extension/LICENSE');
    assert.ok(license, 'Apache license is packaged');
    assert.match(license.toString('utf8'), /Apache License\s+Version 2\.0/);
    assert.equal(manifest.publisher, 'miruku-sora2026');
    const main = path.posix.normalize(`extension/${manifest.main}`);
    assert.ok(files.has(main), `compiled entry point ${main}`);
    for (const [name, data] of files) {
        assert.ok(!/(^|\/)(node_modules|tests|dist|\.vscode-test)(\/|$)/.test(name), `no development tree or local artifacts ${name}`);
        assert.ok(!name.endsWith('.vsix') && !name.endsWith('.map'), `no recursive artifact ${name}`);
        assert.ok(!name.endsWith('.log') && !name.endsWith('/verification.json'), `no local validation output ${name}`);
        assert.doesNotMatch(data.toString('utf8'), /\/Users\/[A-Za-z0-9_][^\s"'<>]*|\/private\/tmp\/|\/var\/folders\//, `no personal local paths ${name}`);
        if (name.endsWith('.json')) JSON.parse(data.toString('utf8'));
        if (name.startsWith('extension/out/src/') && name.endsWith('.js')) {
            for (const match of data.toString('utf8').matchAll(/require\(["'](\.[^"']+)["']\)/g)) {
                const dependency = path.posix.normalize(path.posix.join(path.posix.dirname(name), `${match[1]}.js`));
                assert.ok(files.has(dependency), `${name} dependency ${dependency}`);
            }
        }
    }
    console.log(`VSIX verified: ${files.size} entries, ${fs.statSync(filename).size} bytes, SHA256 ${createHash('sha256').update(fs.readFileSync(filename)).digest('hex')}`);
}

if (require.main === module) {
    const manifest = JSON.parse(fs.readFileSync(path.resolve(__dirname, '../../package.json'), 'utf8'));
    const filename = process.argv[2] ?? path.resolve(__dirname, `../../dist/aevix-${manifest.version}.vsix`);
    verifyVsix(filename);
}
