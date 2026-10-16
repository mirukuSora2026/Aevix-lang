// ========================
//
// Created By : miruku_sora2026
//
// File name : compilerSearch.ts
//
// Description :
//     운영체제별 컴파일러 실행 파일과 CMake 구성 디렉터리를 순서대로 탐색한다.
//     Search platform compiler executables and CMake configuration directories in order.
//
// ========================

import { access, constants, stat } from 'node:fs/promises';
import * as path from 'node:path';

export function compilerSearchCandidates(roots: readonly string[], platform: NodeJS.Platform = process.platform): string[] {
    const paths = platform === 'win32' ? path.win32 : path.posix;
    const names = platform === 'win32'
        ? ['build/aevixc.exe', 'build/bin/aevixc.exe', 'build/Release/aevixc.exe', 'build/Debug/aevixc.exe',
            'build/RelWithDebInfo/aevixc.exe', 'build/MinSizeRel/aevixc.exe',
            'build/bin/Release/aevixc.exe', 'build/bin/Debug/aevixc.exe',
            'build/bin/RelWithDebInfo/aevixc.exe', 'build/bin/MinSizeRel/aevixc.exe', 'bin/aevixc.exe']
        : ['build/aevixc', 'build/bin/aevixc', 'bin/aevixc'];
    return [...new Set(roots.flatMap(root => names.map(name => paths.join(root, name))))];
}

export async function findWorkspaceCompiler(roots: readonly string[], platform: NodeJS.Platform = process.platform,
    canUse?: (candidate: string) => Promise<boolean>): Promise<string> {
    const usable = canUse ?? (async (candidate: string): Promise<boolean> => {
        try {
            if (!(await stat(candidate)).isFile()) { return false; }
            // Windows는 POSIX 실행 비트 대신 실행 파일의 존재를 확인한다.
            // Windows checks file existence rather than a POSIX executable permission bit.
            await access(candidate, platform === 'win32' ? constants.F_OK : constants.X_OK);
            return true;
        } catch { return false; }
    });
    for (const candidate of compilerSearchCandidates(roots, platform)) {
        if (await usable(candidate)) { return candidate; }
    }
    return platform === 'win32' ? 'aevixc.exe' : 'aevixc';
}
