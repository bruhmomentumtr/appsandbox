#!/bin/bash
#
# make-release-mac.sh -- Build, sign, notarize, staple, and zip the macOS app.
#
# Mirrors the Windows tools/sign/make-release.ps1 flow. Reads tunable settings
# from mac-release-config.json (committed, no secrets) and private notarization
# details from mac-signing.local.json (git-ignored; matches tools/sign/*.local.*).
#
# Build-time signing identity/team come from src/app_mac/xcconfig/LocalUser.xcconfig
# (also git-ignored). See tools/sign/SIGNING_MAC.md for the full setup.
#
# Usage:
#   tools/sign/make-release-mac.sh [--no-notarize]
#
#   --no-notarize   Build, sign, verify, and zip only (skip Apple notarization).
#                   Useful for a quick local Developer ID build.
#
set -euo pipefail

# --- locate repo root (script lives in tools/sign/) ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

CONFIG="$SCRIPT_DIR/mac-release-config.json"
LOCAL="$SCRIPT_DIR/mac-signing.local.json"

NOTARIZE=1
[[ "${1:-}" == "--no-notarize" ]] && NOTARIZE=0

# --- helpers ---
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[36m==> %s\033[0m\n' "$*"; }
# read a key from a json file via plutil (built into macOS); empty string if absent
jget() { plutil -extract "$2" raw -o - "$1" 2>/dev/null || true; }

[[ -f "$CONFIG" ]] || die "missing $CONFIG"
if [[ ! -f "$LOCAL" ]]; then
    cat >&2 <<EOF
error: missing $LOCAL

Create it (git-ignored) with your notarization details, e.g.:

  {
    "notaryKeychainProfile": "appsandbox-notary",
    "appleId": "you@example.com",
    "teamId": "YOURTEAMID",
    "signingIdentity": "Developer ID Application: Your Org (YOURTEAMID)"
  }

The keychain profile is created once with:
  xcrun notarytool store-credentials appsandbox-notary \\
      --apple-id you@example.com --team-id YOURTEAMID --password <app-specific-password>

Alternatively omit notaryKeychainProfile and provide appleId + teamId +
appSpecificPassword directly in this file. See tools/sign/SIGNING_MAC.md.
EOF
    exit 1
fi

PROJECT="$(jget "$CONFIG" project)"
SCHEME="$(jget "$CONFIG" scheme)"
CONFIGURATION="$(jget "$CONFIG" configuration)"
APP_PATH="$(jget "$CONFIG" appPath)"
PACKAGE_DIR="$(jget "$CONFIG" packageDir)"
ZIP_NAME="$(jget "$CONFIG" zipName)"
EXPECTED_IDENTITY="$(jget "$CONFIG" expectedIdentity)"

PROFILE="$(jget "$LOCAL" notaryKeychainProfile)"
APPLE_ID="$(jget "$LOCAL" appleId)"
TEAM_ID="$(jget "$LOCAL" teamId)"
APP_PW="$(jget "$LOCAL" appSpecificPassword)"

[[ -n "$PROJECT" && -n "$SCHEME" && -n "$CONFIGURATION" && -n "$APP_PATH" && -n "$PACKAGE_DIR" && -n "$ZIP_NAME" ]] \
    || die "mac-release-config.json missing required keys"

PKG_ZIP="$PACKAGE_DIR/$ZIP_NAME"
# Working zip used only for notarization submission; never shipped.
SUBMIT_ZIP="$(dirname "$APP_PATH")/.notarize-submit.zip"

# Publish ONLY the final zip into a clean package dir — nothing else (no dSYMs,
# no loose products, no build scratch) ever lands there. This is a production
# release; the folder must contain only what the public should see.
publish_package() {
    rm -rf "$PACKAGE_DIR"
    mkdir -p "$PACKAGE_DIR"
    ditto -c -k --keepParent "$APP_PATH" "$PKG_ZIP"
    rm -f "$SUBMIT_ZIP"
}

# --- 1. build ---
step "Syncing version from Directory.Build.props"
"$SCRIPT_DIR/sync-mac-version.sh"

step "Building $SCHEME ($CONFIGURATION)"
xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration "$CONFIGURATION" clean >/dev/null
xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration "$CONFIGURATION" build >/dev/null
[[ -d "$APP_PATH" ]] || die "build produced no app at $APP_PATH"

# --- 2. verify signature ---
step "Verifying signature"
codesign --verify --deep --strict --verbose=1 "$APP_PATH"
ACTUAL_AUTH="$(codesign -dvvv "$APP_PATH" 2>&1 | grep '^Authority=' | head -1 | sed 's/^Authority=//')"
echo "    identity: $ACTUAL_AUTH"
if [[ -n "$EXPECTED_IDENTITY" && "$ACTUAL_AUTH" != "$EXPECTED_IDENTITY"* ]]; then
    die "signing identity '$ACTUAL_AUTH' does not match expected '$EXPECTED_IDENTITY' (check LocalUser.xcconfig)"
fi

if [[ "$NOTARIZE" -eq 0 ]]; then
    # Local signed build only — publish the signed (not notarized) zip.
    step "Packaging signed (NOT notarized) app to $PKG_ZIP"
    publish_package
    step "Done (--no-notarize): signed app at $APP_PATH, package at $PKG_ZIP"
    exit 0
fi

# --- 3. zip for notarization submission (working copy, not shipped) ---
step "Zipping for submission"
rm -f "$SUBMIT_ZIP"
ditto -c -k --keepParent "$APP_PATH" "$SUBMIT_ZIP"

# --- 4. notarize (recursive — covers the framework + iso-patch-mac +
#         appsandbox-agent + appsandbox-clipboard nested in the .app) ---
step "Submitting to Apple notary service"
if [[ -n "$PROFILE" ]]; then
    xcrun notarytool submit "$SUBMIT_ZIP" --keychain-profile "$PROFILE" --wait
elif [[ -n "$APPLE_ID" && -n "$TEAM_ID" && -n "$APP_PW" ]]; then
    xcrun notarytool submit "$SUBMIT_ZIP" --apple-id "$APPLE_ID" --team-id "$TEAM_ID" --password "$APP_PW" --wait
else
    rm -f "$SUBMIT_ZIP"
    die "no notarization credentials: set notaryKeychainProfile, or appleId+teamId+appSpecificPassword in $LOCAL"
fi

# --- 5. staple + verify ---
step "Stapling ticket"
xcrun stapler staple "$APP_PATH"

step "Gatekeeper assessment"
spctl -a -t exec -vvv "$APP_PATH"

# --- 6. publish ONLY the final stapled zip into a clean package folder ---
step "Packaging release to $PKG_ZIP"
publish_package

step "Release complete: notarized + stapled. Public package: $PKG_ZIP"
