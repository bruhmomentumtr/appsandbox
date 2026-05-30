#!/bin/sh
# AppSandbox virtual DRM driver — first-cut deploy script for the running VM.
#
# Runs entirely on the guest. Builds the module against the running kernel,
# installs the DKMS package, drops the modprobe blacklist + autoload files,
# and rebuilds the initramfs so the changes survive reboot.
#
# Run as: sudo sh deploy.sh
set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
PKG_VERSION=$(awk -F\" '/^PACKAGE_VERSION/ {print $2}' "$SRC_DIR/dkms.conf")
PKG_NAME=asb_drm

if [ "$(id -u)" != 0 ]; then
    echo "Run as root (sudo sh $0)" >&2
    exit 1
fi

echo "--- installing build deps ---"
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential \
    dkms \
    "linux-headers-$(uname -r)" \
    >/dev/null

echo "--- staging source at /usr/src/${PKG_NAME}-${PKG_VERSION} ---"
DST=/usr/src/${PKG_NAME}-${PKG_VERSION}
rm -rf "$DST"
mkdir -p "$DST"
cp "$SRC_DIR"/*.c "$SRC_DIR"/*.h "$SRC_DIR/Kbuild" "$SRC_DIR/Makefile" "$SRC_DIR/dkms.conf" "$DST"/

echo "--- DKMS add/build/install ---"
dkms add     -m "$PKG_NAME" -v "$PKG_VERSION" 2>/dev/null || true
dkms build   -m "$PKG_NAME" -v "$PKG_VERSION"
dkms install -m "$PKG_NAME" -v "$PKG_VERSION" --force

echo "--- system configs ---"
install -m 0644 "$SRC_DIR/modprobe.d-asb_drm.conf"    /etc/modprobe.d/asb_drm.conf
install -m 0644 "$SRC_DIR/modules-load.d-asb_drm.conf" /etc/modules-load.d/asb_drm.conf
install -m 0644 "$SRC_DIR/systemd-asb-evict-simpledrm.service" \
                                                /etc/systemd/system/asb-evict-simpledrm.service
systemctl daemon-reload
systemctl enable asb-evict-simpledrm.service

echo "--- rebuild initramfs (so hyperv_drm blacklist takes effect at boot) ---"
update-initramfs -u

cat <<EOF
--- done ---
The module is installed and will autoload on next boot.
To activate now without reboot:

    sudo modprobe -r hyperv_drm 2>/dev/null
    sudo modprobe asb_drm

Then verify with:
    lsmod | grep asb_drm
    ls /dev/dri/
    dmesg | grep -i 'AppSandbox virtual display'
EOF
