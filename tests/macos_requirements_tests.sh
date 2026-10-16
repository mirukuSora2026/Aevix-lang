#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : macos_requirements_tests.sh
#
# Description :
#     격리한 도구와 설치 경로에서 Mach-O 요구 버전 집계와 설치 전 거절을 검사한다.
#     Check Mach-O minimum-version aggregation and rejection before installation using isolated tools and paths.
#
# ========================

set -eu
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/aevix-macos-requirements.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/tools" "$work/build" "$work/captured" "$work/home" "$work/target"
export AEVIX_REQUIREMENTS_CAPTURE="$work/captured"
export AEVIX_REQUIREMENTS_TAR=$(command -v tar)
export AEVIX_REQUIREMENTS_COMPILER_EXECUTED="$work/compiler-executed"
export AEVIX_TEST_MACOS_VERSION=14.10
export AEVIX_TEST_COMPILER_MINIMUM=13.0
cat > "$work/tools/uname" <<'STUB'
#!/bin/sh
case "$1" in -s) echo Darwin ;; -m) echo arm64 ;; *) exit 2 ;; esac
STUB
cat > "$work/tools/sw_vers" <<'STUB'
#!/bin/sh
printf '%s\n' "$AEVIX_TEST_MACOS_VERSION"
STUB
cat > "$work/tools/otool" <<'STUB'
#!/bin/sh
case "$4" in
    */aevixc) printf '%s\n' 'cmd LC_BUILD_VERSION' "minos $AEVIX_TEST_COMPILER_MINIMUM" ;;
    */first.dylib) printf '%s\n' 'cmd LC_BUILD_VERSION' 'minos 14.9' ;;
    */framework-runtime) printf '%s\n' 'cmd LC_VERSION_MIN_MACOSX' 'version 14.10' ;;
    *) exit 2 ;;
esac
STUB
cat > "$work/tools/pkgbuild" <<'STUB'
#!/bin/sh
set -eu
while [ "$#" -gt 0 ]; do
    if [ "$1" = --scripts ]; then cp -R "$2/." "$AEVIX_REQUIREMENTS_CAPTURE/"; fi
    output=$1
    shift
done
printf '%s\n' fixture-package > "$output"
STUB
cat > "$work/tools/tar" <<'STUB'
#!/bin/sh
set -eu
while [ "$#" -gt 0 ]; do
    case "$1" in --uid|--gid|--uname|--gname) shift 2 ;; *) break ;; esac
done
exec "$AEVIX_REQUIREMENTS_TAR" "$@"
STUB
chmod 755 "$work/tools/"*
cat > "$work/build/compiler" <<'STUB'
#!/bin/sh
printf '%s\n' invoked > "$AEVIX_REQUIREMENTS_COMPILER_EXECUTED"
echo 'Aevix macOS requirement fixture'
STUB
chmod 755 "$work/build/compiler"
cat > "$work/build/cmake_install.cmake" <<'CMAKE'
get_filename_component(fixture "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
file(INSTALL "${fixture}/compiler" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE PROGRAM RENAME aevixc)
file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}/lib/aevix/compiler")
file(WRITE "${CMAKE_INSTALL_PREFIX}/lib/aevix/compiler/first.dylib" "fixture")
file(WRITE "${CMAKE_INSTALL_PREFIX}/lib/aevix/compiler/framework-runtime" "fixture")
CMAKE
export PATH="$work/tools:$PATH"
sh "$source_root/tools/packaging/build-installers.sh" --build-dir "$work/build" \
    --output-dir "$work/packages" --version 0.2.0-minimum-test --no-bundle-libraries > "$work/build.log"
requirements="$work/packages/Aevix-0.2.0-minimum-test-macOS-arm64-runtime-requirements.txt"
grep -qx 'minimum_macos_version=14.10' "$requirements"
cmp "$requirements" "$work/captured/runtime-requirements.txt"
grep -q 'macOS 14.10 or newer' "$work/build.log"

# 수치 비교는 부 버전과 패치 버전까지 검사하고 구형 fixture는 허용한다.
# Compare minor and patch components numerically and retain support for legacy fixtures.
for host in 14.10 14.10.1 15.0; do
    env AEVIX_TEST_MACOS_VERSION="$host" /bin/sh "$work/captured/check-runtime-requirements.sh" "$requirements"
done
for host in 14.9 13.99.99; do
    if env AEVIX_TEST_MACOS_VERSION="$host" /bin/sh "$work/captured/check-runtime-requirements.sh" "$requirements" > "$work/old.log" 2>&1; then
        echo 'Expected older macOS version rejection.' >&2; exit 1
    fi
    grep -q 'Nothing was installed' "$work/old.log"
done
/bin/sh "$work/captured/check-runtime-requirements.sh" "$work/legacy-metadata-missing"

# 구형 호스트에서 .run은 컴파일러 실행과 HOME·설정 변경 전에 실패해야 한다.
# On an older host the .run must fail before executing the compiler or changing HOME and shell files.
printf '%s\n' user-settings > "$work/home/.zshrc"
cp "$work/home/.zshrc" "$work/original-zshrc"
rm "$AEVIX_REQUIREMENTS_COMPILER_EXECUTED"
if env HOME="$work/home" AEVIX_TEST_MACOS_VERSION=14.9 /bin/sh "$work/packages/Aevix-0.2.0-minimum-test-Darwin-arm64.run" > "$work/run-old.log" 2>&1; then
    echo 'Expected .run minimum macOS rejection.' >&2; exit 1
fi
grep -q 'requires macOS 14.10' "$work/run-old.log"
test ! -e "$AEVIX_REQUIREMENTS_COMPILER_EXECUTED"
test ! -e "$work/home/.local"
cmp "$work/home/.zshrc" "$work/original-zshrc"
if env AEVIX_TEST_MACOS_VERSION=14.9 /bin/sh "$work/captured/preinstall" ignored ignored "$work/target" > "$work/pkg-old.log" 2>&1; then
    echo 'Expected PKG preinstall minimum macOS rejection.' >&2; exit 1
fi
grep -q 'requires macOS 14.10' "$work/pkg-old.log"
test ! -e "$work/target/Library"
env AEVIX_TEST_MACOS_VERSION=14.10 /bin/sh "$work/captured/preinstall" ignored ignored "$work/target"
env HOME="$work/home" AEVIX_TEST_MACOS_VERSION=14.10 /bin/sh "$work/packages/Aevix-0.2.0-minimum-test-Darwin-arm64.run" --no-modify-path > "$work/run-supported.log"
test -x "$work/home/.local/bin/aevixc"
cmp "$work/home/.zshrc" "$work/original-zshrc"
env HOME="$work/home" /bin/sh "$work/home/.local/share/aevix/uninstall.sh" > /dev/null

# 컴파일러 자체의 요구 버전이 더 높으면 라이브러리보다 우선해야 한다.
# A higher compiler deployment target must take precedence over library minimums.
env AEVIX_TEST_COMPILER_MINIMUM=16.0 sh "$source_root/tools/packaging/build-installers.sh" --build-dir "$work/build" \
    --output-dir "$work/compiler-newer" --version 0.2.0-minimum-test --no-bundle-libraries > "$work/compiler-newer.log"
grep -qx 'minimum_macos_version=16.0' "$work/compiler-newer/Aevix-0.2.0-minimum-test-macOS-arm64-runtime-requirements.txt"
printf '%s\n' 'macOS requirement checks passed: dependency/compiler maximums, numeric versions, preinstall and .run early rejection, supported installation and legacy metadata.'
