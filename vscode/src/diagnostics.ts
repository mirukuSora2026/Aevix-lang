// ========================
//
// Created By : miruku_sora2026
//
// File name : diagnostics.ts
//
// Description :
//     컴파일러 진단과 UTF-8 바이트 열을 에디터의 UTF-16 위치로 변환한다.
//     Parse compiler diagnostics and convert UTF-8 byte columns to editor UTF-16 positions.
//
// ========================

export interface CompilerDiagnostic {
    path?: string; line: number; byteColumn: number;
    severity: 'error' | 'warning' | 'information'; message: string;
}

// 컴파일러 열은 1부터 시작하는 UTF-8 바이트 수이며, 이 반환값은 0부터 시작한다.
// Compiler columns count UTF-8 bytes from one; the returned editor character starts at zero.
export function byteColumnToCharacter(line: string, byteColumn: number): number {
    const target = Math.max(0, byteColumn - 1); let bytes = 0; let characters = 0;
    for (const scalar of line) {
        const length = Buffer.byteLength(scalar, 'utf8');
        if (bytes + length > target) { break; }
        bytes += length; characters += scalar.length;
    }
    return characters;
}
export function parseCompilerDiagnostics(output: string): CompilerDiagnostic[] {
    const result: CompilerDiagnostic[] = [];
    for (const raw of output.replace(/\x1b\[[0-9;]*m/g, '').split(/\r?\n/)) {
        const located = /^(.*):(\d+):(\d+):\s*(error|warning|note):\s*(.*)$/.exec(raw);
        if (located) {
            result.push({ path: located[1], line: Math.max(0, Number(located[2]) - 1), byteColumn: Math.max(1, Number(located[3])), severity: located[4] === 'note' ? 'information' : located[4] as 'error' | 'warning', message: located[5] });
            continue;
        }
        const plain = /^(error|warning|note):\s*(.*)$/.exec(raw);
        if (plain) { result.push({ line: 0, byteColumn: 1, severity: plain[1] === 'note' ? 'information' : plain[1] as 'error' | 'warning', message: plain[2] }); }
    }
    return result;
}
