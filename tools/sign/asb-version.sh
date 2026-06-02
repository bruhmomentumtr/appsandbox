#!/bin/bash
#
# asb-version.sh -- Print the AppSandbox version from the single source of truth,
# Directory.Build.props. This is the ONLY place a version is defined; everything
# (Windows binaries, macOS app/framework, release zip names) derives from it.
#
#   asb-version.sh short   ->  0.1.1      (major.minor.patch)
#   asb-version.sh full    ->  0.1.1.0    (major.minor.patch.revision)
#
set -euo pipefail

# In an Xcode build phase, SRCROOT is the project dir (repo root). Standalone,
# derive the repo root from this script's location (tools/sign/..).
ROOT="${SRCROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
PROPS="$ROOT/Directory.Build.props"
[[ -f "$PROPS" ]] || { echo "asb-version: missing $PROPS" >&2; exit 1; }

num() { grep -oE "<$1>[0-9]+" "$PROPS" | grep -oE '[0-9]+' | head -1; }
MAJ="$(num AsbVersionMajor)"; MIN="$(num AsbVersionMinor)"
PATCHN="$(num AsbVersionPatch)"; REV="$(num AsbVersionRevision)"
[[ -n "$MAJ" && -n "$MIN" && -n "$PATCHN" && -n "$REV" ]] \
    || { echo "asb-version: could not parse AsbVersion* from $PROPS" >&2; exit 1; }

case "${1:-short}" in
    short) echo "$MAJ.$MIN.$PATCHN" ;;
    full)  echo "$MAJ.$MIN.$PATCHN.$REV" ;;
    *) echo "usage: asb-version.sh [short|full]" >&2; exit 2 ;;
esac
