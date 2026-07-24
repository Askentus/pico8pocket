#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_ROOT="${SDK_ROOT:-$PROJECT_ROOT/.deps/openfgpaSDK}"
IMG="${SDK_IMG:-openfpgaos-firmware}"
DOCKERFILE="${SDK_DOCKERFILE:-$SDK_ROOT/tools/docker/Dockerfile.firmware}"

source "$SDK_ROOT/tools/oci.sh"

WORKDIR="$(pwd -P)"
if [[ "${1:-}" == "-C" ]]; then
    WORKDIR="$(cd "$2" && pwd -P)"
    shift 2
fi
[[ $# -ge 1 ]] || { echo "usage: sdk-container.sh [-C dir] command [args...]" >&2; exit 2; }

if ! oci_image_exists "$IMG"; then
    [[ -f "$DOCKERFILE" ]] || { echo "Missing $DOCKERFILE" >&2; exit 1; }
    echo "[sdk] building $IMG (one-time)"
    oci_build -t "$IMG" -f "$DOCKERFILE" "$(dirname "$DOCKERFILE")"
fi

tty_args=()
[[ -t 0 && -t 1 ]] && tty_args=(-i -t)
stdin_source=/dev/null
[[ ${#tty_args[@]} -gt 0 ]] && stdin_source=/dev/tty

container_name="pico8pocket-sdk-$$"
trap 'oci_rm_force "$container_name"' EXIT INT TERM

oci_run --rm --name "$container_name" "${tty_args[@]}" \
    --user "$(id -u):$(id -g)" \
    -v "$PROJECT_ROOT:$PROJECT_ROOT" \
    --tmpfs /sdkhome:exec \
    -e HOME=/sdkhome \
    -e CPATH= \
    -e OF_SDK_IN_CONTAINER=1 \
    -e "MAKEFLAGS=${MAKEFLAGS:-}" \
    -w "$WORKDIR" \
    "$IMG" "$@" < "$stdin_source"
