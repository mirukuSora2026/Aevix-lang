// ========================
//
// Created By : miruku_sora2026
//
// File name : package.ts
//
// Description :
//     매니페스트 버전으로 VSIX를 생성하고 구성과 라이선스를 검사한다.
//     Package the manifest version and verify VSIX contents and licensing.
//
// ========================

import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { verifyVsix } from './verifyVsix';

const root = path.resolve(__dirname, '../..');
const manifest = JSON.parse(fs.readFileSync(path.join(root, 'package.json'), 'utf8')) as { version: string };
const filename = `aevix-${manifest.version}.vsix`;
const output = path.join(root, 'dist', filename);
fs.mkdirSync(path.dirname(output), { recursive: true });
const vscePackage = require.resolve('@vscode/vsce/package.json');
execFileSync(process.execPath, [path.join(path.dirname(vscePackage), 'vsce'), 'package', '--out', output],
    { cwd: root, stdio: 'inherit' });
verifyVsix(output);
const checksums = fs.readdirSync(path.dirname(output)).filter(name => name.endsWith('.vsix')).sort().map(name =>
    `${createHash('sha256').update(fs.readFileSync(path.join(path.dirname(output), name))).digest('hex')}  ${name}`);
fs.writeFileSync(path.join(root, 'dist/SHA256SUMS'), checksums.join('\n') + '\n');
