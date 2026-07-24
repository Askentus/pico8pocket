#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT="$PROJECT_ROOT/build/pocket/pico8pocket"
RELEASES="$PROJECT_ROOT/releases"
VERSION="$(jq -er '.core.metadata.version' "$PROJECT_ROOT/dist/core/core.json")"
if [[ "${BUNDLE_LOCAL_CARTS:-0}" == "1" ]]; then
    OUTPUT="$RELEASES/pico8pocket-v${VERSION}-local.zip"
else
    OUTPUT="$RELEASES/pico8pocket-v${VERSION}.zip"
fi

[[ -d "$INPUT/Cores" ]] || { echo "Missing Pocket tree; run make pocket" >&2; exit 1; }
mkdir -p "$RELEASES"
rm -f "$OUTPUT"
(cd "$INPUT" && zip -r "$OUTPUT" Cores Assets Platforms >/dev/null)
echo "Package created: $OUTPUT"
