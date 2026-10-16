// ========================
//
// Created By : miruku_sora2026
//
// File name : compiler.ts
//
// Description :
//     제한된 출력과 취소를 지원하며 셸 없이 컴파일러 프로세스를 실행한다.
//     Run compiler processes without a shell, with bounded output and cancellation.
//
// ========================

import { spawn, ChildProcessWithoutNullStreams } from 'node:child_process';

export interface CompilerRunOptions {
    executable: string; args: readonly string[]; input?: string; cwd?: string;
    timeoutMs?: number; maxOutputBytes?: number; signal?: AbortSignal;
}
export interface CompilerRunResult {
    exitCode: number | null; stdout: string; stderr: string;
    cancelled: boolean; timedOut: boolean; outputLimited: boolean;
    error?: Error;
}
export function runCompiler(options: CompilerRunOptions): Promise<CompilerRunResult> {
    return new Promise(resolve => {
        if (options.signal?.aborted) { resolve({ exitCode: null, stdout: '', stderr: '', cancelled: true, timedOut: false, outputLimited: false }); return; }
        let settled = false; let bytes = 0;
        const result: CompilerRunResult = { exitCode: null, stdout: '', stderr: '', cancelled: false, timedOut: false, outputLimited: false };
        const limit = Math.max(1, options.maxOutputBytes ?? 1024 * 1024);
        let child: ChildProcessWithoutNullStreams;
        try { child = spawn(options.executable, [...options.args], { cwd: options.cwd, shell: false, windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] }); }
        catch (error) { result.error = error instanceof Error ? error : new Error(String(error)); resolve(result); return; }
        const finish = () => {
            if (settled) { return; } settled = true;
            clearTimeout(timer); options.signal?.removeEventListener('abort', abort);
            resolve(result);
        };
        const abort = () => { result.cancelled = true; child.kill('SIGKILL'); };
        options.signal?.addEventListener('abort', abort, { once: true });
        const timer = setTimeout(() => { result.timedOut = true; child.kill('SIGKILL'); }, Math.max(1, options.timeoutMs ?? 15000));
        const stdout: Buffer[] = [], stderr: Buffer[] = [];
        const receive = (buffers: Buffer[], data: Buffer) => {
            const available = Math.max(0, limit - bytes); if (available > 0) { buffers.push(data.subarray(0, available)); }
            bytes += data.length;
            if (bytes > limit) { result.outputLimited = true; child.kill('SIGKILL'); }
        };
        child.stdout.on('data', (data: Buffer) => receive(stdout, data));
        child.stderr.on('data', (data: Buffer) => receive(stderr, data));
        child.on('error', error => { result.error = error; });
        child.on('close', code => {
            result.exitCode = code; result.stdout = Buffer.concat(stdout).toString('utf8'); result.stderr = Buffer.concat(stderr).toString('utf8'); finish();
        });
        // 조기 종료된 컴파일러의 EPIPE는 확장 호스트를 종료시키지 않는다.
        // An early compiler exit must not crash the extension host through EPIPE.
        child.stdin.on('error', () => undefined);
        child.stdin.end(options.input ?? '');
    });
}
