#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="${1:-$PROJECT_ROOT/build/pocket/pico8pocket}"
BUNDLE_LOCAL_CARTS="${BUNDLE_LOCAL_CARTS:-0}"
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

command -v jq >/dev/null || { echo "jq is required for APF validation" >&2; exit 1; }

core_jsons=()
while IFS= read -r file; do
    core_jsons+=("$file")
done < <(find "$TREE/Cores" -mindepth 2 -maxdepth 2 -name core.json -type f)

[[ ${#core_jsons[@]} -eq 1 ]] || {
    echo "Expected exactly one Cores/<Author>.<shortname>/core.json" >&2
    exit 1
}

CORE_JSON="${core_jsons[0]}"
CORE_DIR="$(dirname "$CORE_JSON")"
CORE_ID="$(jq -er '.core.metadata.author + "." + .core.metadata.shortname' "$CORE_JSON")"
PLATFORM_ID="$(jq -er '.core.metadata.platform_ids[0]' "$CORE_JSON")"

[[ "$(basename "$CORE_DIR")" == "$CORE_ID" ]] || {
    echo "Core folder must be $CORE_ID, got $(basename "$CORE_DIR")" >&2
    exit 1
}

for manifest in audio core data input interact variants video; do
    jq -e . "$CORE_DIR/$manifest.json" >/dev/null
done

SCALER_ORDER="$(jq -er '[.video.scaler_modes[] | "\(.width)x\(.height)"] | join(",")' \
    "$CORE_DIR/video.json")"
EXPECTED_SCALER_ORDER="320x240,320x200,320x224,320x256,320x288,400x300,256x240,640x480"
[[ "$SCALER_ORDER" == "$EXPECTED_SCALER_ORDER" ]] || {
    echo "openfpgaOS scaler slots are out of order: $SCALER_ORDER" >&2
    exit 1
}

[[ -f "$TREE/Platforms/$PLATFORM_ID.json" ]] || {
    echo "Missing Platforms/$PLATFORM_ID.json" >&2
    exit 1
}
jq -e . "$TREE/Platforms/$PLATFORM_ID.json" >/dev/null
jq -e '.platform.category == "Computer"' "$TREE/Platforms/$PLATFORM_ID.json" >/dev/null || {
    echo "Platform category must be Computer for AnalogueOS grouping" >&2
    exit 1
}

BITSTREAM="$(jq -er '.core.cores[0].filename' "$CORE_JSON")"
LOADER="$(jq -er '.core.cores[0].chip32_vm // .core.framework.chip32_vm' "$CORE_JSON")"
[[ -f "$CORE_DIR/$BITSTREAM" ]] || { echo "Missing $BITSTREAM" >&2; exit 1; }
[[ -f "$CORE_DIR/$LOADER" ]] || { echo "Missing $LOADER" >&2; exit 1; }

INSTANCE="$(jq -er '.data.data_slots[] | select(.id == 0) | .filename' "$CORE_DIR/data.json")"
INSTANCE_PATH="$TREE/Assets/$PLATFORM_ID/$CORE_ID/$INSTANCE"
[[ -f "$INSTANCE_PATH" ]] || { echo "Missing APF instance $INSTANCE_PATH" >&2; exit 1; }
jq -e '.instance.magic == "APF_VER_1"' "$INSTANCE_PATH" >/dev/null
jq -e '.instance.variant_select.select == false' "$INSTANCE_PATH" >/dev/null || {
    echo "Instance must contain the legacy variant_select used by Pocket 2.6" >&2
    exit 1
}
jq -e '.instance.core_select.id == 0 and .instance.core_select.select == false' \
    "$INSTANCE_PATH" >/dev/null || {
    echo "Instance must contain the documented core_select" >&2
    exit 1
}

for binding in \
    '1:os.bin' \
    '2:pico8pocket.ini' \
    '3:pico8pocket.elf' \
    '5:p8p_pad5.bin' \
    '6:p8p_pad6.bin' \
    '7:p8p_pad7.ofsf' \
    '8:pico8pocket.cfg'; do
    SLOT_ID="${binding%%:*}"
    SLOT_FILE="${binding#*:}"
    jq -e --argjson id "$SLOT_ID" --arg filename "$SLOT_FILE" \
        '.instance.data_slots[] | select(.id == $id and .filename == $filename)' \
        "$INSTANCE_PATH" >/dev/null
done

# os20 predates entry-resolved save commits.  Its fallback addresses the
# compact APF data table by entry number, so slots 0..8 must all be populated:
# entry 8 is the config/pre-save and entries 9..18 are Save 0..9.  Missing
# filler entries make fclose() commit a different slot; the state then appears
# valid until the core is restarted but never reaches the SD card.
EXPECTED_SLOT_ORDER="0,1,2,3,4,5,6,7,8,10,11,12,13,14,15,16,17,18,19"
ACTUAL_SLOT_ORDER="$(jq -er '[.data.data_slots[].id] | join(",")' \
    "$CORE_DIR/data.json")"
[[ "$ACTUAL_SLOT_ORDER" == "$EXPECTED_SLOT_ORDER" ]] || {
    echo "os20 persistent-slot layout is invalid: $ACTUAL_SLOT_ORDER" >&2
    exit 1
}

for padding_file in p8p_pad5.bin p8p_pad6.bin p8p_pad7.ofsf; do
    [[ -s "$TREE/Assets/$PLATFORM_ID/common/$padding_file" ]] || {
        echo "Missing non-empty os20 slot filler: $padding_file" >&2
        exit 1
    }
done

jq -e '.data.data_slots[] | select(.id == 4 and .required == true and .parameters == 129)' \
    "$CORE_DIR/data.json" >/dev/null

for index in $(seq 0 9); do
    id=$((10 + index))
    jq -e --argjson id "$id" --arg filename "pico8pocket_${index}.sav" \
        '.instance.data_slots[] | select(.id == $id and .filename == $filename)' \
        "$INSTANCE_PATH" >/dev/null
done

if [[ "$BUNDLE_LOCAL_CARTS" == "1" ]]; then
    for cart_dir in "$PROJECT_ROOT/assets/cards" "${CART_DIRS[@]}"; do
        [[ -d "$cart_dir" ]] || continue
        while IFS= read -r -d '' source_cart; do
            if is_multicart_part "$source_cart"; then
                bundled_cart="$TREE/Assets/$PLATFORM_ID/common/cards/$(basename "$source_cart")"
                [[ ! -e "$bundled_cart" ]] || {
                    echo "Unsupported multicart component was bundled: $(basename "$source_cart")" >&2
                    exit 1
                }
                continue
            fi
            bundled_cart="$TREE/Assets/$PLATFORM_ID/common/cards/$(basename "$source_cart")"
            [[ -f "$bundled_cart" ]] || {
                echo "Missing bundled cartridge: $(basename "$source_cart")" >&2
                exit 1
            }
            cmp -s "$source_cart" "$bundled_cart" || {
                echo "Bundled cartridge differs: $(basename "$source_cart")" >&2
                exit 1
            }
        done < <(find "$cart_dir" -maxdepth 1 -type f \
            \( -name '*.p8' -o -name '*.p8.png' -o -name '*.zip' \) -print0)
    done
else
    if find "$TREE/Assets/$PLATFORM_ID/common/cards" -maxdepth 1 -type f \
        \( -name '*.p8' -o -name '*.p8.png' -o -name '*.zip' \) | grep -q .; then
        echo "Public package contains a cartridge" >&2
        exit 1
    fi
fi

for notice in LICENSE.txt THIRD_PARTY_NOTICES.txt openfpgaOS-SDK-LICENSE.txt \
    openfpgaOS-SDK-NOTICE.txt Fake-08-LICENSE.txt; do
    [[ -s "$CORE_DIR/$notice" ]] || {
        echo "Missing release notice: $notice" >&2
        exit 1
    }
done

echo "APF package validation passed: $CORE_ID on $PLATFORM_ID"
