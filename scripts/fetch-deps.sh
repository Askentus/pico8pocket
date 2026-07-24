#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="$PROJECT_ROOT/.deps"
SDK_DIR="$DEPS_DIR/openfgpaSDK"
FAKE08_DIR="$DEPS_DIR/fake-08"

SDK_URL="https://github.com/openfpgaOS/openfgpaSDK.git"
SDK_COMMIT="628a12b551ac8137373c477e97466b84d153d2af"
FAKE08_URL="https://github.com/jtothebell/fake-08.git"
FAKE08_COMMIT="814991a2571ad3970e386cef48f3b148aa1c27b9"
Z8LUA_COMMIT="e6928578d46b61fd5ea30cfcf547e855a30a0553"
Z8LUA_PATCHES=(
    "$PROJECT_ROOT/patches/z8lua-rv32-performance.patch"
    "$PROJECT_ROOT/patches/z8lua-env-fallback.patch"
)

fetch_one() {
    local name="$1" url="$2" commit="$3" destination="$4"

    if [[ -d "$destination/.git" ]]; then
        local current
        current="$(git -C "$destination" rev-parse HEAD)"
        if [[ "$current" != "$commit" ]]; then
            echo "$name is at $current, expected $commit" >&2
            echo "Move $destination aside and run make deps again." >&2
            return 1
        fi
        echo "$name: pinned revision already present"
        return 0
    fi

    if [[ -e "$destination" ]]; then
        echo "$destination exists but is not a Git checkout" >&2
        return 1
    fi

    git clone --no-checkout "$url" "$destination"
    git -C "$destination" fetch --depth 1 origin "$commit"
    git -C "$destination" checkout --detach "$commit"
    echo "$name: checked out $commit"
}

mkdir -p "$DEPS_DIR"
fetch_one "openfpgaOS SDK" "$SDK_URL" "$SDK_COMMIT" "$SDK_DIR"
fetch_one "Fake-08" "$FAKE08_URL" "$FAKE08_COMMIT" "$FAKE08_DIR"

git -C "$FAKE08_DIR" submodule update --init --depth 1 libs/z8lua
actual_z8lua_commit="$(git -C "$FAKE08_DIR/libs/z8lua" rev-parse HEAD)"
if [[ "$actual_z8lua_commit" != "$Z8LUA_COMMIT" ]]; then
    echo "z8lua is at $actual_z8lua_commit, expected $Z8LUA_COMMIT" >&2
    exit 1
fi
echo "z8lua: checked out $actual_z8lua_commit"

for z8lua_patch in "${Z8LUA_PATCHES[@]}"; do
    patch_name="$(basename "$z8lua_patch")"
    if git -C "$FAKE08_DIR/libs/z8lua" apply --reverse --check \
            "$z8lua_patch" >/dev/null 2>&1; then
        echo "z8lua: $patch_name already applied"
    elif git -C "$FAKE08_DIR/libs/z8lua" apply --check "$z8lua_patch"; then
        git -C "$FAKE08_DIR/libs/z8lua" apply "$z8lua_patch"
        echo "z8lua: applied $patch_name"
    else
        echo "z8lua: cannot apply $z8lua_patch" >&2
        exit 1
    fi
done
