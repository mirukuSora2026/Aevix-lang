#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : compiler-launcher.sh
#
# Description :
#     심볼릭 링크를 해석해 동봉한 Linux 공유 라이브러리로 컴파일러를 실행한다.
#     Resolve symlinks and launch the compiler with its bundled Linux shared libraries.
#
# ========================

set -eu
location=$0
while [ -L "$location" ]; do
    directory=$(CDPATH= cd -- "$(dirname -- "$location")" && pwd -P)
    target=$(readlink "$location")
    case "$target" in /*) location=$target ;; *) location="$directory/$target" ;; esac
done
compiler_dir=$(CDPATH= cd -- "$(dirname -- "$location")" && pwd -P)
export LD_LIBRARY_PATH="$compiler_dir/../lib/aevix/compiler${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$compiler_dir/aevixc.real" "$@"
