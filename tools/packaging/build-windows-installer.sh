#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : build-windows-installer.sh
#
# Description :
#     정적 링크 Windows 컴파일러와 런타임을 네이티브 설치 EXE로 묶는다.
#     Embed a statically linked Windows compiler and runtime in a native setup executable.
#
# ========================

set -eu
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
build_dir=
config=
output_dir="$source_root/releases/0.3V/installers"
version=0.3.0
cxx=${AEVIX_WINDOWS_CXX:-x86_64-w64-mingw32-g++}
windres=${AEVIX_WINDOWS_WINDRES:-x86_64-w64-mingw32-windres}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=$2; shift 2 ;;
        --config) [ "$#" -ge 2 ] && [ -n "$2" ] || { echo '--config requires a configuration name.' >&2; exit 2; }; config=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --version) version=$2; shift 2 ;;
        --help) echo 'Usage: build-windows-installer.sh --build-dir WINDOWS_BUILD [--output-dir DIR] [--version VERSION] [--config NAME]'; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done
[ -n "$build_dir" ] || { echo '--build-dir is required.' >&2; exit 2; }
compiler_payload="$build_dir/aevixc.exe"
runtime_payload="$build_dir/runtime-payload/libaevix_runtime.a"
if [ -n "$config" ]; then
    # 여러 구성의 페이로드를 우선하고 단일 구성 빌드의 기존 위치도 지원한다.
    # Prefer configuration-specific payloads, retaining the existing single-config layout.
    if [ -f "$build_dir/$config/aevixc.exe" ] || [ -f "$build_dir/runtime-payload/$config/libaevix_runtime.a" ]; then
        compiler_payload="$build_dir/$config/aevixc.exe"
        runtime_payload="$build_dir/runtime-payload/$config/libaevix_runtime.a"
    fi
fi
[ -f "$compiler_payload" ] && [ -f "$runtime_payload" ] || {
    echo '--build-dir must contain the selected Windows compiler and runtime archive.' >&2; exit 2
}
case "$version" in ''|*[!0-9A-Za-z._-]*) echo 'Invalid version.' >&2; exit 2 ;; esac
command -v "$cxx" >/dev/null 2>&1 || { echo 'The x86-64 MinGW C++ compiler is required.' >&2; exit 2; }
command -v "$windres" >/dev/null 2>&1 || { echo 'The x86-64 MinGW resource compiler is required.' >&2; exit 2; }
mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/aevix-windows-setup.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp "$compiler_payload" "$work/compiler.bin"
cp "$runtime_payload" "$work/runtime.bin"
cp "$source_root/LICENSE" "$work/LICENSE.txt"
cp "$source_root/CONTRIBUTING.md" "$work/CONTRIBUTING.txt"
cp "$source_root/design/logo/aevix-logo.png" "$work/logo.png"
# 표준 라이브러리 파일도 컴파일러와 같은 버전의 설치 페이로드에 포함한다.
# Ship the standard-library files from the same source revision as the compiler.
cp "$source_root/library/Math/numeric.avx" "$work/numeric.avx"
cp "$source_root/library/Math/complex.avx" "$work/complex.avx"
cp "$source_root/packaging/licenses/"*.txt "$work/"
# 모든 소스 모듈과 Unicode 고지를 고정 리소스 목록에 추가한다.
# Add every source module and Unicode notice to the fixed resource payload.
mkdir -p "$work/payload"
find "$source_root/library" -type f -name '*.avx' | LC_ALL=C sort > "$work/module-files"
: > "$work/AevixPayloadFiles.inc"
: > "$work/extra-resources.rc"
resource_id=112
while IFS= read -r module; do
    relative=${module#"$source_root/library/"}
    case "$relative" in
        Math/numeric.avx|Math/complex.avx) continue ;;
        *[!A-Za-z0-9_./-]*) echo 'A library path cannot be encoded safely in the Windows payload.' >&2; exit 2 ;;
    esac
    mkdir -p "$work/payload/$(dirname -- "$relative")"
    cp "$module" "$work/payload/$relative"
    marker=$(printf '%s' "$relative" | tr '/' '!')
    printf '%s RCDATA "payload/%s"\n' "$resource_id" "$relative" >> "$work/extra-resources.rc"
    printf '    {%s, L"share/aevix/library/%s", L".aevix-file-%s"},\n' "$resource_id" "$relative" "$marker" >> "$work/AevixPayloadFiles.inc"
    resource_id=$((resource_id + 1))
done < "$work/module-files"
for notice in LICENSE-UNICODE.txt NOTICE.txt; do
    cp "$source_root/third_party/unicode/17.0.0/$notice" "$work/payload/$notice"
    printf '%s RCDATA "payload/%s"\n' "$resource_id" "$notice" >> "$work/extra-resources.rc"
    printf '    {%s, L"share/aevix/licenses/%s", L".aevix-unicode-%s"},\n' "$resource_id" "$notice" "$notice" >> "$work/AevixPayloadFiles.inc"
    resource_id=$((resource_id + 1))
done
cat > "$work/payload.rc" <<'AEVIX_RESOURCES'
#include <windows.h>
101 RCDATA "compiler.bin"
102 RCDATA "runtime.bin"
103 RCDATA "LICENSE.txt"
104 RCDATA "CONTRIBUTING.txt"
105 RCDATA "logo.png"
106 RCDATA "numeric.avx"
107 RCDATA "complex.avx"
108 RCDATA "GCC-GPL-3.0.txt"
109 RCDATA "GCC-RUNTIME-EXCEPTION.txt"
110 RCDATA "MinGW-w64-COPYING.txt"
111 RCDATA "SOURCES.txt"
1 RT_MANIFEST "setup.manifest"
AEVIX_RESOURCES
cat "$work/extra-resources.rc" >> "$work/payload.rc"
cat > "$work/setup.manifest" <<'AEVIX_MANIFEST'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="0.3.0.0" processorArchitecture="amd64" name="Aevix.Setup" type="win32"/>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security></trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application><supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application></compatibility>
</assembly>
AEVIX_MANIFEST
cd "$work"
"$windres" -i payload.rc -o payload.o
output="$output_dir/Aevix-$version-Windows-x86_64-Setup.exe"
"$cxx" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -municode -mwindows -static -static-libgcc -static-libstdc++ -fno-ident \
    "-ffile-prefix-map=$source_root=." "-DAEVIX_SETUP_VERSION=L\"$version\"" \
    -I "$work" "$source_root/packaging/windows/Setup.cpp" payload.o -o "$output" -lshell32 -lole32 -luuid -ladvapi32 -luser32
printf 'Created %s\n' "$output"
cp "$source_root/packaging/INSTALL.txt" "$output_dir/INSTALL.txt"
sh "$source_root/tools/packaging/checksums.sh" "$output_dir"
