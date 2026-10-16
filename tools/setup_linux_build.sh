#!/usr/bin/env bash
# ========================
#
# Created By : miruku_sora2026
#
# File name : setup_linux_build.sh
#
# Description :
#     Ubuntu 24.04에 서명된 LLVM 22 저장소와 Aevix 빌드 도구를 설치한다.
#     Install the signed LLVM 22 repository and Aevix build tools on Ubuntu 24.04.
#
# ========================
set -euo pipefail

if ! [[ -r /etc/os-release ]]; then
    printf 'Ubuntu 24.04 is required.\n' >&2
    exit 1
fi
. /etc/os-release
if [[ "${ID:-}" != ubuntu || "${VERSION_CODENAME:-}" != noble ]]; then
    printf 'This setup script supports Ubuntu 24.04 (noble).\n' >&2
    exit 1
fi
sudo apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build curl ca-certificates gnupg \
    libffi-dev libedit-dev libxml2-dev zlib1g-dev libzstd-dev time

aevix_key_file=$(mktemp)
trap 'rm -f "$aevix_key_file"' EXIT
curl --fail --silent --show-error --location https://apt.llvm.org/llvm-snapshot.gpg.key -o "$aevix_key_file"
aevix_fingerprint=$(gpg --batch --show-keys --with-colons "$aevix_key_file" 2>/dev/null | awk -F: '$1 == "fpr" { print $10; exit }')
if [[ "$aevix_fingerprint" != 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421 ]]; then
    printf 'LLVM signing key fingerprint differs from the published key.\n' >&2
    exit 1
fi
sudo install -m 0644 "$aevix_key_file" /usr/share/keyrings/aevix-llvm.asc
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/aevix-llvm.asc] https://apt.llvm.org/noble/ llvm-toolchain-noble-22 main' \
    | sudo tee /etc/apt/sources.list.d/aevix-llvm22.list >/dev/null
sudo apt-get update
sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends clang-22 llvm-22-dev libclang-rt-22-dev
printf 'LLVM 22 and Aevix build tools are ready.\n'
