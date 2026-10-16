// ========================
//
// Created By : miruku_sora2026
//
// File name : runExtensionTests.ts
//
// Description :
//     격리된 프로필과 작업공간에서 VS Code 확장 호스트 검증을 실행한다.
//     Run VS Code extension-host verification with an isolated profile and workspace.
//
// ========================

import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { runTests } from '@vscode/test-electron';

export async function runExtensionTests(): Promise<void> {
    const root = path.resolve(__dirname, '../..');
    const temporary = await fs.mkdtemp(path.join(os.tmpdir(), 'aevix-vscode-host-'));
    const workspace = path.join(temporary, 'workspace');
    const compiler = process.env.AEVIX_TEST_COMPILER ?? path.resolve(root, '../build/aevixc');
    await fs.mkdir(path.join(workspace, 'src'), { recursive: true });
    await fs.mkdir(path.join(workspace, '.vscode'), { recursive: true });
    await fs.writeFile(path.join(workspace, '.vscode/settings.json'), JSON.stringify({
        'aevix.compilerPath': compiler,
        'aevix.sourceRoot': 'src',
        'aevix.moduleRoots': ['../shared'],
        'aevix.packages': { pixel: '../pixel/src' },
        'aevix.diagnostics.delay': 50,
    }));
    await fs.writeFile(path.join(workspace, 'src/main.avx'), 'i32 main() { return 42; }\n');
    await fs.writeFile(path.join(workspace, 'src/helper.avx'), 'public struct Point { public i32 x; };\n');
    await fs.mkdir(path.join(temporary, 'pixel/src'), { recursive: true });
    await fs.mkdir(path.join(temporary, 'shared'), { recursive: true });
    await fs.writeFile(path.join(temporary, 'pixel/src/image.avx'), 'public struct Color { public i32 value; };\n');
    await fs.writeFile(path.join(temporary, 'shared/tool.avx'), 'public i32 twice(i32 value) { return value * 2; }\n');
    try {
        await runTests({
            ...(process.env.VSCODE_EXECUTABLE_PATH ? { vscodeExecutablePath: process.env.VSCODE_EXECUTABLE_PATH } : {}),
            extensionDevelopmentPath: process.env.AEVIX_TEST_EXTENSION_PATH ?? root,
            extensionTestsPath: path.join(root, 'out/tests/extensionHost/index.js'),
            extensionTestsEnv: { AEVIX_TEST_COMPILER: compiler, AEVIX_TEST_NODE: process.execPath },
            launchArgs: [workspace, '--user-data-dir', path.join(temporary, 'user-data'),
                '--extensions-dir', path.join(temporary, 'extensions'), '--disable-workspace-trust',
                '--skip-welcome', '--skip-release-notes', '--disable-extensions', '--disable-updates'],
        });
    } finally {
        await fs.rm(temporary, { recursive: true, force: true });
    }
}

if (require.main === module) {
    runExtensionTests().catch((error: unknown) => {
        console.error(error);
        process.exitCode = 1;
    });
}
