#!/usr/bin/env bash
# build-mesa.sh — Build Mesa from upstream source with the d3d12 gallium
# and microsoft-experimental Vulkan drivers enabled.
#
# Purpose: produce userspace GPU drivers that talk /dev/dxg directly
# (Mesa's own dxcore-equivalent embedded in the d3d12 + dzn drivers)
# so an Ubuntu VM with Hyper-V GPU-PV gets hardware OpenGL/Vulkan
# acceleration without needing Microsoft's proprietary libdxcore.so.
#
# Output: installs to /opt/wsl-mesa/. The caller is expected to tar that
# tree and stash it under tools/wsl-mesa/prebuilt/<ubuntu-codename>-amd64/.
#
# Tested on: Ubuntu 26.04 LTS (resolute), amd64, LLVM 21, Mesa 25.3.x.

set -euo pipefail

MESA_BRANCH="${MESA_BRANCH:-25.3}"
MESA_SRC="${MESA_SRC:-$HOME/mesa}"
PREFIX="${PREFIX:-/opt/wsl-mesa}"

echo "==> Installing build dependencies (apt build-dep mesa + LLVM 21 dev pkgs)"
sudo apt-get update
# deb-src must be enabled in /etc/apt/sources.list.d/ubuntu.sources
sudo apt-get build-dep -y mesa
sudo apt-get install -y \
    git meson ninja-build \
    libclc-21-dev llvm-spirv-21 libllvmspirvlib-21-dev libclang-21-dev

echo "==> Cloning Mesa $MESA_BRANCH"
[[ -d "$MESA_SRC" ]] || git clone --depth 1 -b "$MESA_BRANCH" \
    https://gitlab.freedesktop.org/mesa/mesa.git "$MESA_SRC"

echo "==> Configuring meson (prefix=$PREFIX)"
cd "$MESA_SRC"
rm -rf build
meson setup --prefix="$PREFIX" --buildtype=release --strip \
    -D gallium-drivers=llvmpipe,d3d12 \
    -D vulkan-drivers=swrast,microsoft-experimental \
    -D microsoft-clc=enabled \
    -D video-codecs=all \
    -D glvnd=enabled \
    -D platforms=x11,wayland \
    build/

echo "==> Building (this is the slow part, ~30 min)"
ninja -C build -j "$(nproc)"

echo "==> Installing to $PREFIX"
sudo ninja -C build install

echo "==> Done. Tar for stash:"
echo "    sudo tar -C / -cf - opt/wsl-mesa | zstd -T0 > wsl-mesa.tar.zst"
