#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : installer_configuration_tests.sh
#
# Description :
#     CMake 설치 구성과 Windows 구성별 페이로드 선택을 격리 환경에서 검사한다.
#     Check CMake install configuration and Windows payload selection in isolated fixtures.
#
# ========================

set -eu
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/aevix-installer-config.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/posix build" "$work/windows build/CustomRelease" "$work/windows build/runtime-payload/CustomRelease" "$work/tools"
# 실제 CMake 설치 단계가 요청한 구성을 받았는지 확인한다.
# Verify the requested configuration in a real CMake install step.
printf '%s\n' '#!/bin/sh' 'echo "Aevix selected CustomRelease"' > "$work/posix build/compiler"
chmod 755 "$work/posix build/compiler"
# 이 페이로드는 셸 스텁이므로 Mach-O 검사는 별도 메타데이터 스텁으로 격리한다.
# Isolate Mach-O inspection for this shell payload; real Mach-O requirements have a separate regression gate.
printf '%s\n' '#!/bin/sh' 'printf "%s\\n" "cmd LC_BUILD_VERSION" "minos 11.0"' > "$work/tools/otool"
chmod 755 "$work/tools/otool"
cat > "$work/posix build/cmake_install.cmake" <<'CMAKE'
if(NOT CMAKE_INSTALL_CONFIG_NAME STREQUAL "CustomRelease")
    message(FATAL_ERROR "The requested install configuration was lost")
endif()
get_filename_component(fixture "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
file(INSTALL "${fixture}/compiler" DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE PROGRAM RENAME aevixc)
CMAKE
env PATH="$work/tools:$PATH" sh "$source_root/tools/packaging/build-installers.sh" --build-dir "$work/posix build" --config CustomRelease --output-dir "$work/posix packages" --version 0.2.0-config-test --no-bundle-libraries > "$work/posix.log" 2>&1
grep -q 'Aevix selected CustomRelease' "$work/posix.log"
test -s "$work/posix packages/SHA256SUMS"
# 이 도구 스텁은 경로 선택만 검사한다. Windows 실행 증거로 취급하지 않는다.
# These tool stubs test payload selection only; they are not Windows execution evidence.
printf '%s\n' selected-compiler > "$work/windows build/CustomRelease/aevixc.exe"
printf '%s\n' selected-runtime > "$work/windows build/runtime-payload/CustomRelease/libaevix_runtime.a"
printf '%s\n' default-compiler > "$work/windows build/aevixc.exe"
printf '%s\n' default-runtime > "$work/windows build/runtime-payload/libaevix_runtime.a"
cat > "$work/tools/windres" <<'STUB'
#!/bin/sh
set -eu
cmp compiler.bin "$AEVIX_CONFIG_EXPECT_COMPILER"
cmp runtime.bin "$AEVIX_CONFIG_EXPECT_RUNTIME"
for payload in LICENSE.txt CONTRIBUTING.txt logo.png numeric.avx complex.avx GCC-GPL-3.0.txt GCC-RUNTIME-EXCEPTION.txt MinGW-w64-COPYING.txt SOURCES.txt payload/LICENSE-UNICODE.txt payload/NOTICE.txt; do test -s "$payload"; done
# 중첩 모듈과 고지문이 리소스 목록과 설치 목록 양쪽에 포함되는지 검사한다.
# Check nested modules and notices occur in both resource and installation lists.
test -s payload/Core/unicode.avx
test -s payload/System/process.avx
grep -q 'share/aevix/library/Core/unicode.avx' AevixPayloadFiles.inc
grep -q 'share/aevix/library/System/process.avx' AevixPayloadFiles.inc
grep -q 'share/aevix/licenses/LICENSE-UNICODE.txt' AevixPayloadFiles.inc
grep -q 'share/aevix/licenses/NOTICE.txt' AevixPayloadFiles.inc
grep -q 'payload/Core/unicode.avx' payload.rc
grep -q 'payload/System/process.avx' payload.rc
find "$AEVIX_CONFIG_SOURCE_ROOT/library" -type f -name '*.avx' > module-expected
while IFS= read -r module; do
    relative=${module#"$AEVIX_CONFIG_SOURCE_ROOT/library/"}
    case "$relative" in
        Math/numeric.avx) cmp "$module" numeric.avx ;;
        Math/complex.avx) cmp "$module" complex.avx ;;
        *)
            cmp "$module" "payload/$relative"
            grep -Fq "share/aevix/library/$relative" AevixPayloadFiles.inc
            grep -Fq "payload/$relative" payload.rc
            ;;
    esac
done < module-expected
for notice in LICENSE-UNICODE.txt NOTICE.txt; do
    cmp "$AEVIX_CONFIG_SOURCE_ROOT/third_party/unicode/17.0.0/$notice" "payload/$notice"
done
printf '%s\n' resources > payload.o
STUB
cat > "$work/tools/cxx" <<'STUB'
#!/bin/sh
set -eu
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then printf '%s\n' fixture-setup > "$2"; exit 0; fi
    shift
done
exit 2
STUB
chmod 755 "$work/tools/windres" "$work/tools/cxx"
export AEVIX_WINDOWS_CXX="$work/tools/cxx" AEVIX_WINDOWS_WINDRES="$work/tools/windres"
export AEVIX_CONFIG_SOURCE_ROOT="$source_root"
export AEVIX_CONFIG_EXPECT_COMPILER="$work/windows build/CustomRelease/aevixc.exe"
export AEVIX_CONFIG_EXPECT_RUNTIME="$work/windows build/runtime-payload/CustomRelease/libaevix_runtime.a"
sh "$source_root/tools/packaging/build-windows-installer.sh" --build-dir "$work/windows build" --config CustomRelease --output-dir "$work/windows selected" --version 0.2.0-config-test > "$work/windows-selected.log"
export AEVIX_CONFIG_EXPECT_COMPILER="$work/windows build/aevixc.exe"
export AEVIX_CONFIG_EXPECT_RUNTIME="$work/windows build/runtime-payload/libaevix_runtime.a"
sh "$source_root/tools/packaging/build-windows-installer.sh" --build-dir "$work/windows build" --output-dir "$work/windows default" --version 0.2.0-config-test > "$work/windows-default.log"
# 단일 구성 빌드에서도 명시적 --config를 사용할 수 있다.
# Explicit --config remains usable with a single-configuration build.
sh "$source_root/tools/packaging/build-windows-installer.sh" --build-dir "$work/windows build" --config Release --output-dir "$work/windows fallback" --version 0.2.0-config-test > "$work/windows-fallback.log"
# 구성별 파일 하나가 없으면 서로 다른 구성의 컴파일러와 런타임을 섞지 않는다.
# Reject an incomplete configured payload instead of mixing it with default-build files.
mkdir -p "$work/windows build/Incomplete"
printf '%s\n' incomplete-compiler > "$work/windows build/Incomplete/aevixc.exe"
if sh "$source_root/tools/packaging/build-windows-installer.sh" --build-dir "$work/windows build" --config Incomplete --output-dir "$work/windows incomplete" > "$work/windows-incomplete.log" 2>&1; then
    echo 'Expected incomplete configured payload rejection.' >&2; exit 1
fi
grep -q 'selected Windows compiler and runtime archive' "$work/windows-incomplete.log"
for script in build-installers.sh build-windows-installer.sh; do
    if sh "$source_root/tools/packaging/$script" --config > "$work/missing-value.log" 2>&1; then
        echo 'Expected missing --config value rejection.' >&2; exit 1
    fi
    grep -q -- '--config requires a configuration name.' "$work/missing-value.log"
done
printf '%s\n' 'Installer configuration checks passed: CMake selection, Windows configured/default payloads, and argument errors.'
