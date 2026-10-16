#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : checksums.sh
#
# Description :
#     같은 디렉터리의 설치 배포물에 대한 SHA-256 목록을 갱신한다.
#     Refresh SHA-256 checksums for installer artifacts in one directory.
#
# ========================

set -eu
[ "$#" -eq 1 ] || { echo 'Usage: checksums.sh INSTALLER_DIRECTORY' >&2; exit 2; }
cd "$1"
set --
for file in ./*.run ./*.pkg ./*-Setup.exe; do
    [ ! -f "$file" ] || set -- "$@" "$file"
done
[ "$#" -gt 0 ] || { echo 'No installer artifacts were found.' >&2; exit 2; }
if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@" > SHA256SUMS.new
else shasum -a 256 "$@" > SHA256SUMS.new; fi
mv SHA256SUMS.new SHA256SUMS
