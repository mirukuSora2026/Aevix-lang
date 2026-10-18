#!/bin/sh
# ========================
#
# Created By : miruku_sora2026
#
# File name : build-installers.sh
#
# Description :
#     CMake 설치 결과로 재배치 가능한 사용자 설치기와 macOS 패키지를 생성한다.
#     Build relocatable user installers and a macOS package from a CMake installation.
#
# ========================

set -eu
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
build_dir=
config=
output_dir="$source_root/releases/0.3V/installers"
version=0.3.0
bundle=yes
while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=$2; shift 2 ;;
        --config) [ "$#" -ge 2 ] && [ -n "$2" ] || { echo '--config requires a configuration name.' >&2; exit 2; }; config=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --version) version=$2; shift 2 ;;
        --no-bundle-libraries) bundle=no; shift ;;
        --help) echo 'Usage: build-installers.sh --build-dir DIR [--output-dir DIR] [--version VERSION] [--config NAME] [--no-bundle-libraries]'; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done
[ -n "$build_dir" ] || { echo '--build-dir is required.' >&2; exit 2; }
case "$version" in ''|*[!0-9A-Za-z._-]*) echo 'Invalid version.' >&2; exit 2 ;; esac
system=$(uname -s)
arch=$(uname -m)
case "$arch" in arm64|aarch64) arch=arm64 ;; x86_64|amd64) arch=x86_64 ;; *) echo 'Only ARM64 and x86-64 installers are supported.' >&2; exit 2 ;; esac
case "$system/$arch" in Darwin/arm64|Linux/arm64|Linux/x86_64) ;; *) echo 'Supported hosts: macOS ARM64 and Linux ARM64/x86-64.' >&2; exit 2 ;; esac
if [ "$system" = Darwin ]; then
    # 배포 파일에 호스트 확장 속성이나 AppleDouble 메타데이터를 포함하지 않는다.
    # Exclude host extended attributes and AppleDouble metadata from distributable files.
    export COPYFILE_DISABLE=1
fi
mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd -P)
work=$(mktemp -d "${TMPDIR:-/tmp}/aevix-package.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
stage="$work/stage"
if [ -n "$config" ]; then
    cmake --install "$build_dir" --config "$config" --prefix "$stage"
else
    cmake --install "$build_dir" --prefix "$stage"
fi
# 개인용 예제와 빌드 로그는 설치 배포물에 포함하지 않는다.
# Exclude local examples and build logs from distributable installers.
rm -rf "$stage/share/aevix/examples"
if [ "$bundle" = yes ]; then cmake "-DSTAGE=$stage" -P "$source_root/tools/packaging/bundle-runtime.cmake"; fi
if [ "$system" = Darwin ]; then
    # 컴파일러와 실제 동봉 라이브러리 중 가장 높은 Mach-O 최소 버전을 기록한다.
    # Record the highest Mach-O minimum version among the compiler and its bundled libraries.
    command -v otool >/dev/null 2>&1 || { echo 'otool is required to inspect macOS runtime requirements.' >&2; exit 2; }
    printf '%s\n' "$stage/bin/aevixc" > "$work/macos-images.txt"
    if [ -d "$stage/lib/aevix/compiler" ]; then
        find "$stage/lib/aevix/compiler" -type f >> "$work/macos-images.txt"
    fi
    : > "$work/macos-minimums.txt"
    while IFS= read -r image; do
        otool -arch arm64 -l "$image" > "$work/macos-load-commands.txt"
        awk '$1 == "cmd" { command = $2 }
            command == "LC_BUILD_VERSION" && $1 == "minos" { print $2 }
            command == "LC_VERSION_MIN_MACOSX" && $1 == "version" { print $2 }' \
            "$work/macos-load-commands.txt" > "$work/image-minimums.txt"
        [ -s "$work/image-minimums.txt" ] || { printf 'Cannot determine the macOS minimum version: %s\n' "$image" >&2; exit 2; }
        cat "$work/image-minimums.txt" >> "$work/macos-minimums.txt"
    done < "$work/macos-images.txt"
    minimum_macos=$(awk '
        function greater(left, right, first, second, part) {
            split(left, first, "."); split(right, second, ".");
            for (part = 1; part <= 3; part++) {
                if (first[part] + 0 != second[part] + 0) return first[part] + 0 > second[part] + 0;
            }
            return 0;
        }
        !/^[0-9]+[.][0-9]+([.][0-9]+)?$/ { invalid = 1 }
        NR == 1 || greater($0, maximum) { maximum = $0 }
        END { if (invalid || NR == 0) exit 1; print maximum }' "$work/macos-minimums.txt")
    mkdir -p "$stage/share/aevix"
    printf '%s\n' 'format=1' 'system=Darwin' "architecture=$arch" "minimum_macos_version=$minimum_macos" \
        > "$stage/share/aevix/runtime-requirements.txt"
    cp "$source_root/packaging/macos/check-runtime-requirements.sh" "$stage/share/aevix/check-runtime-requirements.sh"
    cp "$stage/share/aevix/runtime-requirements.txt" "$output_dir/Aevix-$version-macOS-$arch-runtime-requirements.txt"
    printf 'macOS runtime requirement: macOS %s or newer on Apple Silicon (compiler and bundled libraries).\n' "$minimum_macos"
fi
mkdir -p "$stage/share/aevix/licenses"
if [ -f "$source_root/LICENSE" ]; then cp "$source_root/LICENSE" "$stage/share/aevix/licenses/Aevix-LICENSE"; fi
cp "$source_root/packaging/posix/uninstall.sh" "$stage/uninstall.sh"
chmod 755 "$stage/uninstall.sh"
"$stage/bin/aevixc" --version
if [ "$system" = Darwin ]; then
    tar --uid 0 --gid 0 --uname root --gname root -czf "$work/payload.tar.gz" -C "$stage" .
else
    tar --owner=0 --group=0 --numeric-owner -czf "$work/payload.tar.gz" -C "$stage" .
fi
if command -v sha256sum >/dev/null 2>&1; then
    payload_sha256=$(sha256sum "$work/payload.tar.gz" | awk '{print $1}')
else payload_sha256=$(shasum -a 256 "$work/payload.tar.gz" | awk '{print $1}'); fi
run="$output_dir/Aevix-$version-$system-$arch.run"
sed -e "s/@AEVIX_VERSION@/$version/g" -e "s/@AEVIX_SYSTEM@/$system/g" -e "s/@AEVIX_ARCH@/$arch/g" -e "s/@AEVIX_PAYLOAD_SHA256@/$payload_sha256/g" "$source_root/packaging/posix/install-header.sh.in" > "$run"
cat "$work/payload.tar.gz" >> "$run"
chmod 755 "$run"
printf 'Created %s\n' "$run"
if [ "$system" = Darwin ]; then
    command -v pkgbuild >/dev/null 2>&1 || { echo 'pkgbuild is required for the macOS package.' >&2; exit 2; }
    command -v xar >/dev/null 2>&1 || { echo 'xar is required to remove builder metadata from the macOS package.' >&2; exit 2; }
    root="$work/pkgroot"
    mkdir -p "$root/Library" "$root/etc/paths.d"
    cp -R -X "$stage" "$root/Library/Aevix"
    rm "$root/Library/Aevix/uninstall.sh"
    cp "$source_root/packaging/macos/uninstall.command" "$root/Library/Aevix/uninstall.command"
    chmod 755 "$root/Library/Aevix/uninstall.command"
    printf '%s\n' 'Aevix system package v1' > "$root/Library/Aevix/.aevix-system-install"
    printf '%s\n' '/Library/Aevix/bin' > "$root/etc/paths.d/aevix"
    pkg_version=$(printf '%s' "$version" | sed 's/[-_].*$//')
    pkg="$output_dir/Aevix-$version-macOS-arm64.pkg"
    scripts="$work/pkg-scripts"
    mkdir -p "$scripts"
    cp "$source_root/packaging/macos/scripts/preinstall" "$scripts/preinstall"
    cp "$stage/share/aevix/runtime-requirements.txt" "$stage/share/aevix/check-runtime-requirements.sh" "$scripts/"
    chmod 755 "$scripts/preinstall"
    pkgbuild --root "$root" --identifier org.aevix.compiler --version "$pkg_version" --install-location / --ownership recommended --scripts "$scripts" "$pkg"
    # 외부 XAR에도 빌더의 사용자 이름과 UID가 남지 않도록 배포 속성만 보관한다.
    # Keep distribution properties in the outer XAR without the builder's user name or UID.
    pkg_contents="$work/pkg-contents"
    mkdir -p "$pkg_contents"
    xar -xf "$pkg" -C "$pkg_contents"
    (
        cd "$pkg_contents"
        xar -cf "$work/distribution.pkg" --distribution --prop-exclude=user --prop-exclude=group \
            --prop-exclude=uid --prop-exclude=gid --compression=none Bom Payload Scripts PackageInfo
    )
    mv "$work/distribution.pkg" "$pkg"
    printf 'Created %s\n' "$pkg"
fi
cp "$source_root/packaging/INSTALL.txt" "$output_dir/INSTALL.txt"
sh "$source_root/tools/packaging/checksums.sh" "$output_dir"
