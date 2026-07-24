#include "p8p/menu.h"

#include "p8p/platform.h"
#include "p8p/state_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_WIDTH 128
#define MENU_HEIGHT 128

enum menu_screen {
    SCREEN_MAIN,
    SCREEN_STATES,
    SCREEN_CONTROLS,
    SCREEN_DISPLAY,
    SCREEN_AUDIO,
    SCREEN_INFO,
    SCREEN_MODAL
};

enum modal_kind {
    MODAL_NONE,
    MODAL_RESTART,
    MODAL_EXIT,
    MODAL_DELETE,
    MODAL_CART_PICKER
};

struct p8p_menu {
    p8p_settings_t *settings;
    p8p_cart_hash_t cart_hash;
    char cart_kind[16];
    uint8_t framebuffer[MENU_WIDTH * MENU_HEIGHT];
    uint8_t background[MENU_WIDTH * MENU_HEIGHT];
    p8p_state_meta_t states[P8P_STATE_SLOT_COUNT];
    uint16_t previous_buttons;
    int screen;
    int cursor;
    int modal;
    int modal_slot;
    int capture_action;
    int notice_frames;
    char notice[32];
};

static uint16_t glyph_ascii(uint32_t character) {
#define GLYPH(r0, r1, r2, r3, r4) \
    ((uint16_t)((r0) << 12) | (uint16_t)((r1) << 9) | \
     (uint16_t)((r2) << 6) | (uint16_t)((r3) << 3) | (uint16_t)(r4))
    if (character >= 'a' && character <= 'z')
        character -= 'a' - 'A';
    switch (character) {
    case '0': return GLYPH(2, 5, 5, 5, 2);
    case '1': return GLYPH(2, 6, 2, 2, 7);
    case '2': return GLYPH(6, 1, 2, 4, 7);
    case '3': return GLYPH(6, 1, 2, 1, 6);
    case '4': return GLYPH(5, 5, 7, 1, 1);
    case '5': return GLYPH(7, 4, 6, 1, 6);
    case '6': return GLYPH(3, 4, 6, 5, 2);
    case '7': return GLYPH(7, 1, 2, 2, 2);
    case '8': return GLYPH(2, 5, 2, 5, 2);
    case '9': return GLYPH(2, 5, 3, 1, 6);
    case 'A': return GLYPH(2, 5, 7, 5, 5);
    case 'B': return GLYPH(6, 5, 6, 5, 6);
    case 'C': return GLYPH(3, 4, 4, 4, 3);
    case 'D': return GLYPH(6, 5, 5, 5, 6);
    case 'E': return GLYPH(7, 4, 6, 4, 7);
    case 'F': return GLYPH(7, 4, 6, 4, 4);
    case 'G': return GLYPH(3, 4, 5, 5, 3);
    case 'H': return GLYPH(5, 5, 7, 5, 5);
    case 'I': return GLYPH(7, 2, 2, 2, 7);
    case 'J': return GLYPH(1, 1, 1, 5, 2);
    case 'K': return GLYPH(5, 5, 6, 5, 5);
    case 'L': return GLYPH(4, 4, 4, 4, 7);
    case 'M': return GLYPH(5, 7, 7, 5, 5);
    case 'N': return GLYPH(5, 7, 7, 7, 5);
    case 'O': return GLYPH(2, 5, 5, 5, 2);
    case 'P': return GLYPH(6, 5, 6, 4, 4);
    case 'Q': return GLYPH(2, 5, 5, 3, 1);
    case 'R': return GLYPH(6, 5, 6, 5, 5);
    case 'S': return GLYPH(3, 4, 2, 1, 6);
    case 'T': return GLYPH(7, 2, 2, 2, 2);
    case 'U': return GLYPH(5, 5, 5, 5, 7);
    case 'V': return GLYPH(5, 5, 5, 5, 2);
    case 'W': return GLYPH(5, 5, 7, 7, 5);
    case 'X': return GLYPH(5, 5, 2, 5, 5);
    case 'Y': return GLYPH(5, 5, 2, 2, 2);
    case 'Z': return GLYPH(7, 1, 2, 4, 7);
    case '!': return GLYPH(2, 2, 2, 0, 2);
    case '?': return GLYPH(6, 1, 2, 0, 2);
    case '.': return GLYPH(0, 0, 0, 0, 2);
    case ',': return GLYPH(0, 0, 0, 2, 4);
    case ':': return GLYPH(0, 2, 0, 2, 0);
    case '-': return GLYPH(0, 0, 7, 0, 0);
    case '+': return GLYPH(0, 2, 7, 2, 0);
    case '/': return GLYPH(1, 1, 2, 4, 4);
    case '<': return GLYPH(1, 2, 4, 2, 1);
    case '>': return GLYPH(4, 2, 1, 2, 4);
    case '=': return GLYPH(0, 7, 0, 7, 0);
    case '_': return GLYPH(0, 0, 0, 0, 7);
    default: return 0;
    }
#undef GLYPH
}

static uint16_t glyph_cyrillic(uint32_t c) {
#define G(a,b,c,d,e) ((uint16_t)((a)<<12)|(uint16_t)((b)<<9)| \
    (uint16_t)((c)<<6)|(uint16_t)((d)<<3)|(uint16_t)(e))
    if (c >= 0x430 && c <= 0x44f) c -= 0x20;
    switch (c) {
    case 0x410: return G(2,5,7,5,5); /* А */
    case 0x411: return G(7,4,6,5,6); /* Б */
    case 0x412: return G(6,5,6,5,6); /* В */
    case 0x413: return G(7,4,4,4,4); /* Г */
    case 0x414: return G(2,5,5,7,5); /* Д */
    case 0x415: case 0x401: return G(7,4,6,4,7); /* Е Ё */
    case 0x416: return G(5,5,2,5,5); /* Ж */
    case 0x417: return G(6,1,2,1,6); /* З */
    case 0x418: case 0x419: return G(5,5,7,7,5); /* И Й */
    case 0x41a: return G(5,5,6,5,5); /* К */
    case 0x41b: return G(2,5,5,5,5); /* Л */
    case 0x41c: return G(5,7,7,5,5); /* М */
    case 0x41d: return G(5,5,7,5,5); /* Н */
    case 0x41e: return G(2,5,5,5,2); /* О */
    case 0x41f: return G(7,5,5,5,5); /* П */
    case 0x420: return G(6,5,6,4,4); /* Р */
    case 0x421: return G(3,4,4,4,3); /* С */
    case 0x422: return G(7,2,2,2,2); /* Т */
    case 0x423: return G(5,5,3,1,6); /* У */
    case 0x424: return G(2,7,7,7,2); /* Ф */
    case 0x425: return G(5,5,2,5,5); /* Х */
    case 0x426: return G(5,5,5,5,7); /* Ц */
    case 0x427: return G(5,5,3,1,1); /* Ч */
    case 0x428: case 0x429: return G(5,5,5,5,7); /* Ш Щ */
    case 0x42a: return G(4,4,6,5,6); /* Ъ */
    case 0x42b: return G(5,5,6,5,6); /* Ы */
    case 0x42c: return G(4,4,6,5,6); /* Ь */
    case 0x42d: return G(6,1,3,1,6); /* Э */
    case 0x42e: return G(6,5,7,5,6); /* Ю */
    case 0x42f: return G(3,5,3,5,5); /* Я */
    default: return 0;
    }
#undef G
}

static uint32_t utf8_next(const char **text) {
    const uint8_t *p = (const uint8_t *)*text;
    uint32_t c;
    if (*p < 0x80) {
        *text += *p ? 1 : 0;
        return *p;
    }
    if ((*p & 0xe0) == 0xc0 && p[1]) {
        c = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        *text += 2;
        return c;
    }
    if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
        c = ((uint32_t)(p[0] & 0x0f) << 12) |
            ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *text += 3;
        return c;
    }
    ++*text;
    return '?';
}

static void fill(uint8_t *fb, int x, int y, int w, int h, uint8_t color) {
    for (int py = 0; py < h; ++py) {
        int dy = y + py;
        if ((unsigned)dy >= MENU_HEIGHT) continue;
        for (int px = 0; px < w; ++px) {
            int dx = x + px;
            if ((unsigned)dx < MENU_WIDTH)
                fb[dy * MENU_WIDTH + dx] = color;
        }
    }
}

static void text(uint8_t *fb, int x, int y, const char *value, uint8_t color) {
    while (value && *value && x <= MENU_WIDTH - 3) {
        uint32_t c = utf8_next(&value);
        uint16_t bits = c < 128 ? glyph_ascii(c) : glyph_cyrillic(c);
        for (int row = 0; row < 5; ++row)
            for (int column = 0; column < 3; ++column)
                if ((bits & (1u << (14 - row * 3 - column))) &&
                    (unsigned)(x + column) < MENU_WIDTH &&
                    (unsigned)(y + row) < MENU_HEIGHT)
                    fb[(y + row) * MENU_WIDTH + x + column] = color;
        x += 4;
    }
}

static const char *tr(const p8p_menu_t *menu, const char *english,
                      const char *russian) {
    return menu->settings->language ? russian : english;
}

static void menu_base(p8p_menu_t *menu, const char *title) {
    memcpy(menu->framebuffer, menu->background, sizeof(menu->framebuffer));
    for (int y = 0; y < MENU_HEIGHT; ++y)
        for (int x = (y & 1); x < MENU_WIDTH; x += 2)
            menu->framebuffer[y * MENU_WIDTH + x] = 0;
    fill(menu->framebuffer, 0, 0, 128, 9, 1);
    text(menu->framebuffer, 3, 2, title, 7);
}

static void selected_row(p8p_menu_t *menu, int row, int y, const char *label,
                         int enabled) {
    if (row == menu->cursor)
        fill(menu->framebuffer, 1, y - 1, 126, 7, enabled ? 12 : 5);
    text(menu->framebuffer, 4, y, label, enabled ? 7 : 6);
}

static void set_notice(p8p_menu_t *menu, const char *message) {
    snprintf(menu->notice, sizeof(menu->notice), "%s", message ? message : "");
    menu->notice_frames = 90;
}

static const char *slot_name(const p8p_menu_t *menu, int slot, char out[16]) {
    if (slot == P8P_STATE_QUICK)
        return tr(menu, "QUICK", "БЫСТРОЕ");
    snprintf(out, 16, "%s %d", tr(menu, "SLOT", "СЛОТ"), slot);
    return out;
}

static void refresh_state(p8p_menu_t *menu, int slot) {
    (void)p8p_state_get_meta(slot, &menu->cart_hash, &menu->states[slot]);
}

static void refresh_states(p8p_menu_t *menu) {
    for (int slot = 0; slot < P8P_STATE_SLOT_COUNT; ++slot)
        refresh_state(menu, slot);
}

static void draw_main(p8p_menu_t *menu) {
    static const char *en[] = {
        "RESUME", "QUICK SAVE", "QUICK LOAD", "STATES", "RESTART CART",
        "SELECT CART", "CONTROLS", "DISPLAY", "AUDIO", "LANGUAGE",
        "CART INFO", "EXIT"
    };
    static const char *ru[] = {
        "ПРОДОЛЖИТЬ", "БЫСТ.СОХР", "БЫСТ.ЗАГР", "СОСТОЯНИЯ",
        "ПЕРЕЗАПУСК", "ВЫБРАТЬ ИГРУ", "УПРАВЛЕНИЕ", "ЭКРАН", "ЗВУК",
        "ЯЗЫК", "ОБ ИГРЕ", "ВЫХОД"
    };
    menu_base(menu, "PICO-8 POCKET");
    for (int row = 0; row < 12; ++row)
        selected_row(menu, row, 12 + row * 8,
                     menu->settings->language ? ru[row] : en[row],
                     row != 2 || menu->states[P8P_STATE_QUICK].exists);
    text(menu->framebuffer, 97, 121, "X FPS", 6);
    if (menu->notice_frames > 0) {
        fill(menu->framebuffer, 0, 112, 128, 8, 1);
        text(menu->framebuffer, 3, 114, menu->notice, 10);
    }
}

static void draw_states(p8p_menu_t *menu) {
    char label[16];
    menu_base(menu, tr(menu, "SAVE STATES", "СОСТОЯНИЯ"));
    fill(menu->framebuffer, 63, 11, 65, 66, 1);
    for (int row = 0; row < P8P_STATE_SLOT_COUNT; ++row) {
        const char *name = slot_name(menu, row, label);
        if (row == menu->cursor)
            fill(menu->framebuffer, 1, 11 + row * 7, 61, 7, 12);
        text(menu->framebuffer, 3, 12 + row * 7, name,
             menu->states[row].exists ? 7 : 6);
    }
    if (menu->states[menu->cursor].exists) {
        const uint8_t *thumb = menu->states[menu->cursor].thumbnail;
        for (int y = 0; y < 64; ++y)
            memcpy(menu->framebuffer + (12 + y) * 128 + 64,
                   thumb + y * 64, 64);
    } else {
        text(menu->framebuffer, 83, 40, tr(menu, "EMPTY", "ПУСТО"), 6);
    }
    text(menu->framebuffer, 3, 86, tr(menu, "A LOAD", "A ЗАГР"), 7);
    text(menu->framebuffer, 3, 94, tr(menu, "X SAVE", "X СОХР"), 7);
    text(menu->framebuffer, 3, 102, tr(menu, "Y DELETE", "Y УДАЛ"), 7);
    text(menu->framebuffer, 3, 118, tr(menu, "B BACK", "B НАЗАД"), 6);
    if (menu->notice_frames > 0)
        text(menu->framebuffer, 66, 86, menu->notice, 10);
}

static void button_names(uint16_t mask, char out[16]) {
    static const struct { uint16_t bit; const char *name; } names[] = {
        {P8P_PHYS_LEFT,"LEFT"}, {P8P_PHYS_RIGHT,"RIGHT"},
        {P8P_PHYS_UP,"UP"}, {P8P_PHYS_DOWN,"DOWN"}, {P8P_PHYS_A,"A"},
        {P8P_PHYS_B,"B"}, {P8P_PHYS_X,"X"}, {P8P_PHYS_Y,"Y"},
        {P8P_PHYS_L,"L"}, {P8P_PHYS_R,"R"}, {P8P_PHYS_START,"START"}
    };
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!(mask & names[i].bit)) continue;
        int n = snprintf(out + used, 16 - used, "%s%s", used ? "/" : "",
                         names[i].name);
        if (n < 0 || (size_t)n >= 16 - used) break;
        used += (size_t)n;
    }
    if (!used) snprintf(out, 16, "NONE");
}

static const p8p_control_profile_t *active_controls(p8p_menu_t *menu,
                                                    int *override) {
    return p8p_settings_controls(menu->settings, &menu->cart_hash, override);
}

static void draw_controls(p8p_menu_t *menu) {
    static const char *actions_en[] = {"LEFT","RIGHT","UP","DOWN","P8 O","P8 X","PAUSE"};
    static const char *actions_ru[] = {"ВЛЕВО","ВПРАВО","ВВЕРХ","ВНИЗ","P8 O","P8 X","ПАУЗА"};
    char row[32];
    char buttons[16];
    int has_override;
    const p8p_control_profile_t *profile = active_controls(menu, &has_override);
    menu_base(menu, tr(menu, "CONTROLS", "УПРАВЛЕНИЕ"));
    snprintf(row, sizeof(row), "%s: %s", tr(menu, "PROFILE", "ПРОФИЛЬ"),
             has_override ? tr(menu, "CART", "ИГРА") :
                            tr(menu, "GLOBAL", "ОБЩИЙ"));
    selected_row(menu, 0, 11, row, 1);
    for (int action = 0; action < P8P_CONTROL_ACTIONS; ++action) {
        button_names(profile->physical[action], buttons);
        snprintf(row, sizeof(row), "%s:%s",
                 menu->settings->language ? actions_ru[action] : actions_en[action],
                 buttons);
        selected_row(menu, action + 1, 19 + action * 7, row, 1);
    }
    selected_row(menu, 8, 70, tr(menu, "COPY GLOBAL", "КОПИЯ ОБЩЕГО"), 1);
    selected_row(menu, 9, 77, tr(menu, "RESET DEFAULT", "СБРОС"), 1);
    selected_row(menu, 10, 84, tr(menu, "MAKE GLOBAL", "СДЕЛАТЬ ОБЩИМ"), 1);
    selected_row(menu, 11, 91, tr(menu, "USE GLOBAL", "УБРАТЬ ПРОФИЛЬ"),
                 has_override);
    selected_row(menu, 12, 98, tr(menu, "BACK", "НАЗАД"), 1);
    if (menu->capture_action >= 0) {
        fill(menu->framebuffer, 6, 106, 116, 17, 1);
        text(menu->framebuffer, 10, 110,
             tr(menu, "PRESS A BUTTON", "НАЖМИ КНОПКУ"), 10);
        text(menu->framebuffer, 10, 117,
             tr(menu, "SELECT CANCEL", "SELECT ОТМЕНА"), 6);
    } else if (menu->notice_frames > 0) {
        text(menu->framebuffer, 4, 113, menu->notice, 10);
    }
}

static void draw_display(p8p_menu_t *menu) {
    char row[32];
    const char *scale = menu->settings->scale == 0 ? "AUTO" :
                        menu->settings->scale == 1 ? "1X" :
                        menu->settings->scale == 2 ? "2X" : "FULL SOFT";
    menu_base(menu, tr(menu, "DISPLAY", "ЭКРАН"));
    snprintf(row, sizeof(row), "%s: %s", tr(menu, "SCALE", "МАСШТАБ"), scale);
    selected_row(menu, 0, 18, row, 1);
    selected_row(menu, 1, 28, tr(menu, "BACK", "НАЗАД"), 1);
    text(menu->framebuffer, 4, 44, menu->settings->scale == 3 ?
         tr(menu, "SHARP BILINEAR", "МЯГКИЙ 1440X") :
         tr(menu, "PIXEL PERFECT", "ПИКСЕЛЬ В ПИКСЕЛЬ"), 6);
}

static void draw_audio(p8p_menu_t *menu) {
    char row[32];
    menu_base(menu, tr(menu, "AUDIO", "ЗВУК"));
    snprintf(row, sizeof(row), "%s: %u", tr(menu, "VOLUME", "ГРОМКОСТЬ"),
             (unsigned)menu->settings->volume);
    selected_row(menu, 0, 18, row, 1);
    snprintf(row, sizeof(row), "%s: %s", tr(menu, "MUTE", "БЕЗ ЗВУКА"),
             menu->settings->muted ? tr(menu, "ON", "ДА") : tr(menu, "OFF", "НЕТ"));
    selected_row(menu, 1, 28, row, 1);
    selected_row(menu, 2, 38, tr(menu, "BACK", "НАЗАД"), 1);
}

static void draw_info(p8p_menu_t *menu, p8p_runtime_t *runtime) {
    char line[40];
    menu_base(menu, tr(menu, "CART INFO", "ОБ ИГРЕ"));
    snprintf(line, sizeof(line), "%s: %s", tr(menu, "FORMAT", "ФОРМАТ"),
             menu->cart_kind);
    text(menu->framebuffer, 4, 16, line, 7);
    snprintf(line, sizeof(line), "FPS: %d", p8p_runtime_target_fps(runtime));
    text(menu->framebuffer, 4, 26, line, 7);
    text(menu->framebuffer, 4, 40, "CONTENT HASH", 6);
    for (int half = 0; half < 2; ++half) {
        char hex[17];
        for (int i = 0; i < 8; ++i)
            snprintf(hex + i * 2, 3, "%02X", menu->cart_hash.bytes[half * 8 + i]);
        text(menu->framebuffer, 4, 49 + half * 8, hex, 7);
    }
    text(menu->framebuffer, 4, 114, tr(menu, "B BACK", "B НАЗАД"), 6);
}

static void draw_modal(p8p_menu_t *menu) {
    menu_base(menu, tr(menu, "CONFIRM", "ПОДТВЕРЖДЕНИЕ"));
    fill(menu->framebuffer, 6, 30, 116, 65, 1);
    if (menu->modal == MODAL_RESTART) {
        text(menu->framebuffer, 14, 43,
             tr(menu, "RESTART CART?", "ПЕРЕЗАПУСТИТЬ?"), 7);
    } else if (menu->modal == MODAL_EXIT) {
        text(menu->framebuffer, 14, 43,
             tr(menu, "EXIT TO OS?", "ВЫЙТИ В ОС?"), 7);
    } else if (menu->modal == MODAL_DELETE) {
        char name[16];
        char line[32];
        snprintf(line, sizeof(line), "%s %s?", tr(menu, "DELETE", "УДАЛИТЬ"),
                 slot_name(menu, menu->modal_slot, name));
        text(menu->framebuffer, 14, 43, line, 7);
    } else if (menu->modal == MODAL_CART_PICKER) {
        text(menu->framebuffer, 10, 36,
             tr(menu, "SELECT NEW CART", "ВЫБЕРИ НОВУЮ ИГРУ"), 10);
        text(menu->framebuffer, 10, 48,
             tr(menu, "PRESS ANALOGUE", "НАЖМИ ANALOGUE"), 7);
        text(menu->framebuffer, 10, 56, "CORE SETTINGS", 7);
        text(menu->framebuffer, 10, 64, "CARTRIDGE", 7);
        text(menu->framebuffer, 10, 80,
             tr(menu, "B CANCEL", "B ОТМЕНА"), 6);
        return;
    }
    text(menu->framebuffer, 18, 72,
         tr(menu, "A YES   B NO", "A ДА   B НЕТ"), 10);
}

static void draw_menu(p8p_menu_t *menu, p8p_runtime_t *runtime) {
    switch (menu->screen) {
    case SCREEN_MAIN: draw_main(menu); break;
    case SCREEN_STATES: draw_states(menu); break;
    case SCREEN_CONTROLS: draw_controls(menu); break;
    case SCREEN_DISPLAY: draw_display(menu); break;
    case SCREEN_AUDIO: draw_audio(menu); break;
    case SCREEN_INFO: draw_info(menu, runtime); break;
    case SCREEN_MODAL: draw_modal(menu); break;
    }
    if (menu->notice_frames > 0)
        --menu->notice_frames;
}

p8p_menu_t *p8p_menu_create(p8p_settings_t *settings,
                            const p8p_cart_hash_t *cart_hash,
                            const char *cart_kind) {
    p8p_menu_t *menu;
    if (!settings || !cart_hash)
        return NULL;
    menu = (p8p_menu_t *)calloc(1, sizeof(*menu));
    if (!menu)
        return NULL;
    menu->settings = settings;
    menu->cart_hash = *cart_hash;
    menu->capture_action = -1;
    snprintf(menu->cart_kind, sizeof(menu->cart_kind), "%s",
             cart_kind ? cart_kind : "UNKNOWN");
    return menu;
}

void p8p_menu_destroy(p8p_menu_t *menu) {
    free(menu);
}

void p8p_menu_open(p8p_menu_t *menu, p8p_runtime_t *runtime,
                   uint16_t physical_buttons) {
    const uint8_t *source;
    const uint8_t *palette;
    if (!menu || !runtime)
        return;
    source = p8p_runtime_framebuffer(runtime);
    palette = p8p_runtime_screen_palette(runtime);
    for (int i = 0; i < MENU_WIDTH * MENU_HEIGHT; ++i) {
        uint8_t color = palette ? palette[source[i] & 15] : source[i];
        menu->background[i] = p8p_platform_dim_color(color);
    }
    menu->previous_buttons = physical_buttons;
    menu->screen = SCREEN_MAIN;
    menu->cursor = 0;
    menu->modal = MODAL_NONE;
    menu->capture_action = -1;
    refresh_state(menu, P8P_STATE_QUICK);
    draw_menu(menu, runtime);
}

static void enter_screen(p8p_menu_t *menu, int screen, int cursor) {
    menu->screen = screen;
    menu->cursor = cursor;
    menu->capture_action = -1;
}

static uint16_t first_remappable_button(uint16_t buttons) {
    static const uint16_t order[] = {
        P8P_PHYS_LEFT, P8P_PHYS_RIGHT, P8P_PHYS_UP, P8P_PHYS_DOWN,
        P8P_PHYS_A, P8P_PHYS_B, P8P_PHYS_X, P8P_PHYS_Y,
        P8P_PHYS_L, P8P_PHYS_R, P8P_PHYS_START
    };
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
        if (buttons & order[i]) return order[i];
    return 0;
}

static void save_settings(p8p_menu_t *menu) {
    menu->settings->scale = (uint8_t)p8p_platform_set_scale(
        menu->settings->scale);
    (void)p8p_settings_save(menu->settings);
    p8p_platform_audio_set_volume(menu->settings->muted ? 0 :
                                  menu->settings->volume);
}

static enum p8p_menu_action update_main(p8p_menu_t *menu,
                                        p8p_runtime_t *runtime,
                                        uint16_t pressed) {
    if (pressed & P8P_PHYS_B)
        return P8P_MENU_CLOSE;
    if (pressed & P8P_PHYS_X)
        return P8P_MENU_TOGGLE_DIAGNOSTICS;
    if (!(pressed & P8P_PHYS_A))
        return P8P_MENU_NONE;
    switch (menu->cursor) {
    case 0: return P8P_MENU_CLOSE;
    case 1:
        if (p8p_state_save(P8P_STATE_QUICK, &menu->cart_hash, runtime) == 0) {
            refresh_state(menu, P8P_STATE_QUICK);
            set_notice(menu, tr(menu, "SAVED", "СОХРАНЕНО"));
        } else set_notice(menu, tr(menu, "SAVE FAILED", "ОШИБКА СОХР"));
        break;
    case 2:
        if (menu->states[P8P_STATE_QUICK].exists &&
            p8p_state_load(P8P_STATE_QUICK, &menu->cart_hash, runtime) == 0)
            return P8P_MENU_CLOSE;
        set_notice(menu, tr(menu, "LOAD FAILED", "ОШИБКА ЗАГР"));
        break;
    case 3:
        refresh_states(menu);
        enter_screen(menu, SCREEN_STATES, 0);
        break;
    case 4:
        menu->modal = MODAL_RESTART;
        enter_screen(menu, SCREEN_MODAL, 0);
        break;
    case 5:
        menu->modal = MODAL_CART_PICKER;
        enter_screen(menu, SCREEN_MODAL, 0);
        break;
    case 6: enter_screen(menu, SCREEN_CONTROLS, 0); break;
    case 7: enter_screen(menu, SCREEN_DISPLAY, 0); break;
    case 8: enter_screen(menu, SCREEN_AUDIO, 0); break;
    case 9:
        menu->settings->language ^= 1;
        save_settings(menu);
        break;
    case 10: enter_screen(menu, SCREEN_INFO, 0); break;
    case 11:
        menu->modal = MODAL_EXIT;
        enter_screen(menu, SCREEN_MODAL, 0);
        break;
    }
    return P8P_MENU_NONE;
}

static enum p8p_menu_action update_states(p8p_menu_t *menu,
                                          p8p_runtime_t *runtime,
                                          uint16_t pressed) {
    int slot = menu->cursor;
    if (pressed & P8P_PHYS_B) {
        enter_screen(menu, SCREEN_MAIN, 3);
    } else if (pressed & P8P_PHYS_X) {
        if (p8p_state_save(slot, &menu->cart_hash, runtime) == 0) {
            refresh_state(menu, slot);
            set_notice(menu, tr(menu, "SAVED", "СОХРАНЕНО"));
        } else set_notice(menu, tr(menu, "FAILED", "ОШИБКА"));
    } else if ((pressed & P8P_PHYS_A) && menu->states[slot].exists) {
        if (p8p_state_load(slot, &menu->cart_hash, runtime) == 0)
            return P8P_MENU_CLOSE;
        set_notice(menu, tr(menu, "FAILED", "ОШИБКА"));
    } else if ((pressed & P8P_PHYS_Y) && menu->states[slot].exists) {
        menu->modal_slot = slot;
        menu->modal = MODAL_DELETE;
        enter_screen(menu, SCREEN_MODAL, 0);
    }
    return P8P_MENU_NONE;
}

static void update_controls(p8p_menu_t *menu, uint16_t pressed) {
    p8p_control_profile_t *cart;
    int has_override;
    const p8p_control_profile_t *active = active_controls(menu, &has_override);
    if (menu->capture_action >= 0) {
        uint16_t chosen;
        if (pressed & P8P_PHYS_SELECT) {
            menu->capture_action = -1;
            return;
        }
        chosen = first_remappable_button(pressed & ~P8P_PHYS_SELECT);
        if (chosen) {
            cart = p8p_settings_cart_controls(menu->settings, &menu->cart_hash, 1);
            if (cart) {
                cart->physical[menu->capture_action] = chosen;
                save_settings(menu);
            }
            menu->capture_action = -1;
            set_notice(menu, tr(menu, "MAPPED", "НАЗНАЧЕНО"));
        }
        return;
    }
    if (pressed & P8P_PHYS_B) {
        enter_screen(menu, SCREEN_MAIN, 6);
        return;
    }
    if (!(pressed & P8P_PHYS_A))
        return;
    if (menu->cursor == 0) {
        if (has_override)
            p8p_settings_remove_cart_controls(menu->settings, &menu->cart_hash);
        else
            (void)p8p_settings_cart_controls(menu->settings, &menu->cart_hash, 1);
        save_settings(menu);
    } else if (menu->cursor >= 1 && menu->cursor <= 7) {
        menu->capture_action = menu->cursor - 1;
    } else if (menu->cursor == 8) {
        cart = p8p_settings_cart_controls(menu->settings, &menu->cart_hash, 1);
        if (cart) *cart = menu->settings->global_controls;
        save_settings(menu);
    } else if (menu->cursor == 9) {
        if (has_override) {
            cart = p8p_settings_cart_controls(menu->settings, &menu->cart_hash, 0);
            p8p_control_profile_defaults(cart);
        } else {
            p8p_control_profile_defaults(&menu->settings->global_controls);
        }
        save_settings(menu);
    } else if (menu->cursor == 10) {
        menu->settings->global_controls = *active;
        save_settings(menu);
    } else if (menu->cursor == 11) {
        p8p_settings_remove_cart_controls(menu->settings, &menu->cart_hash);
        save_settings(menu);
    } else if (menu->cursor == 12) {
        enter_screen(menu, SCREEN_MAIN, 6);
    }
}

static void update_display(p8p_menu_t *menu, uint16_t pressed) {
    if ((pressed & P8P_PHYS_B) ||
        ((pressed & P8P_PHYS_A) && menu->cursor == 1)) {
        enter_screen(menu, SCREEN_MAIN, 7);
        return;
    }
    if (menu->cursor == 0 &&
        (pressed & (P8P_PHYS_A | P8P_PHYS_LEFT | P8P_PHYS_RIGHT))) {
        if (pressed & P8P_PHYS_LEFT)
            menu->settings->scale = menu->settings->scale == 0 ? 3 :
                                    menu->settings->scale - 1;
        else
            menu->settings->scale = (menu->settings->scale + 1) % 4;
        save_settings(menu);
    }
}

static void update_audio(p8p_menu_t *menu, uint16_t pressed) {
    if ((pressed & P8P_PHYS_B) ||
        ((pressed & P8P_PHYS_A) && menu->cursor == 2)) {
        enter_screen(menu, SCREEN_MAIN, 8);
        return;
    }
    if (menu->cursor == 0 &&
        (pressed & (P8P_PHYS_LEFT | P8P_PHYS_RIGHT | P8P_PHYS_A))) {
        if (pressed & P8P_PHYS_LEFT)
            menu->settings->volume = menu->settings->volume < 10 ? 0 :
                                     menu->settings->volume - 10;
        else
            menu->settings->volume = menu->settings->volume > 90 ? 100 :
                                     menu->settings->volume + 10;
        save_settings(menu);
    } else if (menu->cursor == 1 && (pressed & P8P_PHYS_A)) {
        menu->settings->muted ^= 1;
        save_settings(menu);
    }
}

static enum p8p_menu_action update_modal(p8p_menu_t *menu,
                                         uint16_t pressed) {
    int modal = menu->modal;
    if ((pressed & P8P_PHYS_B) ||
        (modal == MODAL_CART_PICKER && (pressed & P8P_PHYS_A))) {
        enter_screen(menu, modal == MODAL_DELETE ? SCREEN_STATES : SCREEN_MAIN,
                     modal == MODAL_DELETE ? menu->modal_slot : 5);
        menu->modal = MODAL_NONE;
        return P8P_MENU_NONE;
    }
    if (!(pressed & P8P_PHYS_A))
        return P8P_MENU_NONE;
    menu->modal = MODAL_NONE;
    if (modal == MODAL_RESTART)
        return P8P_MENU_RESTART;
    if (modal == MODAL_EXIT)
        return P8P_MENU_EXIT;
    if (modal == MODAL_DELETE) {
        (void)p8p_state_delete(menu->modal_slot, &menu->cart_hash);
        refresh_state(menu, menu->modal_slot);
        enter_screen(menu, SCREEN_STATES, menu->modal_slot);
        set_notice(menu, tr(menu, "DELETED", "УДАЛЕНО"));
    }
    return P8P_MENU_NONE;
}

enum p8p_menu_action p8p_menu_update(p8p_menu_t *menu,
                                     p8p_runtime_t *runtime,
                                     uint16_t physical_buttons,
                                     uint16_t pressed_buttons) {
    uint16_t pressed;
    enum p8p_menu_action action = P8P_MENU_NONE;
    int count = 0;
    if (!menu || !runtime)
        return P8P_MENU_CLOSE;
    pressed = pressed_buttons |
              (physical_buttons & (uint16_t)~menu->previous_buttons);
    menu->previous_buttons = physical_buttons;

    if ((pressed & P8P_PHYS_SELECT) && menu->capture_action < 0)
        return P8P_MENU_CLOSE;

    switch (menu->screen) {
    case SCREEN_MAIN: count = 12; break;
    case SCREEN_STATES: count = P8P_STATE_SLOT_COUNT; break;
    case SCREEN_CONTROLS: count = 13; break;
    case SCREEN_DISPLAY: count = 2; break;
    case SCREEN_AUDIO: count = 3; break;
    default: count = 0; break;
    }
    if (count && menu->capture_action < 0) {
        if (pressed & P8P_PHYS_UP)
            menu->cursor = menu->cursor == 0 ? count - 1 : menu->cursor - 1;
        if (pressed & P8P_PHYS_DOWN)
            menu->cursor = (menu->cursor + 1) % count;
    }

    switch (menu->screen) {
    case SCREEN_MAIN: action = update_main(menu, runtime, pressed); break;
    case SCREEN_STATES: action = update_states(menu, runtime, pressed); break;
    case SCREEN_CONTROLS: update_controls(menu, pressed); break;
    case SCREEN_DISPLAY: update_display(menu, pressed); break;
    case SCREEN_AUDIO: update_audio(menu, pressed); break;
    case SCREEN_INFO:
        if (pressed & (P8P_PHYS_A | P8P_PHYS_B))
            enter_screen(menu, SCREEN_MAIN, 10);
        break;
    case SCREEN_MODAL: action = update_modal(menu, pressed); break;
    }
    draw_menu(menu, runtime);
    return action;
}

const uint8_t *p8p_menu_framebuffer(const p8p_menu_t *menu) {
    return menu ? menu->framebuffer : NULL;
}
