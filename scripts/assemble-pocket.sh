#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_ROOT="$PROJECT_ROOT/.deps/openfgpaSDK"
FAKE08_ROOT="$PROJECT_ROOT/.deps/fake-08"
OUT="$PROJECT_ROOT/build/pocket/pico8pocket"
BUNDLE_LOCAL_CARTS="${BUNDLE_LOCAL_CARTS:-0}"
CORE_JSON="$PROJECT_ROOT/dist/core/core.json"
CORE_ID="$(jq -er '.core.metadata.author + "." + .core.metadata.shortname' \
    "$CORE_JSON")"
CORE_DIR="$OUT/Cores/$CORE_ID"
ASSET_DIR="$OUT/Assets/pico8pocket/common"
CART_DIRS=(
    "$PROJECT_ROOT/Доп игры"
    "$PROJECT_ROOT/Топ игры"
)

is_multicart_part() {
    case "$(basename "$1")" in
        mot_sorcerergame.p8|solitomb_game.p8.png|poom_one_level.p8.png) return 0 ;;
        *) return 1 ;;
    esac
}
INSTANCE_DIR="$OUT/Assets/pico8pocket/$CORE_ID"
PLATFORM_DIR="$OUT/Platforms"
ELF="$PROJECT_ROOT/.obj/pico8pocket/app.elf"

[[ -f "$ELF" ]] || { echo "Missing $ELF; run make pocket-elf first" >&2; exit 1; }
[[ -f "$SDK_ROOT/runtime/pocket/os20.rbf_r" ]] || { echo "Missing openfpgaOS runtime; run make deps" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$CORE_DIR" "$ASSET_DIR/cards" "$INSTANCE_DIR" "$PLATFORM_DIR"
cp "$PROJECT_ROOT"/dist/core/*.json "$CORE_DIR/"
cp "$PROJECT_ROOT/dist/platforms/pico8pocket.json" "$PLATFORM_DIR/"
cp "$SDK_ROOT/runtime/pocket/os20.rbf_r" "$CORE_DIR/"
cp "$SDK_ROOT/runtime/pocket/loader.bin" "$CORE_DIR/"
cp "$SDK_ROOT/runtime/pocket/os.bin" "$ASSET_DIR/"
cp "$ELF" "$ASSET_DIR/pico8pocket.elf"
cp "$PROJECT_ROOT/src/app/pico8pocket.ini" "$ASSET_DIR/"
cp "$PROJECT_ROOT/assets/p8p_slot_pad.bin" "$ASSET_DIR/p8p_pad5.bin"
cp "$PROJECT_ROOT/assets/p8p_slot_pad.bin" "$ASSET_DIR/p8p_pad6.bin"
cp "$PROJECT_ROOT/assets/p8p_slot_pad.bin" "$ASSET_DIR/p8p_pad7.ofsf"
cp "$PROJECT_ROOT/assets/cards/README.txt" "$ASSET_DIR/cards/"
cp "$PROJECT_ROOT/LICENSE" "$CORE_DIR/LICENSE.txt"
cp "$PROJECT_ROOT/THIRD_PARTY_NOTICES.md" "$CORE_DIR/THIRD_PARTY_NOTICES.txt"
cp "$SDK_ROOT/LICENSE" "$CORE_DIR/openfpgaOS-SDK-LICENSE.txt"
cp "$SDK_ROOT/NOTICE" "$CORE_DIR/openfpgaOS-SDK-NOTICE.txt"
cp "$FAKE08_ROOT/LICENSE.MD" "$CORE_DIR/Fake-08-LICENSE.txt"

cart_count=0
if [[ "$BUNDLE_LOCAL_CARTS" == "1" ]]; then
    while IFS= read -r -d '' cart; do
        destination="$ASSET_DIR/cards/$(basename "$cart")"
        cp "$cart" "$destination"
        cart_count=$((cart_count + 1))
    done < <(find "$PROJECT_ROOT/assets/cards" -maxdepth 1 -type f \
        \( -name '*.p8' -o -name '*.p8.png' -o -name '*.zip' \) -print0)

    for cart_dir in "${CART_DIRS[@]}"; do
        [[ -d "$cart_dir" ]] || continue
        while IFS= read -r -d '' cart; do
            if is_multicart_part "$cart"; then
                echo "Skipping unsupported multicart component: $(basename "$cart")"
                continue
            fi
            destination="$ASSET_DIR/cards/$(basename "$cart")"
            [[ ! -e "$destination" ]] || {
                echo "Duplicate bundled cartridge name: $(basename "$cart")" >&2
                exit 1
            }
            cp "$cart" "$destination"
            cart_count=$((cart_count + 1))
        done < <(find "$cart_dir" -maxdepth 1 -type f \
            \( -name '*.p8' -o -name '*.p8.png' -o -name '*.zip' \) -print0)
    done
fi
cp "$PROJECT_ROOT/dist/instance/pico8pocket.json" "$INSTANCE_DIR/"

echo "Pocket tree ready: $OUT ($cart_count local cartridges bundled)"
