// ========================
//
// Created By : miruku_sora2026
//
// File name : runPackagedTests.ts
//
// Description :
//     임시 확장 디렉터리에 VSIX를 설치하고 설치된 코드로 호스트 검증을 실행한다.
//     Install the VSIX into a temporary extensions directory and test its installed code.
//
// ========================

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { runExtensionTests } from './runExtensionTests';
import { verifyVsix } from './verifyVsix';

async function main(): Promise<void> {
    const root = path.resolve(__dirname, '../..');
    const artifact = path.join(root, 'dist/aevix-0.1.0.vsix');
    verifyVsix(artifact);
    const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'aevix-vsix-installed-'));
    const cli = process.env.VSCODE_CLI_PATH ?? (process.platform === 'darwin' && process.env.VSCODE_EXECUTABLE_PATH
        ? path.resolve(path.dirname(process.env.VSCODE_EXECUTABLE_PATH), '../Resources/app/bin/code') : 'code');
    const extensions = path.join(temporary, 'extensions');
    const common = ['--user-data-dir', path.join(temporary, 'user-data'), '--extensions-dir', extensions];
    const previous = process.env.AEVIX_TEST_EXTENSION_PATH;
    try {
        execFileSync(cli, [...common, '--install-extension', artifact, '--force'], { encoding: 'utf8', timeout: 60000, stdio: ['ignore', 'pipe', 'pipe'] });
        const installed = execFileSync(cli, [...common, '--list-extensions', '--show-versions'], { encoding: 'utf8', timeout: 30000 });
        assert.ok(installed.includes('miruku-sora2026.aevix@0.1.0'), 'VSIX is installed into isolated profile');
        const directories = await fs.readdir(extensions);
        const directory = directories.find((name) => name.startsWith('miruku-sora2026.aevix-0.1.0'));
        assert.ok(directory, 'installed extension directory exists');
        process.env.AEVIX_TEST_EXTENSION_PATH = path.join(extensions, directory);
        await runExtensionTests();
        console.log('Installed VSIX activation and editor/compiler integration checks passed.');
    } finally {
        if (previous === undefined) delete process.env.AEVIX_TEST_EXTENSION_PATH;
        else process.env.AEVIX_TEST_EXTENSION_PATH = previous;
        await fs.rm(temporary, { recursive: true, force: true });
    }
}

main().catch((error: unknown) => { console.error(error); process.exitCode = 1; });
