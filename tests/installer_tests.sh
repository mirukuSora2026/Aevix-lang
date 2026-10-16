#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : installer_tests.sh
#
# Description :
#     격리한 홈에서 설치·업그레이드·PATH·제거 및 충돌 처리를 검사한다.
#     Check installation, upgrades, PATH, removal, and conflicts in isolated home directories.
#
# ========================

set -eu
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/aevix-installer-tests.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
home="$work/home with spaces and 'quote'"
stage="$work/payload"
mkdir -p "$stage/bin" "$home"
printf '%s\n' '#!/bin/sh' 'echo "Aevix installer test compiler"' > "$stage/bin/aevixc"
cp "$source_root/packaging/posix/uninstall.sh" "$stage/uninstall.sh"
chmod 755 "$stage/bin/aevixc" "$stage/uninstall.sh"
tar -czf "$work/payload.tar.gz" -C "$stage" .
if command -v sha256sum >/dev/null 2>&1; then digest=$(sha256sum "$work/payload.tar.gz" | awk '{print $1}')
else digest=$(shasum -a 256 "$work/payload.tar.gz" | awk '{print $1}'); fi
arch=$(uname -m)
case "$arch" in aarch64|arm64) arch=arm64 ;; esac
sed -e 's/@AEVIX_VERSION@/0.2.0-test/g' -e "s/@AEVIX_SYSTEM@/$(uname -s)/g" -e "s/@AEVIX_ARCH@/$arch/g" -e "s/@AEVIX_PAYLOAD_SHA256@/$digest/g" "$source_root/packaging/posix/install-header.sh.in" > "$work/setup.run"
cat "$work/payload.tar.gz" >> "$work/setup.run"
printf '%s\n' '# existing user settings' 'export AEVIX_USER_SENTINEL=preserved' > "$home/.zshrc"
cp "$home/.zshrc" "$work/original-zshrc"
env HOME="$home" /bin/sh "$work/setup.run" > "$work/install.log"
test -x "$home/.local/bin/aevixc"
test "$(grep -c '^# >>> Aevix PATH >>>$' "$home/.zshrc")" = 1
grep -q 'AEVIX_USER_SENTINEL=preserved' "$home/.zshrc"
# 설치한 경로를 실제 새 셸에서 조회해 환경 변수 적용을 확인한다.
# Resolve the installed command from a fresh shell to check the generated PATH configuration.
env HOME="$home" PATH=/usr/bin:/bin /bin/sh -c '. "$HOME/.profile"; command -v aevixc; aevixc --version' > "$work/path.log"
grep -q 'Aevix installer test compiler' "$work/path.log"
env HOME="$home" /bin/sh "$work/setup.run" > "$work/upgrade.log"
test "$(grep -c '^# >>> Aevix PATH >>>$' "$home/.zshrc")" = 1
env HOME="$home" /bin/sh "$home/.local/share/aevix/uninstall.sh" > "$work/uninstall.log"
test ! -e "$home/.local/share/aevix"
test ! -L "$home/.local/bin/aevixc"
cmp "$home/.zshrc" "$work/original-zshrc"
# 다른 프로그램과 사용자가 만든 디렉터리를 덮어쓰지 않는다.
# Never replace another command or an unmanaged user directory.
mkdir -p "$home/.local/bin"
printf '%s\n' existing-command > "$home/.local/bin/aevixc"
if env HOME="$home" /bin/sh "$work/setup.run" > "$work/conflict.log" 2>&1; then echo 'Expected command conflict rejection.' >&2; exit 1; fi
grep -q existing-command "$home/.local/bin/aevixc"
rm "$home/.local/bin/aevixc"
mkdir -p "$home/.local/share/aevix"
touch "$home/.local/share/aevix/user-file"
if env HOME="$home" /bin/sh "$work/setup.run" > "$work/unmanaged.log" 2>&1; then echo 'Expected unmanaged prefix rejection.' >&2; exit 1; fi
test -f "$home/.local/share/aevix/user-file"
rm -rf "$home/.local/share/aevix"
env HOME="$home" /bin/sh "$work/setup.run" --no-modify-path > "$work/no-path.log"
cmp "$home/.zshrc" "$work/original-zshrc"
env HOME="$home" /bin/sh "$home/.local/share/aevix/uninstall.sh" > /dev/null
cp "$work/setup.run" "$work/corrupt.run"
printf '\ncorrupt' >> "$work/corrupt.run"
if env HOME="$home" /bin/sh "$work/corrupt.run" > "$work/corrupt.log" 2>&1; then echo 'Expected checksum rejection.' >&2; exit 1; fi
test ! -e "$home/.local/share/aevix"
# 공유 라이브러리 실행 래퍼도 사용자 PATH의 심볼릭 링크를 따라가야 한다.
# The shared-library launcher must also follow the symlink installed on the user's PATH.
mkdir -p "$work/launcher/bin" "$work/launcher/lib/aevix/compiler" "$work/launcher-link"
cp "$source_root/packaging/posix/compiler-launcher.sh" "$work/launcher/bin/aevixc"
printf '%s\n' '#!/bin/sh' 'test -n "$LD_LIBRARY_PATH"' 'printf "launcher argument: %s\\n" "$1"' > "$work/launcher/bin/aevixc.real"
chmod 755 "$work/launcher/bin/aevixc" "$work/launcher/bin/aevixc.real"
ln -s '../launcher/bin/aevixc' "$work/launcher-link/aevixc"
"$work/launcher-link/aevixc" 'spaces and $literal' > "$work/launcher.log"
grep -Fq 'launcher argument: spaces and $literal' "$work/launcher.log"
echo 'Installer checks passed: PATH, spaces, upgrades, uninstall, ownership, and checksums.'
sh "$source_root/tests/installer_configuration_tests.sh"
sh "$source_root/tests/macos_requirements_tests.sh"
cmake "-DTEST_DIR=$work/debian-notices" -P "$source_root/tests/debian_notices_tests.cmake"
