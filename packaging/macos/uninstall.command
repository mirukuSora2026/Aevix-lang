#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : uninstall.command
#
# Description :
#     시스템 패키지가 소유한 파일과 paths.d 항목을 제거한다.
#     Remove only the system package's managed installation and paths.d entry.
#
# ========================

set -eu
if [ "$(id -u)" -ne 0 ]; then exec sudo /bin/sh "$0"; fi
prefix=/Library/Aevix
[ -f "$prefix/.aevix-system-install" ] && [ "$(cat "$prefix/.aevix-system-install")" = 'Aevix system package v1' ] || {
    echo 'No managed Aevix system installation was found.' >&2; exit 2
}
if [ -f /etc/paths.d/aevix ] && [ "$(cat /etc/paths.d/aevix)" = "$prefix/bin" ]; then rm /etc/paths.d/aevix; fi
rm -rf "$prefix"
pkgutil --forget org.aevix.compiler >/dev/null 2>&1 || true
echo 'Aevix was uninstalled. Open a new terminal to refresh PATH.'
