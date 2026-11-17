#include "term.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ptysession.h"
#include "vt.h"

#define MARGIN 4
#define DEFAULT_FG 0xCDD6F4u
#define DEFAULT_BG 0x1E1E2Eu
#define CURSOR_COLOR 0xF5E0DCu
#define SELECTION_COLOR 0x45475Au
#define FONT PGF_MONO
#define FONT_BOLD PGF_MONO_BOLD

struct term {
    struct pg_widget base;
    struct vt vt;
    struct pty pty;
    bool vt_ready;
    uint32_t watch;
    uint32_t reaper;
    uint32_t repaint;
    int scroll;
    bool selecting;
    int sel_ax, sel_ay, sel_bx, sel_by;
    term_fn on_title;
    term_fn on_exit;
    void *data;
    bool exited;
};

static char *clipboard;

void term_cell_size(int *width, int *height)
{
    const struct pgf_glyph *g = pgc_glyph(FONT, 'M');
    if (width)
        *width = g && g->adv ? g->adv : 9;
    if (height)
        *height = pgc_font_h(FONT) + 2;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void term_measure(struct pg_widget *w, int *width, int *height)
{
    (void)w;
    int cw, ch;
    term_cell_size(&cw, &ch);
    *width = 100 * cw + 2 * MARGIN;
    *height = 30 * ch + 2 * MARGIN;
}

static void term_arrange(struct pg_widget *w)
{
    struct term *t = (struct term *)w;
    if (!t->vt_ready)
        return;
    int cw, ch;
    term_cell_size(&cw, &ch);
    int cols = imax(2, (w->rect.w - 2 * MARGIN) / cw);
    int rows = imax(2, (w->rect.h - 2 * MARGIN) / ch);
    if (cols != t->vt.cols || rows != t->vt.rows) {
        vt_resize(&t->vt, cols, rows);
        pty_resize(&t->pty, cols, rows);
        t->scroll = imin(t->scroll, t->vt.sb_count);
    }
}

static const struct vt_cell *view_line(const struct term *t, int y, int *len)
{
    int line = y - t->scroll;
    if (line < 0) {
        const struct vt_line *l = vt_scrollback_line(&t->vt, t->vt.sb_count + line);
        *len = l ? l->len : 0;
        return l ? l->cells : NULL;
    }
    *len = t->vt.cols;
    return vt_screen_line(&t->vt, line);
}

static bool has_selection(const struct term *t)
{
    return t->sel_ay >= 0 && (t->sel_ax != t->sel_bx || t->sel_ay != t->sel_by);
}

static void ordered(const struct term *t, int *ax, int *ay, int *bx, int *by)
{
    *ax = t->sel_ax;
    *ay = t->sel_ay;
    *bx = t->sel_bx;
    *by = t->sel_by;
    if (*by < *ay || (*by == *ay && *bx < *ax)) {
        int x = *ax, y = *ay;
        *ax = *bx;
        *ay = *by;
        *bx = x;
        *by = y;
    }
}

static bool selected(const struct term *t, int line, int x)
{
    if (!has_selection(t))
        return false;
    int ax, ay, bx, by;
    ordered(t, &ax, &ay, &bx, &by);
    if (line < ay || line > by)
        return false;
    if (line == ay && x < ax)
        return false;
    if (line == by && x > bx)
        return false;
    return true;
}

static void colors(const struct term *t, const struct vt_cell *c, int line, int x, uint32_t *fg, uint32_t *bg)
{
    *fg = (c->attrs & VT_DEFAULT_FG) ? DEFAULT_FG : c->fg;
    *bg = (c->attrs & VT_DEFAULT_BG) ? DEFAULT_BG : c->bg;
    if (c->attrs & VT_INVERSE) {
        uint32_t s = *fg;
        *fg = *bg;
        *bg = s;
    }
    if (c->attrs & VT_FAINT)
        *fg = pgc_mix(*fg, 0, 85);
    if (selected(t, line, x))
        *bg = SELECTION_COLOR;
}

static void term_draw(struct pg_widget *w, struct pg_canvas *c)
{
    struct term *t = (struct term *)w;
    struct pg_rect r = w->rect;
    pgc_fill(c, r.x, r.y, r.w, r.h, DEFAULT_BG);
    if (!t->vt_ready)
        return;
    int cw, chh;
    term_cell_size(&cw, &chh);
    int base = t->vt.sb_count - t->scroll;
    int top_pad = (chh - pgc_font_h(FONT)) / 2;
    for (int y = 0; y < t->vt.rows; y++) {
        int py = r.y + MARGIN + y * chh;
        if (py + chh < c->cy0 || py > c->cy1)
            continue;
        int len = 0;
        const struct vt_cell *line = view_line(t, y, &len);
        if (!line)
            continue;
        for (int x = 0; x < len && x < t->vt.cols; x++) {
            const struct vt_cell *cell = &line[x];
            uint32_t fg, bg;
            colors(t, cell, base + y, x, &fg, &bg);
            int px = r.x + MARGIN + x * cw;
            if (bg != DEFAULT_BG)
                pgc_fill(c, px, py, cw, chh, bg);
            if (cell->ch != ' ' && cell->ch)
                pgc_glyph_draw(c, (cell->attrs & VT_BOLD) ? FONT_BOLD : FONT, px, py + top_pad, cell->ch, fg);
            if (cell->attrs & VT_UNDERLINE)
                pgc_fill(c, px, py + chh - 2, cw, 1, fg);
        }
    }
    if (t->scroll == 0 && t->vt.cursor_visible) {
        int px = r.x + MARGIN + t->vt.cx * cw, py = r.y + MARGIN + t->vt.cy * chh;
        if (pg_widget_has_focus(w)) {
            pgc_fill(c, px, py, cw, chh, CURSOR_COLOR);
            const struct vt_cell *cell = &vt_screen_line(&t->vt, t->vt.cy)[t->vt.cx];
            if (cell->ch != ' ' && cell->ch)
                pgc_glyph_draw(c, (cell->attrs & VT_BOLD) ? FONT_BOLD : FONT, px, py + top_pad, cell->ch, DEFAULT_BG);
        } else {
            pgc_fill(c, px, py, cw, 1, CURSOR_COLOR);
            pgc_fill(c, px, py + chh - 1, cw, 1, CURSOR_COLOR);
            pgc_fill(c, px, py, 1, chh, CURSOR_COLOR);
            pgc_fill(c, px + cw - 1, py, 1, chh, CURSOR_COLOR);
        }
    }
    if (t->scroll > 0) {
        char info[48];
        snprintf(info, sizeof(info), " %d/%d ", t->scroll, t->vt.sb_count);
        int tw = pgc_text_w(PGF_SMALL, info) + 8;
        int ix = r.x + r.w - tw - 8, iy = r.y + 4;
        pgc_rrect(c, ix, iy, tw, 22, 6, SELECTION_COLOR, 255);
        pgc_text(c, PGF_SMALL, ix + 4, iy + 3, info, DEFAULT_FG);
    }
}

static bool repaint_tick(void *data)
{
    struct term *t = data;
    t->repaint = 0;
    pg_widget_queue_draw(&t->base);
    return false;
}

static void schedule(struct term *t)
{
    if (!t->repaint)
        t->repaint = pg_timer_add(8, repaint_tick, t);
}

static void send(struct term *t, const char *data, int size)
{
    if (t->scroll) {
        t->scroll = 0;
        schedule(t);
    }
    pty_write(&t->pty, data, size);
}

static void finish(struct term *t)
{
    if (t->exited)
        return;
    t->exited = true;
    if (t->watch) {
        pg_fd_unwatch(t->watch);
        t->watch = 0;
    }
    if (t->reaper) {
        pg_timer_remove(t->reaper);
        t->reaper = 0;
    }
    if (t->on_exit)
        t->on_exit(&t->base, t->data);
}

static bool reap_tick(void *data)
{
    struct term *t = data;
    if (!pty_reap(&t->pty))
        return true;
    t->reaper = 0;
    finish(t);
    return false;
}

static void readable(int fd, uint32_t events, void *data)
{
    (void)fd;
    (void)events;
    struct term *t = data;
    static char buf[16384];
    for (int i = 0; i < 8; i++) {
        int n = pty_read(&t->pty, buf, sizeof(buf));
        if (n > 0) {
            vt_feed(&t->vt, buf, n);
            continue;
        }
        if (n < 0) {
            pg_fd_unwatch(t->watch);
            t->watch = 0;
            if (pty_reap(&t->pty))
                finish(t);
            return;
        }
        break;
    }
    if (t->scroll && !t->vt.alt)
        t->scroll = imin(t->scroll, t->vt.sb_count);
    schedule(t);
}

static void reply(void *user, const char *data, int size)
{
    struct term *t = user;
    pty_write(&t->pty, data, size);
}

static void title(void *user, const char *text)
{
    (void)text;
    struct term *t = user;
    if (t->on_title)
        t->on_title(&t->base, t->data);
}

static void scroll_view(struct term *t, int lines)
{
    if (t->vt.alt)
        return;
    int v = clampi(t->scroll + lines, 0, t->vt.sb_count);
    if (v != t->scroll) {
        t->scroll = v;
        pg_widget_queue_draw(&t->base);
    }
}

static void cell_at(struct term *t, int px, int py, int *x, int *line)
{
    int cw, ch;
    term_cell_size(&cw, &ch);
    int cx = clampi((px - t->base.rect.x - MARGIN) / cw, 0, t->vt.cols - 1);
    int cy = clampi((py - t->base.rect.y - MARGIN) / ch, 0, t->vt.rows - 1);
    *x = cx;
    *line = t->vt.sb_count - t->scroll + cy;
}

static void copy_selection(struct term *t)
{
    if (!has_selection(t))
        return;
    int ax, ay, bx, by;
    ordered(t, &ax, &ay, &bx, &by);
    size_t cap = (size_t)(by - ay + 1) * (size_t)(t->vt.cols * 4 + 1) + 1;
    char *out = malloc(cap);
    if (!out)
        return;
    size_t n = 0;
    for (int line = ay; line <= by; line++) {
        const struct vt_cell *cells;
        int len;
        if (line < t->vt.sb_count) {
            const struct vt_line *l = vt_scrollback_line(&t->vt, line);
            cells = l ? l->cells : NULL;
            len = l ? l->len : 0;
        } else {
            cells = vt_screen_line(&t->vt, line - t->vt.sb_count);
            len = t->vt.cols;
        }
        int from = line == ay ? ax : 0, to = line == by ? bx : len - 1;
        size_t start = n;
        for (int x = from; cells && x <= to && x < len; x++) {
            uint32_t cp = cells[x].ch ? cells[x].ch : ' ';
            if (cp < 0x80) {
                out[n++] = (char)cp;
            } else if (cp < 0x800) {
                out[n++] = (char)(0xC0 | (cp >> 6));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out[n++] = (char)(0xE0 | (cp >> 12));
                out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[n++] = (char)(0xF0 | (cp >> 18));
                out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        while (n > start && out[n - 1] == ' ')
            n--;
        if (line != by)
            out[n++] = '\n';
    }
    out[n] = 0;
    free(clipboard);
    clipboard = out;
}

static void paste(struct term *t)
{
    if (!clipboard || !*clipboard)
        return;
    size_t len = strlen(clipboard);
    char *data = malloc(len + 16);
    if (!data)
        return;
    size_t n = 0;
    if (t->vt.bracketed_paste) {
        memcpy(data, "\x1b[200~", 6);
        n = 6;
    }
    for (size_t i = 0; i < len; i++)
        data[n++] = clipboard[i] == '\n' ? '\r' : clipboard[i];
    if (t->vt.bracketed_paste) {
        memcpy(data + n, "\x1b[201~", 6);
        n += 6;
    }
    send(t, data, (int)n);
    free(data);
}

static char hid_char(int key, bool shift)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
    if (key < 0x04 || key > 0x38)
        return 0;
    return (shift ? upper : lower)[key - 0x04];
}

static bool key_down(struct term *t, const struct pg_event *e)
{
    bool ctrl = e->mods & PG_MOD_CTRL, shift = e->mods & PG_MOD_SHIFT, alt = e->mods & PG_MOD_ALT;
    int key = e->key;
    if (ctrl && shift && key == PG_KEY_C) {
        copy_selection(t);
        return true;
    }
    if (ctrl && shift && key == PG_KEY_V) {
        paste(t);
        return true;
    }
    if (shift && key == PG_KEY_PAGE_UP) {
        scroll_view(t, t->vt.rows / 2);
        return true;
    }
    if (shift && key == PG_KEY_PAGE_DOWN) {
        scroll_view(t, -t->vt.rows / 2);
        return true;
    }
    bool app = t->vt.app_cursor;
    const char *seq = NULL;
    char buf[8];
    switch (key) {
    case PG_KEY_UP: seq = app ? "\x1bOA" : "\x1b[A"; break;
    case PG_KEY_DOWN: seq = app ? "\x1bOB" : "\x1b[B"; break;
    case PG_KEY_RIGHT: seq = app ? "\x1bOC" : "\x1b[C"; break;
    case PG_KEY_LEFT: seq = app ? "\x1bOD" : "\x1b[D"; break;
    case PG_KEY_HOME: seq = app ? "\x1bOH" : "\x1b[H"; break;
    case PG_KEY_END: seq = app ? "\x1bOF" : "\x1b[F"; break;
    case PG_KEY_PAGE_UP: seq = "\x1b[5~"; break;
    case PG_KEY_PAGE_DOWN: seq = "\x1b[6~"; break;
    case 0x49: seq = "\x1b[2~"; break;
    case PG_KEY_DELETE: seq = "\x1b[3~"; break;
    case 0x3A: seq = "\x1bOP"; break;
    case 0x3B: seq = "\x1bOQ"; break;
    case 0x3C: seq = "\x1bOR"; break;
    case 0x3D: seq = "\x1bOS"; break;
    case 0x3E: seq = "\x1b[15~"; break;
    case 0x3F: seq = "\x1b[17~"; break;
    case 0x40: seq = "\x1b[18~"; break;
    case 0x41: seq = "\x1b[19~"; break;
    case 0x42: seq = "\x1b[20~"; break;
    case 0x43: seq = "\x1b[21~"; break;
    case 0x44: seq = "\x1b[23~"; break;
    case 0x45: seq = "\x1b[24~"; break;
    case PG_KEY_ENTER:
    case PG_KEY_KP_ENTER: seq = "\r"; break;
    case PG_KEY_BACKSPACE: seq = ctrl ? "\x08" : "\x7f"; break;
    case PG_KEY_TAB: seq = shift ? "\x1b[Z" : "\t"; break;
    case PG_KEY_ESCAPE: seq = "\x1b"; break;
    default: break;
    }
    int n = 0;
    if (seq) {
        n = (int)strlen(seq);
    } else if (ctrl) {
        char c = 0;
        bool ok = true;
        if (key >= 0x04 && key <= 0x1D)
            c = (char)(key - 0x04 + 1);
        else if (key == PG_KEY_SPACE || key == 0x1F)
            c = 0;
        else if (key == 0x2F)
            c = 0x1B;
        else if (key == 0x31)
            c = 0x1C;
        else if (key == 0x30)
            c = 0x1D;
        else
            ok = false;
        if (!ok)
            return false;
        buf[0] = c;
        n = 1;
        seq = buf;
    } else {
        char c = hid_char(key, shift);
        if (!c)
            return false;
        buf[0] = c;
        n = 1;
        seq = buf;
    }
    if (alt && seq[0] != '\x1b') {
        char tmp[16];
        tmp[0] = '\x1b';
        memcpy(tmp + 1, seq, (size_t)n);
        send(t, tmp, n + 1);
    } else {
        send(t, seq, n);
    }
    return true;
}

static bool term_event(struct pg_widget *w, const struct pg_event *e)
{
    struct term *t = (struct term *)w;
    switch (e->type) {
    case PG_EVENT_KEY_DOWN:
        return key_down(t, e);
    case PG_EVENT_WHEEL: {
        int steps = e->wheel * 3;
        if (t->vt.alt) {
            for (int i = 0; i < (steps < 0 ? -steps : steps); i++)
                pty_write(&t->pty, steps > 0 ? "\x1b[A" : "\x1b[B", 3);
            return true;
        }
        scroll_view(t, steps);
        return true;
    }
    case PG_EVENT_POINTER_DOWN:
        pg_widget_grab_focus(w);
        if (e->button == PG_BUTTON_LEFT) {
            cell_at(t, e->x, e->y, &t->sel_ax, &t->sel_ay);
            t->sel_bx = t->sel_ax;
            t->sel_by = t->sel_ay;
            t->selecting = true;
            pg_widget_queue_draw(w);
        } else if (e->button == PG_BUTTON_MIDDLE) {
            paste(t);
        }
        return true;
    case PG_EVENT_POINTER_MOVE:
        if (!t->selecting)
            return false;
        cell_at(t, e->x, e->y, &t->sel_bx, &t->sel_by);
        pg_widget_queue_draw(w);
        return true;
    case PG_EVENT_POINTER_UP:
        if (e->button != PG_BUTTON_LEFT || !t->selecting)
            return true;
        t->selecting = false;
        cell_at(t, e->x, e->y, &t->sel_bx, &t->sel_by);
        if (!has_selection(t))
            t->sel_ay = -1;
        else
            copy_selection(t);
        pg_widget_queue_draw(w);
        return true;
    case PG_EVENT_FOCUS_IN:
    case PG_EVENT_FOCUS_OUT:
        pg_widget_queue_draw(w);
        return false;
    default:
        return false;
    }
}

static void term_destroy(struct pg_widget *w)
{
    struct term *t = (struct term *)w;
    if (t->watch)
        pg_fd_unwatch(t->watch);
    if (t->reaper)
        pg_timer_remove(t->reaper);
    if (t->repaint)
        pg_timer_remove(t->repaint);
    pty_close(&t->pty);
    if (t->vt_ready)
        vt_free(&t->vt);
}

static const struct pg_widget_class term_class = {
    .name = "terminal",
    .size = sizeof(struct term),
    .measure = term_measure,
    .arrange = term_arrange,
    .draw = term_draw,
    .event = term_event,
    .destroy = term_destroy,
};

struct pg_widget *term_new(term_fn on_title, term_fn on_exit, void *data)
{
    struct term *t = (struct term *)pg_widget_new(&term_class);
    if (!t)
        return NULL;
    t->pty.master = -1;
    t->pty.pid = -1;
    t->sel_ay = -1;
    t->on_title = on_title;
    t->on_exit = on_exit;
    t->data = data;
    t->base.focusable = true;
    t->base.hexpand = true;
    t->base.vexpand = true;
    if (!vt_init(&t->vt, 100, 30)) {
        pg_widget_destroy(&t->base);
        return NULL;
    }
    t->vt_ready = true;
    t->vt.reply = reply;
    t->vt.on_title = title;
    t->vt.user = t;
    return &t->base;
}

bool term_start(struct pg_widget *w)
{
    struct term *t = (struct term *)w;
    if (!w || w->cls != &term_class || !pty_start(&t->pty, t->vt.cols, t->vt.rows))
        return false;
    t->watch = pg_fd_watch(t->pty.master, PG_FD_READ, readable, t);
    t->reaper = pg_timer_add(250, reap_tick, t);
    return true;
}

const char *term_title(const struct pg_widget *w)
{
    if (!w || w->cls != &term_class)
        return "";
    return ((const struct term *)w)->vt.title;
}
