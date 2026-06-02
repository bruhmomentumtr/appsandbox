#!/bin/bash
#
# stamp-version.sh -- Xcode build-phase script. Writes the version from
# Directory.Build.props (the single source of truth) into the built bundle's
# Info.plist. Runs on every build — Xcode GUI, xcodebuild, and Archive — so the
# only place a version is ever set is Directory.Build.props.
#
# Wired in as a "Stamp version" Run Script phase on the AppSandbox and
# AppSandboxCore targets (see project.pbxproj). Uses TARGET_BUILD_DIR /
# INFOPLIST_PATH from the build environment to find the product's Info.plist.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHORT="$("$HERE/asb-version.sh" short)"
FULL="$("$HERE/asb-version.sh" full)"

if [[ -z "${TARGET_BUILD_DIR:-}" || -z "${INFOPLIST_PATH:-}" ]]; then
    echo "stamp-version: not in an Xcode build (no TARGET_BUILD_DIR/INFOPLIST_PATH); nothing to do"
    exit 0
fi

PLIST="$TARGET_BUILD_DIR/$INFOPLIST_PATH"
[[ -f "$PLIST" ]] || { echo "stamp-version: no Info.plist at $PLIST (skipping)"; exit 0; }

PB=/usr/libexec/PlistBuddy
$PB -c "Set :CFBundleShortVersionString $SHORT" "$PLIST" 2>/dev/null \
    || $PB -c "Add :CFBundleShortVersionString string $SHORT" "$PLIST"
$PB -c "Set :CFBundleVersion $FULL" "$PLIST" 2>/dev/null \
    || $PB -c "Add :CFBundleVersion string $FULL" "$PLIST"

echo "stamp-version: ${INFOPLIST_PATH##*/} -> $SHORT ($FULL)"
