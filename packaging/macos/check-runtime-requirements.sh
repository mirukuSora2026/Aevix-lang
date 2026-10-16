#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : check-runtime-requirements.sh
#
# Description :
#     설치 변경 전에 배포물에 기록된 실제 macOS 최소 버전을 검사한다.
#     Check the recorded macOS minimum version before changing an installation.
#
# ========================

set -eu
requirements=${1:?Runtime requirements metadata is required}
[ -f "$requirements" ] || exit 0
minimum=$(awk -F= '$1 == "minimum_macos_version" { print $2 }' "$requirements")
if ! printf '%s\n' "$minimum" | awk '
    /^[0-9]+[.][0-9]+([.][0-9]+)?$/ { valid++ }
    END { exit !(NR == 1 && valid == 1) }'; then
    echo 'Aevix runtime requirements contain an invalid macOS minimum version.' >&2; exit 1
fi
current=$(sw_vers -productVersion) || { echo 'Cannot determine the macOS version; nothing was installed.' >&2; exit 1; }
if ! awk -v current="$current" -v minimum="$minimum" 'BEGIN {
    if (current !~ /^[0-9]+[.][0-9]+([.][0-9]+)?$/) exit 1;
    split(current, host, "."); split(minimum, required, ".");
    for (part = 1; part <= 3; part++) {
        if (host[part] + 0 > required[part] + 0) exit 0;
        if (host[part] + 0 < required[part] + 0) exit 1;
    }
    exit 0;
}'; then
    printf 'Aevix requires macOS %s or newer on Apple Silicon; this host runs macOS %s. Nothing was installed.\n' "$minimum" "$current" >&2
    exit 1
fi
