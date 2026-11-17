#include "vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GROUND, ESCAPE, CSI, OSC, OSC_ESCAPE, CHARSET };

static const uint32_t palette16[16] = {
    0x1E1E2E, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xBAC2DE,
    0x585B70, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xA6ADC8,
};

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t rgb(int r, int g, int b)
{
    return ((uint32_t)(r & 255) << 16) | ((uint32_t)(g & 255) << 8) | (uint32_t)(b & 255);
}

static uint32_t color256(int n)
{
    if (n < 16)
        return palette16[n];
    if (n < 232) {
        n -= 16;
        static const int steps[6] = {0, 95, 135, 175, 215, 255};
        return rgb(steps[n / 36], steps[(n / 6) % 6], steps[n % 6]);
    }
    int v = 8 + (n - 232) * 10;
    return rgb(v, v, v);
}

static struct vt_cell blank(const struct vt *vt)
{
    struct vt_cell c;
    c.ch = ' ';
    c.fg = 0;
    c.bg = vt->pen.bg;
    c.attrs = (uint8_t)(VT_DEFAULT_FG | (vt->pen.attrs & VT_DEFAULT_BG));
    return c;
}

static void fill_blank(const struct vt *vt, struct vt_cell *row, int from, int to)
{
    struct vt_cell b = blank(vt);
    for (int x = from; x < to; x++)
        row[x] = b;
}

static struct vt_cell *new_row(const struct vt *vt, int cols)
{
    struct vt_cell *row = malloc(sizeof(struct vt_cell) * (size_t)cols);
    if (row)
        fill_blank(vt, row, 0, cols);
    return row;
}

static struct vt_cell **new_rows(const struct vt *vt, int cols, int rows)
{
    struct vt_cell **r = calloc((size_t)rows, sizeof(*r));
    if (!r)
        return NULL;
    for (int y = 0; y < rows; y++)
        if (!(r[y] = new_row(vt, cols)))
            return NULL;
    return r;
}

static void free_rows(struct vt_cell **r, int rows)
{
    for (int y = 0; r && y < rows; y++)
        free(r[y]);
    free(r);
}

bool vt_init(struct vt *vt, int cols, int rows)
{
    memset(vt, 0, sizeof(*vt));
    vt->cols = imax(cols, 2);
    vt->rows = imax(rows, 2);
    vt->pen.ch = ' ';
    vt->pen.attrs = VT_DEFAULT_FG | VT_DEFAULT_BG;
    vt->saved_pen = vt->pen;
    vt->main_rows = new_rows(vt, vt->cols, vt->rows);
    vt->alt_rows = new_rows(vt, vt->cols, vt->rows);
    vt->scrollback = calloc(VT_SCROLLBACK, sizeof(struct vt_line));
    if (!vt->main_rows || !vt->alt_rows || !vt->scrollback)
        return false;
    vt->screen = vt->main_rows;
    vt->bottom = vt->rows - 1;
    vt->cursor_visible = true;
    vt->autowrap = true;
    return true;
}

void vt_free(struct vt *vt)
{
    free_rows(vt->main_rows, vt->rows);
    free_rows(vt->alt_rows, vt->rows);
    for (int i = 0; vt->scrollback && i < vt->sb_count; i++)
        free(vt->scrollback[(vt->sb_head + i) % VT_SCROLLBACK].cells);
    free(vt->scrollback);
    memset(vt, 0, sizeof(*vt));
}

static void push_scrollback(struct vt *vt, struct vt_cell *row, int len)
{
    struct vt_cell *copy = malloc(sizeof(struct vt_cell) * (size_t)len);
    if (!copy)
        return;
    memcpy(copy, row, sizeof(struct vt_cell) * (size_t)len);
    if (vt->sb_count == VT_SCROLLBACK) {
        free(vt->scrollback[vt->sb_head].cells);
        vt->scrollback[vt->sb_head].cells = copy;
        vt->scrollback[vt->sb_head].len = len;
        vt->sb_head = (vt->sb_head + 1) % VT_SCROLLBACK;
        return;
    }
    struct vt_line *l = &vt->scrollback[(vt->sb_head + vt->sb_count) % VT_SCROLLBACK];
    l->cells = copy;
    l->len = len;
    vt->sb_count++;
}

static void clear_scrollback(struct vt *vt)
{
    for (int i = 0; i < vt->sb_count; i++)
        free(vt->scrollback[(vt->sb_head + i) % VT_SCROLLBACK].cells);
    memset(vt->scrollback, 0, sizeof(struct vt_line) * VT_SCROLLBACK);
    vt->sb_head = vt->sb_count = 0;
}

const struct vt_cell *vt_screen_line(const struct vt *vt, int y)
{
    return vt->screen[clampi(y, 0, vt->rows - 1)];
}

const struct vt_line *vt_scrollback_line(const struct vt *vt, int index)
{
    if (index < 0 || index >= vt->sb_count)
        return NULL;
    return &vt->scrollback[(vt->sb_head + index) % VT_SCROLLBACK];
}

static void clamp_cursor(struct vt *vt)
{
    vt->cx = clampi(vt->cx, 0, vt->cols - 1);
    vt->cy = clampi(vt->cy, 0, vt->rows - 1);
    vt->wrap_pending = false;
}

static struct vt_cell **fit(struct vt *vt, struct vt_cell **old, bool is_main, bool keep_bottom, int cols, int rows)
{
    int count = vt->rows, first = 0;
    while (count > rows) {
        if (keep_bottom && vt->cy > 0) {
            if (is_main)
                push_scrollback(vt, old[first], vt->cols);
            free(old[first]);
            first++;
            vt->cy--;
        } else {
            free(old[first + count - 1]);
        }
        count--;
    }
    struct vt_cell **r = calloc((size_t)rows, sizeof(*r));
    if (!r)
        return old;
    for (int y = 0; y < rows; y++) {
        struct vt_cell *row = new_row(vt, cols);
        if (y < count && row) {
            memcpy(row, old[first + y], sizeof(struct vt_cell) * (size_t)imin(cols, vt->cols));
            free(old[first + y]);
        }
        r[y] = row;
    }
    free(old);
    return r;
}

void vt_resize(struct vt *vt, int cols, int rows)
{
    cols = imax(cols, 2);
    rows = imax(rows, 2);
    if (cols == vt->cols && rows == vt->rows)
        return;
    bool on_alt = vt->screen == vt->alt_rows;
    int cy = vt->cy;
    vt->main_rows = fit(vt, vt->main_rows, true, !on_alt, cols, rows);
    int main_cy = vt->cy;
    vt->cy = cy;
    vt->alt_rows = fit(vt, vt->alt_rows, false, on_alt, cols, rows);
    if (!on_alt)
        vt->cy = main_cy;
    vt->screen = on_alt ? vt->alt_rows : vt->main_rows;
    vt->cols = cols;
    vt->rows = rows;
    vt->top = 0;
    vt->bottom = rows - 1;
    clamp_cursor(vt);
}

static void scroll_up(struct vt *vt, int top, int bottom, int n)
{
    n = imin(n, bottom - top + 1);
    for (int i = 0; i < n; i++) {
        struct vt_cell *row = vt->screen[top];
        if (vt->screen == vt->main_rows && top == 0)
            push_scrollback(vt, row, vt->cols);
        memmove(&vt->screen[top], &vt->screen[top + 1], sizeof(*vt->screen) * (size_t)(bottom - top));
        fill_blank(vt, row, 0, vt->cols);
        vt->screen[bottom] = row;
    }
}

static void scroll_down(struct vt *vt, int top, int bottom, int n)
{
    n = imin(n, bottom - top + 1);
    for (int i = 0; i < n; i++) {
        struct vt_cell *row = vt->screen[bottom];
        memmove(&vt->screen[top + 1], &vt->screen[top], sizeof(*vt->screen) * (size_t)(bottom - top));
        fill_blank(vt, row, 0, vt->cols);
        vt->screen[top] = row;
    }
}

static void erase(struct vt *vt, int y, int x0, int x1)
{
    fill_blank(vt, vt->screen[y], imax(0, x0), imin(vt->cols, x1));
}

static void do_index(struct vt *vt)
{
    if (vt->cy == vt->bottom)
        scroll_up(vt, vt->top, vt->bottom, 1);
    else if (vt->cy < vt->rows - 1)
        vt->cy++;
    vt->wrap_pending = false;
}

static void reverse_index(struct vt *vt)
{
    if (vt->cy == vt->top)
        scroll_down(vt, vt->top, vt->bottom, 1);
    else if (vt->cy > 0)
        vt->cy--;
}

static void put_char(struct vt *vt, uint32_t ch)
{
    if (vt->wrap_pending && vt->autowrap) {
        vt->cx = 0;
        do_index(vt);
    }
    vt->wrap_pending = false;
    struct vt_cell *c = &vt->screen[vt->cy][vt->cx];
    *c = vt->pen;
    c->ch = ch;
    if (vt->cx == vt->cols - 1)
        vt->wrap_pending = true;
    else
        vt->cx++;
}

static void control(struct vt *vt, char c)
{
    switch (c) {
    case '\r':
        vt->cx = 0;
        vt->wrap_pending = false;
        break;
    case '\n':
    case '\v':
    case '\f':
        do_index(vt);
        break;
    case '\b':
        if (vt->cx > 0)
            vt->cx--;
        vt->wrap_pending = false;
        break;
    case '\t':
        vt->cx = imin(vt->cols - 1, (vt->cx / 8 + 1) * 8);
        break;
    default:
        break;
    }
}

static void switch_alt(struct vt *vt, bool on)
{
    if (on == vt->alt)
        return;
    vt->alt = on;
    if (on) {
        vt->saved_x = vt->cx;
        vt->saved_y = vt->cy;
        vt->saved_pen = vt->pen;
        vt->screen = vt->alt_rows;
        for (int y = 0; y < vt->rows; y++)
            erase(vt, y, 0, vt->cols);
    } else {
        vt->screen = vt->main_rows;
        vt->cx = vt->saved_x;
        vt->cy = vt->saved_y;
        vt->pen = vt->saved_pen;
    }
    vt->top = 0;
    vt->bottom = vt->rows - 1;
    clamp_cursor(vt);
}

static void reset_pen(struct vt *vt)
{
    memset(&vt->pen, 0, sizeof(vt->pen));
    vt->pen.ch = ' ';
    vt->pen.attrs = VT_DEFAULT_FG | VT_DEFAULT_BG;
}

static void escape(struct vt *vt, char c)
{
    vt->state = GROUND;
    switch (c) {
    case '[':
        vt->state = CSI;
        vt->plen = 0;
        vt->private_mode = false;
        break;
    case ']':
        vt->state = OSC;
        vt->olen = 0;
        break;
    case '(':
    case ')':
    case '*':
    case '+':
        vt->state = CHARSET;
        break;
    case '7':
        vt->saved_x = vt->cx;
        vt->saved_y = vt->cy;
        vt->saved_pen = vt->pen;
        break;
    case '8':
        vt->cx = vt->saved_x;
        vt->cy = vt->saved_y;
        vt->pen = vt->saved_pen;
        clamp_cursor(vt);
        break;
    case 'D':
        do_index(vt);
        break;
    case 'E':
        vt->cx = 0;
        do_index(vt);
        break;
    case 'M':
        reverse_index(vt);
        break;
    case 'c':
        reset_pen(vt);
        switch_alt(vt, false);
        for (int y = 0; y < vt->rows; y++)
            erase(vt, y, 0, vt->cols);
        vt->cx = vt->cy = 0;
        vt->top = 0;
        vt->bottom = vt->rows - 1;
        vt->cursor_visible = true;
        vt->app_cursor = false;
        vt->autowrap = true;
        break;
    default:
        break;
    }
}

static int param(const struct vt *vt, int index, int fallback)
{
    if (index >= vt->nargs || vt->args[index] <= 0)
        return fallback;
    return vt->args[index];
}

static void set_mode(struct vt *vt, bool on)
{
    if (!vt->private_mode)
        return;
    for (int i = 0; i < vt->nargs; i++) {
        switch (vt->args[i]) {
        case 1: vt->app_cursor = on; break;
        case 7: vt->autowrap = on; break;
        case 25: vt->cursor_visible = on; break;
        case 47:
        case 1047:
        case 1049: switch_alt(vt, on); break;
        case 2004: vt->bracketed_paste = on; break;
        default: break;
        }
    }
}

static void sgr(struct vt *vt)
{
    if (!vt->nargs)
        vt->args[vt->nargs++] = 0;
    for (int i = 0; i < vt->nargs; i++) {
        int a = vt->args[i];
        if (a == 0) {
            reset_pen(vt);
        } else if (a == 1) {
            vt->pen.attrs |= VT_BOLD;
        } else if (a == 2) {
            vt->pen.attrs |= VT_FAINT;
        } else if (a == 4) {
            vt->pen.attrs |= VT_UNDERLINE;
        } else if (a == 7) {
            vt->pen.attrs |= VT_INVERSE;
        } else if (a == 22) {
            vt->pen.attrs &= (uint8_t)~(VT_BOLD | VT_FAINT);
        } else if (a == 24) {
            vt->pen.attrs &= (uint8_t)~VT_UNDERLINE;
        } else if (a == 27) {
            vt->pen.attrs &= (uint8_t)~VT_INVERSE;
        } else if (a >= 30 && a <= 37) {
            vt->pen.fg = palette16[a - 30];
            vt->pen.attrs &= (uint8_t)~VT_DEFAULT_FG;
        } else if (a >= 90 && a <= 97) {
            vt->pen.fg = palette16[a - 90 + 8];
            vt->pen.attrs &= (uint8_t)~VT_DEFAULT_FG;
        } else if (a == 39) {
            vt->pen.attrs |= VT_DEFAULT_FG;
        } else if (a >= 40 && a <= 47) {
            vt->pen.bg = palette16[a - 40];
            vt->pen.attrs &= (uint8_t)~VT_DEFAULT_BG;
        } else if (a >= 100 && a <= 107) {
            vt->pen.bg = palette16[a - 100 + 8];
            vt->pen.attrs &= (uint8_t)~VT_DEFAULT_BG;
        } else if (a == 49) {
            vt->pen.attrs |= VT_DEFAULT_BG;
        } else if (a == 38 || a == 48) {
            uint32_t color;
            if (i + 2 < vt->nargs && vt->args[i + 1] == 5) {
                color = color256(clampi(vt->args[i + 2], 0, 255));
                i += 2;
            } else if (i + 4 < vt->nargs && vt->args[i + 1] == 2) {
                color = rgb(vt->args[i + 2], vt->args[i + 3], vt->args[i + 4]);
                i += 4;
            } else {
                break;
            }
            if (a == 38) {
                vt->pen.fg = color;
                vt->pen.attrs &= (uint8_t)~VT_DEFAULT_FG;
            } else {
                vt->pen.bg = color;
                vt->pen.attrs &= (uint8_t)~VT_DEFAULT_BG;
            }
        }
    }
}

static void reply(struct vt *vt, const char *s)
{
    if (vt->reply)
        vt->reply(vt->user, s, (int)strlen(s));
}

static void csi(struct vt *vt, char final)
{
    int n = param(vt, 0, 1);
    struct vt_cell *line = vt->screen[vt->cy];
    switch (final) {
    case '@':
        n = imin(n, vt->cols - vt->cx);
        memmove(&line[vt->cx + n], &line[vt->cx], sizeof(*line) * (size_t)(vt->cols - vt->cx - n));
        fill_blank(vt, line, vt->cx, vt->cx + n);
        break;
    case 'A': vt->cy = imax(vt->cy - n, vt->cy >= vt->top ? vt->top : 0); vt->wrap_pending = false; break;
    case 'B': vt->cy = imin(vt->cy + n, vt->cy <= vt->bottom ? vt->bottom : vt->rows - 1); vt->wrap_pending = false; break;
    case 'C': vt->cx += n; clamp_cursor(vt); break;
    case 'D': vt->cx -= n; clamp_cursor(vt); break;
    case 'E': vt->cx = 0; vt->cy += n; clamp_cursor(vt); break;
    case 'F': vt->cx = 0; vt->cy -= n; clamp_cursor(vt); break;
    case 'G':
    case '`': vt->cx = n - 1; clamp_cursor(vt); break;
    case 'd': vt->cy = n - 1; clamp_cursor(vt); break;
    case 'H':
    case 'f':
        vt->cy = param(vt, 0, 1) - 1;
        vt->cx = param(vt, 1, 1) - 1;
        clamp_cursor(vt);
        break;
    case 'J': {
        int mode = vt->nargs ? vt->args[0] : 0;
        if (mode == 0) {
            erase(vt, vt->cy, vt->cx, vt->cols);
            for (int y = vt->cy + 1; y < vt->rows; y++)
                erase(vt, y, 0, vt->cols);
        } else if (mode == 1) {
            for (int y = 0; y < vt->cy; y++)
                erase(vt, y, 0, vt->cols);
            erase(vt, vt->cy, 0, vt->cx + 1);
        } else {
            for (int y = 0; y < vt->rows; y++)
                erase(vt, y, 0, vt->cols);
            if (mode == 3)
                clear_scrollback(vt);
        }
        break;
    }
    case 'K': {
        int mode = vt->nargs ? vt->args[0] : 0;
        if (mode == 0)
            erase(vt, vt->cy, vt->cx, vt->cols);
        else if (mode == 1)
            erase(vt, vt->cy, 0, vt->cx + 1);
        else
            erase(vt, vt->cy, 0, vt->cols);
        break;
    }
    case 'L':
        if (vt->cy >= vt->top && vt->cy <= vt->bottom)
            scroll_down(vt, vt->cy, vt->bottom, n);
        break;
    case 'M':
        if (vt->cy >= vt->top && vt->cy <= vt->bottom) {
            struct vt_cell **saved = vt->screen;
            if (saved == vt->main_rows) {
                int top = vt->cy;
                n = imin(n, vt->bottom - top + 1);
                for (int i = 0; i < n; i++) {
                    struct vt_cell *row = vt->screen[top];
                    memmove(&vt->screen[top], &vt->screen[top + 1], sizeof(*vt->screen) * (size_t)(vt->bottom - top));
                    fill_blank(vt, row, 0, vt->cols);
                    vt->screen[vt->bottom] = row;
                }
            } else {
                scroll_up(vt, vt->cy, vt->bottom, n);
            }
        }
        break;
    case 'P':
        n = imin(n, vt->cols - vt->cx);
        memmove(&line[vt->cx], &line[vt->cx + n], sizeof(*line) * (size_t)(vt->cols - vt->cx - n));
        fill_blank(vt, line, vt->cols - n, vt->cols);
        break;
    case 'S': scroll_up(vt, vt->top, vt->bottom, n); break;
    case 'T': scroll_down(vt, vt->top, vt->bottom, n); break;
    case 'X': erase(vt, vt->cy, vt->cx, vt->cx + n); break;
    case 'm': sgr(vt); break;
    case 'r': {
        int top = param(vt, 0, 1) - 1, bottom = param(vt, 1, vt->rows) - 1;
        if (top < bottom && bottom < vt->rows) {
            vt->top = top;
            vt->bottom = bottom;
            vt->cx = 0;
            vt->cy = 0;
        }
        break;
    }
    case 's':
        vt->saved_x = vt->cx;
        vt->saved_y = vt->cy;
        break;
    case 'u':
        vt->cx = vt->saved_x;
        vt->cy = vt->saved_y;
        clamp_cursor(vt);
        break;
    case 'h': set_mode(vt, true); break;
    case 'l': set_mode(vt, false); break;
    case 'n':
        if (param(vt, 0, 0) == 6) {
            char buf[32];
            snprintf(buf, sizeof(buf), "\x1b[%d;%dR", vt->cy + 1, vt->cx + 1);
            reply(vt, buf);
        } else if (param(vt, 0, 0) == 5) {
            reply(vt, "\x1b[0n");
        }
        break;
    case 'c':
        if (!vt->private_mode)
            reply(vt, "\x1b[?1;2c");
        break;
    default:
        break;
    }
}

static void osc(struct vt *vt)
{
    vt->osc[vt->olen] = 0;
    char *semi = strchr(vt->osc, ';');
    if (!semi)
        return;
    *semi = 0;
    int code = atoi(vt->osc);
    if (code == 0 || code == 2) {
        snprintf(vt->title, sizeof(vt->title), "%s", semi + 1);
        if (vt->on_title)
            vt->on_title(vt->user, vt->title);
    }
}

static void parse_args(struct vt *vt)
{
    vt->nargs = 0;
    int value = 0;
    bool any = false;
    for (int i = 0; i <= vt->plen; i++) {
        char ch = i < vt->plen ? vt->params[i] : ';';
        if (ch == ';' || ch == ':') {
            if (vt->nargs < (int)(sizeof(vt->args) / sizeof(vt->args[0])))
                vt->args[vt->nargs++] = any ? value : 0;
            value = 0;
            any = false;
        } else if (ch >= '0' && ch <= '9') {
            value = value * 10 + (ch - '0');
            if (value > 100000)
                value = 100000;
            any = true;
        }
    }
    if (vt->plen == 0)
        vt->nargs = 0;
}

void vt_feed(struct vt *vt, const char *data, int size)
{
    for (int i = 0; i < size; i++) {
        unsigned char b = (unsigned char)data[i];
        switch (vt->state) {
        case GROUND:
            if (vt->utf8_left) {
                if ((b & 0xC0) == 0x80) {
                    vt->utf8 = (vt->utf8 << 6) | (b & 0x3F);
                    if (--vt->utf8_left == 0)
                        put_char(vt, vt->utf8);
                    continue;
                }
                vt->utf8_left = 0;
                put_char(vt, 0xFFFD);
            }
            if (b == 0x1B) {
                vt->state = ESCAPE;
            } else if (b < 0x20 || b == 0x7F) {
                control(vt, (char)b);
            } else if (b < 0x80) {
                put_char(vt, b);
            } else if ((b & 0xE0) == 0xC0) {
                vt->utf8 = b & 0x1F;
                vt->utf8_left = 1;
            } else if ((b & 0xF0) == 0xE0) {
                vt->utf8 = b & 0x0F;
                vt->utf8_left = 2;
            } else if ((b & 0xF8) == 0xF0) {
                vt->utf8 = b & 0x07;
                vt->utf8_left = 3;
            } else {
                put_char(vt, 0xFFFD);
            }
            break;
        case ESCAPE:
            escape(vt, (char)b);
            break;
        case CSI:
            if (b >= 0x40 && b <= 0x7E) {
                vt->state = GROUND;
                parse_args(vt);
                csi(vt, (char)b);
            } else if (b == '?' || b == '>' || b == '=') {
                vt->private_mode = true;
            } else if (b < 0x20) {
                control(vt, (char)b);
            } else if (vt->plen < (int)sizeof(vt->params)) {
                vt->params[vt->plen++] = (char)b;
            }
            break;
        case OSC:
            if (b == 0x07) {
                osc(vt);
                vt->state = GROUND;
            } else if (b == 0x1B) {
                vt->state = OSC_ESCAPE;
            } else if (vt->olen < (int)sizeof(vt->osc) - 1) {
                vt->osc[vt->olen++] = (char)b;
            }
            break;
        case OSC_ESCAPE:
            osc(vt);
            vt->state = GROUND;
            if (b != '\\')
                escape(vt, (char)b);
            break;
        case CHARSET:
        default:
            vt->state = GROUND;
            break;
        }
    }
}
