// ========================
//
// Created By : miruku_sora2026
//
// File name : compilerSearch.test.ts
//
// Description :
//     Windows 실행 파일과 MSVC 다중 구성 경로 및 기존 POSIX 탐색을 검증한다.
//     Verify Windows executables, MSVC multi-configuration paths, and existing POSIX discovery.
//
// ========================

import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { compilerSearchCandidates, findWorkspaceCompiler } from '../src/compilerSearch';

test('Windows candidates include Ninja and MSVC Release and Debug executables', () => {
    const candidates = compilerSearchCandidates(['C:\\work space\\Aevix'], 'win32');
    assert.equal(candidates[0], 'C:\\work space\\Aevix\\build\\aevixc.exe');
    assert.ok(candidates.includes('C:\\work space\\Aevix\\build\\Release\\aevixc.exe'));
    assert.ok(candidates.includes('C:\\work space\\Aevix\\build\\Debug\\aevixc.exe'));
    assert.ok(candidates.includes('C:\\work space\\Aevix\\build\\RelWithDebInfo\\aevixc.exe'));
    assert.ok(candidates.includes('C:\\work space\\Aevix\\build\\bin\\Release\\aevixc.exe'));
    assert.ok(candidates.every(candidate => candidate.endsWith('.exe') && !candidate.includes('/')));
});

test('Windows discovery selects a multi-configuration Release compiler without invoking it', async () => {
    const expected = 'C:\\project\\build\\Release\\aevixc.exe';
    const visited: string[] = [];
    const found = await findWorkspaceCompiler(['C:\\project'], 'win32', async candidate => {
        visited.push(candidate); return candidate === expected;
    });
    assert.equal(found, expected);
    assert.deepEqual(visited, ['C:\\project\\build\\aevixc.exe', 'C:\\project\\build\\bin\\aevixc.exe', expected]);
});

test('workspace candidate roots retain priority and duplicate candidates are omitted', async () => {
    const roots = ['C:\\first', 'C:\\first', 'D:\\second'];
    const candidates = compilerSearchCandidates(roots, 'win32');
    assert.equal(candidates.length, new Set(candidates).size);
    const expected = 'C:\\first\\bin\\aevixc.exe';
    const found = await findWorkspaceCompiler(roots, 'win32', async candidate => candidate === expected || candidate === 'D:\\second\\build\\aevixc.exe');
    assert.equal(found, expected);
});

test('POSIX candidates preserve the existing build and bin order', () => {
    assert.deepEqual(compilerSearchCandidates(['/workspace'], 'darwin'),
        ['/workspace/build/aevixc', '/workspace/build/bin/aevixc', '/workspace/bin/aevixc']);
    assert.deepEqual(compilerSearchCandidates(['/workspace'], 'linux'),
        ['/workspace/build/aevixc', '/workspace/build/bin/aevixc', '/workspace/bin/aevixc']);
});

test('missing workspace candidates fall back to the platform PATH command', async () => {
    assert.equal(await findWorkspaceCompiler(['C:\\project'], 'win32', async () => false), 'aevixc.exe');
    assert.equal(await findWorkspaceCompiler(['/workspace'], 'linux', async () => false), 'aevixc');
});

test('filesystem discovery rejects directories and non-executable POSIX files', async context => {
    if (process.platform === 'win32') { context.skip('POSIX executable permissions'); return; }
    const root = await fs.mkdtemp(path.join(os.tmpdir(), 'aevix compiler search '));
    try {
        await fs.mkdir(path.join(root, 'build/aevixc'), { recursive: true });
        await fs.mkdir(path.join(root, 'build/bin'), { recursive: true });
        await fs.writeFile(path.join(root, 'build/bin/aevixc'), 'not executable');
        await fs.chmod(path.join(root, 'build/bin/aevixc'), 0o600);
        await fs.mkdir(path.join(root, 'bin'));
        const expected = path.join(root, 'bin/aevixc');
        await fs.writeFile(expected, '#!/bin/sh\nexit 0\n'); await fs.chmod(expected, 0o700);
        assert.equal(await findWorkspaceCompiler([root]), expected);
    } finally { await fs.rm(root, { recursive: true, force: true }); }
});
