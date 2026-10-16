#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : uninstall.sh
#
# Description :
#     사용자 설치 파일과 설치기가 추가한 PATH 항목만 제거한다.
#     Remove the managed per-user installation and only its installer-owned PATH entries.
#
# ========================

set -eu
prefix=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
[ -f "$prefix/.aevix-install" ] && [ "$(cat "$prefix/.aevix-install")" = 'Aevix managed installation v1' ] || {
    echo 'This directory is not an Aevix managed installation.' >&2; exit 2
}
link="${HOME:?HOME must be set}/.local/bin/aevixc"
owns_link=no
if [ -L "$link" ]; then
    link_target=$(readlink "$link")
    case "$link_target" in
        /*/bin/aevixc)
            link_prefix=$(CDPATH= cd -- "$(dirname -- "$link_target")/.." 2>/dev/null && pwd -P) || link_prefix=
            if [ "$link_prefix" = "$prefix" ]; then rm "$link"; owns_link=yes; fi ;;
    esac
fi
if [ -f "$prefix/.aevix-path-managed" ] && [ "$owns_link" = yes ]; then
    work=$(mktemp -d "${TMPDIR:-/tmp}/aevix-uninstall.XXXXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    for file in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zprofile" "$HOME/.zshrc"; do
        [ ! -f "$file" ] || {
            awk '/^# >>> Aevix PATH >>>$/ { skip=1; next } /^# <<< Aevix PATH <<<$/{ skip=0; next } !skip { print }' "$file" > "$work/profile"
            cat "$work/profile" > "$file"
        }
    done
    fish_file="$HOME/.config/fish/conf.d/aevix.fish"
    if [ -f "$fish_file" ] && grep -q '^# Aevix managed PATH$' "$fish_file"; then rm "$fish_file"; fi
fi
rm -rf "$prefix"
printf '%s\n' 'Aevix was uninstalled. Open a new terminal to refresh PATH.'
