#include "surface.h"
#include <ghostty/vt.h>
#include <stdlib.h>
#include <string.h>

struct gat_surface {
    GhosttyTerminal terminal;
    GhosttyRenderState render;
    GhosttyRenderStateRowIterator rows;
    GhosttyRenderStateRowCells cells;
    GhosttyKeyEncoder encoder;
    GhosttyKeyEvent key;
    gat_write_fn write;
    void *context;
    bool write_ok;
};

static void reply(GhosttyTerminal terminal, void *context, const uint8_t *bytes, size_t length) {
    (void)terminal;
    gat_surface *s = context;
    if (!s->write(s->context, bytes, length)) s->write_ok = false;
}

gat_surface *gat_surface_new(gat_write_fn write, void *context, const char **error) {
    *error = "无法分配终端引擎";
    gat_surface *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->write = write; s->context = context; s->write_ok = true;
#define INIT(call, message) do { if ((call) != GHOSTTY_SUCCESS) { *error = message; gat_surface_free(s); return NULL; } } while (0)
    INIT(ghostty_terminal_new(NULL, &s->terminal, 80, 24), "无法创建 VT 网格");
    INIT(ghostty_render_state_new(NULL, &s->render), "无法创建 VT 渲染状态");
    INIT(ghostty_render_state_row_iterator_new(NULL, &s->rows), "无法创建 VT 行迭代器");
    INIT(ghostty_render_state_row_cells_new(NULL, &s->cells), "无法创建 VT 单元格迭代器");
    INIT(ghostty_key_encoder_new(NULL, &s->encoder), "无法创建 VT 按键编码器");
    INIT(ghostty_key_event_new(NULL, &s->key), "无法创建 VT 按键事件");
    GhosttyColorRgb background = {23, 27, 32}, foreground = {228, 232, 238};
    size_t scrollback = 8 * 1024 * 1024;
    INIT(ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_USERDATA, s), "无法设置 VT 上下文");
    INIT(ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *)reply), "无法设置 VT 输入回调");
    INIT(ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &background), "无法设置终端背景色");
    INIT(ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &foreground), "无法设置终端前景色");
    INIT(ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &scrollback), "无法设置终端滚动历史");
#undef INIT
    return s;
}

void gat_surface_free(gat_surface *s) {
    if (!s) return;
    ghostty_key_event_free(s->key); ghostty_key_encoder_free(s->encoder);
    ghostty_render_state_row_cells_free(s->cells); ghostty_render_state_row_iterator_free(s->rows);
    ghostty_render_state_free(s->render); ghostty_terminal_free(s->terminal); free(s);
}

void gat_surface_feed(gat_surface *s, const uint8_t *bytes, size_t length) {
    ghostty_terminal_vt_write(s->terminal, bytes, length);
}

bool gat_surface_resize(gat_surface *s, uint16_t cols, uint16_t rows, uint16_t cw, uint16_t ch) {
    return ghostty_terminal_resize(s->terminal, cols, rows, cw, ch) == GHOSTTY_SUCCESS;
}

static void bottom(gat_surface *s) {
    ghostty_terminal_scroll_viewport(s->terminal, (GhosttyTerminalScrollViewport){.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM});
}

bool gat_surface_text(gat_surface *s, const uint8_t *text, size_t length) {
    bottom(s);
    /* IME committed text is typed input, not bracketed paste. */
    return s->write(s->context, text, length);
}

/* Android physical key codes; text is supplied separately by the platform. */
static GhosttyKey key_code(int key) {
    /* Private TerminalKeys.KEYPAD_NAVIGATION extension, frozen by the View at down. */
    if (key >= (0x10000 | 144) && key <= (0x10000 | 153)) {
        static const GhosttyKey nav[] = { GHOSTTY_KEY_NUMPAD_INSERT, GHOSTTY_KEY_NUMPAD_END,
            GHOSTTY_KEY_NUMPAD_DOWN, GHOSTTY_KEY_NUMPAD_PAGE_DOWN, GHOSTTY_KEY_NUMPAD_LEFT,
            GHOSTTY_KEY_NUMPAD_BEGIN, GHOSTTY_KEY_NUMPAD_RIGHT, GHOSTTY_KEY_NUMPAD_HOME,
            GHOSTTY_KEY_NUMPAD_UP, GHOSTTY_KEY_NUMPAD_PAGE_UP };
        return nav[key - (0x10000 | 144)];
    }
    if (key == (0x10000 | 158)) return GHOSTTY_KEY_NUMPAD_DELETE;
    if (key >= 144 && key <= 153) return GHOSTTY_KEY_NUMPAD_0 + key - 144;
    if (key >= 29 && key <= 54) return GHOSTTY_KEY_A + key - 29;
    if (key >= 7 && key <= 16) return GHOSTTY_KEY_DIGIT_0 + key - 7;
    if (key >= 131 && key <= 142) return GHOSTTY_KEY_F1 + key - 131;
    switch (key) {
        case 57: return GHOSTTY_KEY_ALT_LEFT; case 58: return GHOSTTY_KEY_ALT_RIGHT;
        case 59: return GHOSTTY_KEY_SHIFT_LEFT; case 60: return GHOSTTY_KEY_SHIFT_RIGHT;
        case 113: return GHOSTTY_KEY_CONTROL_LEFT; case 114: return GHOSTTY_KEY_CONTROL_RIGHT;
        case 115: return GHOSTTY_KEY_CAPS_LOCK; case 116: return GHOSTTY_KEY_SCROLL_LOCK;
        case 117: return GHOSTTY_KEY_META_LEFT; case 118: return GHOSTTY_KEY_META_RIGHT;
        case 120: return GHOSTTY_KEY_PRINT_SCREEN; case 121: return GHOSTTY_KEY_PAUSE;
        case 143: return GHOSTTY_KEY_NUM_LOCK;
        case 154: return GHOSTTY_KEY_NUMPAD_DIVIDE; case 155: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
        case 156: return GHOSTTY_KEY_NUMPAD_SUBTRACT; case 157: return GHOSTTY_KEY_NUMPAD_ADD;
        case 158: return GHOSTTY_KEY_NUMPAD_DECIMAL; case 159: return GHOSTTY_KEY_NUMPAD_COMMA;
        case 160: return GHOSTTY_KEY_NUMPAD_ENTER; case 161: return GHOSTTY_KEY_NUMPAD_EQUAL;
        case 162: return GHOSTTY_KEY_NUMPAD_PAREN_LEFT; case 163: return GHOSTTY_KEY_NUMPAD_PAREN_RIGHT;
        case 19: return GHOSTTY_KEY_ARROW_UP; case 20: return GHOSTTY_KEY_ARROW_DOWN;
        case 21: return GHOSTTY_KEY_ARROW_LEFT; case 22: return GHOSTTY_KEY_ARROW_RIGHT;
        case 61: return GHOSTTY_KEY_TAB; case 62: return GHOSTTY_KEY_SPACE;
        case 66: return GHOSTTY_KEY_ENTER; case 67: return GHOSTTY_KEY_BACKSPACE;
        case 92: return GHOSTTY_KEY_PAGE_UP; case 93: return GHOSTTY_KEY_PAGE_DOWN;
        case 111: return GHOSTTY_KEY_ESCAPE; case 112: return GHOSTTY_KEY_DELETE;
        case 122: return GHOSTTY_KEY_HOME; case 123: return GHOSTTY_KEY_END;
        case 124: return GHOSTTY_KEY_INSERT;
        case 68: return GHOSTTY_KEY_BACKQUOTE; case 69: return GHOSTTY_KEY_MINUS;
        case 70: return GHOSTTY_KEY_EQUAL; case 71: return GHOSTTY_KEY_BRACKET_LEFT;
        case 72: return GHOSTTY_KEY_BRACKET_RIGHT; case 73: return GHOSTTY_KEY_BACKSLASH;
        case 74: return GHOSTTY_KEY_SEMICOLON; case 75: return GHOSTTY_KEY_QUOTE;
        case 76: return GHOSTTY_KEY_SLASH; case 55: return GHOSTTY_KEY_COMMA;
        case 56: return GHOSTTY_KEY_PERIOD; default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

bool gat_surface_key(gat_surface *s, int key, int mods, uint32_t unshifted, int action, const uint8_t *text, size_t length) {
    if (action != GHOSTTY_KEY_ACTION_RELEASE) bottom(s);
    ghostty_key_encoder_setopt_from_terminal(s->encoder, s->terminal);
    ghostty_key_event_set_action(s->key, (GhosttyKeyAction)action);
    ghostty_key_event_set_key(s->key, key_code(key));
    ghostty_key_event_set_mods(s->key, (GhosttyMods)mods);
    ghostty_key_event_set_consumed_mods(s->key, 0);
    ghostty_key_event_set_utf8(s->key, (const char *)text, length);
    ghostty_key_event_set_unshifted_codepoint(s->key, unshifted ? unshifted :
        (key >= 29 && key <= 54 ? (uint32_t)('a' + key - 29) : 0));
    char buffer[256]; size_t length_out = 0;
    GhosttyResult result = ghostty_key_encoder_encode(s->encoder, s->key, buffer, sizeof(buffer), &length_out);
    if (result != GHOSTTY_SUCCESS) return false;
    return s->write(s->context, (const uint8_t *)buffer, length_out);
}

void gat_surface_scroll(gat_surface *s, int delta) {
    ghostty_terminal_scroll_viewport(s->terminal, (GhosttyTerminalScrollViewport){
        .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA, .value.delta = delta});
}

typedef struct { const uint8_t *bytes; size_t length; } paste_data;
static bool read_paste(void *context, GhosttyString mime, GhosttyWriter writer) {
    (void)mime;
    paste_data *data = context;
    return !data->length || writer.write(writer.userdata, data->bytes, data->length);
}

int gat_surface_action(gat_surface *s, int kind, int x, int y,
        const uint8_t *bytes, size_t length, uint8_t **result, size_t *result_length) {
    *result = NULL; *result_length = 0;
    if (kind == 1) {
        paste_data data = {bytes, length};
        const GhosttyString mime = {.ptr = (const uint8_t *)"text/plain", .len = 10};
        GhosttyPaste paste = {.size = sizeof(paste), .location = GHOSTTY_CLIPBOARD_LOCATION_STANDARD,
            .source = GHOSTTY_PASTE_SOURCE_CLIPBOARD, .mimes = &mime, .mimes_len = 1,
            .reader = {.read = read_paste, .userdata = &data}, .allow_unsafe = x != 0};
        GhosttyResult status = ghostty_terminal_paste(s->terminal, &paste, NULL);
        if (status == GHOSTTY_REJECTED) return 1;
        if (status != GHOSTTY_SUCCESS || !s->write_ok) return 2;
        bottom(s); return 0;
    }
    if (kind == 8) return ghostty_terminal_clear_local(s->terminal) == GHOSTTY_SUCCESS ? 0 : 2;
    if (kind == 9) {
        GhosttySelection current = GHOSTTY_INIT_SIZED(GhosttySelection);
        GhosttySelection ordered = GHOSTTY_INIT_SIZED(GhosttySelection);
        if (ghostty_terminal_get(s->terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &current) != GHOSTTY_SUCCESS ||
            ghostty_terminal_selection_ordered(s->terminal, &current, GHOSTTY_SELECTION_ORDER_FORWARD, &ordered) != GHOSTTY_SUCCESS) return 2;
        return ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &ordered) == GHOSTTY_SUCCESS ? 0 : 2;
    }
    if (kind == 6)
        return ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, NULL) == GHOSTTY_SUCCESS ? 0 : 2;
    if (kind == 7) {
        GhosttyTerminalSelectionFormatOptions opts = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
        opts.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN; opts.unwrap = true; opts.trim = true;
        size_t required = 0;
        GhosttyResult status = ghostty_terminal_selection_format_buf(s->terminal, opts, NULL, 0, &required);
        if (status == GHOSTTY_NO_VALUE || (status == GHOSTTY_SUCCESS && !required)) return 0;
        if (status != GHOSTTY_OUT_OF_SPACE || required > 1024 * 1024) return 2;
        uint8_t *copy = malloc(required);
        if (!copy) return 2;
        status = ghostty_terminal_selection_format_buf(s->terminal, opts, copy, required, result_length);
        if (status != GHOSTTY_SUCCESS) { free(copy); *result_length = 0; return 2; }
        *result = copy; return 0;
    }
    if (kind == 10 || kind == 11) {
        if (length != 1 || (int8_t)bytes[0] < -1 || (int8_t)bytes[0] > 1) return 2;
        /* Scrolling and moving the endpoint are one owner operation. Cancellation
         * can skip the whole command, never scroll without the matching selection. */
        gat_surface_scroll(s, (int8_t)bytes[0]);
        kind = kind == 10 ? 3 : 4;
    }
    GhosttySelection selection = GHOSTTY_INIT_SIZED(GhosttySelection);
    GhosttyResult status;
    if (kind == 5) {
        status = ghostty_terminal_select_all(s->terminal, &selection);
    } else {
        if (x < 0 || y < 0 || x > 255 || y > 255) return 2;
        GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
        GhosttyPoint point = {.tag = GHOSTTY_POINT_TAG_VIEWPORT, .value.coordinate = {.x = x, .y = y}};
        if (ghostty_terminal_grid_ref(s->terminal, point, &ref) != GHOSTTY_SUCCESS) return 2;
        if (kind == 2) {
            GhosttyTerminalSelectWordOptions word = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
            word.ref = ref;
            status = ghostty_terminal_select_word(s->terminal, &word, &selection);
        } else if (kind == 3 || kind == 4) {
            GhosttySelection current = GHOSTTY_INIT_SIZED(GhosttySelection);
            if (ghostty_terminal_get(s->terminal, GHOSTTY_TERMINAL_DATA_SELECTION, &current) != GHOSTTY_SUCCESS) return 2;
            /* Preserve the other endpoint throughout this gesture, even after crossing it.
             * The UI orders endpoints once, when the next handle gesture starts. */
            selection = current;
            status = GHOSTTY_SUCCESS;
            if (kind == 3) selection.start = ref; else selection.end = ref;
        } else return 2;
    }
    if (status == GHOSTTY_NO_VALUE) return ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, NULL) == GHOSTTY_SUCCESS ? 0 : 2;
    if (status != GHOSTTY_SUCCESS) return 2;
    return ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &selection) == GHOSTTY_SUCCESS ? 0 : 2;
}

static int32_t color(GhosttyColorRgb c) { return (int32_t)(0xff000000u | (uint32_t)c.r << 16 | (uint32_t)c.g << 8 | c.b); }

/* Private packed frame v1: 10 header integers then (bg, fg, flags, width, n, codepoints...) per cell.
 * flags: bold=1 italic=2 faint=4 invisible=8 strike=16 underline=32 overline=64 blink=128.
 * Every glyph, including combining marks and wide-cell spacers, comes from the real VT. */
bool gat_surface_frame(gat_surface *s, int32_t **values, size_t *count) {
    *values = NULL; *count = 0;
    if (!s->write_ok || ghostty_render_state_update(s->render, s->terminal) != GHOSTTY_SUCCESS) return false;
    uint16_t cols = 0, rows = 0;
    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    GhosttyRenderStateCursor cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    if (ghostty_render_state_get(s->render, GHOSTTY_RENDER_STATE_DATA_COLS, &cols) != GHOSTTY_SUCCESS ||
        ghostty_render_state_get(s->render, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_get(s->render, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors) != GHOSTTY_SUCCESS ||
        ghostty_render_state_get(s->render, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor) != GHOSTTY_SUCCESS ||
        ghostty_render_state_get(s->render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &s->rows) != GHOSTTY_SUCCESS) return false;
    size_t cap = 10 + (size_t)cols * rows * 6, used = 10;
    int32_t *out = malloc(cap * sizeof(*out));
    if (!out) return false;
    out[0] = 1; out[1] = cols; out[2] = rows; out[3] = color(colors.background); out[4] = color(colors.foreground);
    out[5] = cursor.viewport_has_value ? cursor.viewport_x : -1;
    out[6] = cursor.viewport_has_value ? cursor.viewport_y : -1;
    out[7] = cursor.visible && cursor.viewport_has_value; out[8] = cursor.blinking; out[9] = cursor.visual_style;
    while (ghostty_render_state_row_iterator_next(s->rows)) {
        if (ghostty_render_state_row_get(s->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &s->cells) != GHOSTTY_SUCCESS) goto failed;
        while (ghostty_render_state_row_cells_next(s->cells)) {
            GhosttyCell raw; GhosttyCellWide wide; uint32_t n = 0; bool selected = false;
            GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            GhosttyColorRgb bg = colors.background, fg = colors.foreground;
            if (ghostty_render_state_row_cells_get(s->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw) != GHOSTTY_SUCCESS ||
                ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide) != GHOSTTY_SUCCESS ||
                ghostty_render_state_row_cells_get(s->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style) != GHOSTTY_SUCCESS ||
                ghostty_render_state_row_cells_get(s->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &n) != GHOSTTY_SUCCESS) goto failed;
            (void)ghostty_render_state_row_cells_get(s->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg);
            (void)ghostty_render_state_row_cells_get(s->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
            if (n > 65536 || used + 5 + n > 4 * 1024 * 1024) goto failed;
            if (used + 5 + n > cap) {
                cap = (used + 5 + n) * 2;
                int32_t *grown = realloc(out, cap * sizeof(*out)); if (!grown) goto failed; out = grown;
            }
            (void)ghostty_render_state_row_cells_get(s->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_SELECTED, &selected);
            out[used++] = color(style.inverse ? fg : bg); out[used++] = color(style.inverse ? bg : fg);
            out[used++] = style.bold | style.italic << 1 | style.faint << 2 | style.invisible << 3 |
                style.strikethrough << 4 | (style.underline != 0) << 5 | style.overline << 6 | style.blink << 7 | selected << 8;
            out[used++] = wide == GHOSTTY_CELL_WIDE_NARROW ? 1 : wide == GHOSTTY_CELL_WIDE_WIDE ? 2 : 0;
            out[used++] = (int32_t)n;
            if (n && ghostty_render_state_row_cells_get(s->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, out + used) != GHOSTTY_SUCCESS) goto failed;
            used += n;
        }
    }
    *values = out; *count = used; return true;
failed:
    free(out); return false;
}
