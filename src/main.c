#include <stdio.h>
#include <string.h>

#include "pg/pg.h"
#include "term.h"

#define MAX_TABS 16
#define BAR_H 30
#define TAB_MAX_W 220
#define BAR_BG 0x181825u
#define TAB_BG 0x1E1E2Eu
#define TAB_FG 0xA6ADC8u
#define TAB_FG_ACTIVE 0xCDD6F4u
#define ACCENT 0x89B4FAu

static struct {
    struct pg_window *win;
    struct pg_widget *bar;
    struct pg_widget *stack;
    struct pg_widget *tabs[MAX_TABS];
    int count;
    int current;
    int hot;
    int hot_close;
} app;

static void new_tab(void);
static void close_tab(int index);

static const char *tab_label(int i)
{
    const char *t = term_title(app.tabs[i]);
    return t && *t ? t : "Shell";
}

static int tab_width(int i)
{
    int w = pgc_text_w(PGF_BODY, tab_label(i)) + 48;
    return w > TAB_MAX_W ? TAB_MAX_W : w < 100 ? 100 : w;
}

static struct pg_rect tab_rect(struct pg_widget *bar, int index)
{
    int x = bar->rect.x;
    for (int i = 0; i < index; i++)
        x += tab_width(i);
    struct pg_rect r = {x, bar->rect.y, tab_width(index), BAR_H};
    return r;
}

static struct pg_rect close_rect(struct pg_rect tab)
{
    struct pg_rect r = {tab.x + tab.w - 26, tab.y + (BAR_H - 20) / 2, 20, 20};
    return r;
}

static bool inside(struct pg_rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void bar_measure(struct pg_widget *w, int *width, int *height)
{
    (void)w;
    *width = 100;
    *height = BAR_H;
}

static void bar_draw(struct pg_widget *w, struct pg_canvas *c)
{
    pgc_fill(c, w->rect.x, w->rect.y, w->rect.w, w->rect.h, BAR_BG);
    for (int i = 0; i < app.count; i++) {
        struct pg_rect r = tab_rect(w, i);
        bool sel = i == app.current;
        if (sel) {
            pgc_fill(c, r.x, r.y, r.w, r.h, TAB_BG);
            pgc_fill(c, r.x, r.y, r.w, 2, ACCENT);
        } else if (i == app.hot) {
            pgc_fill(c, r.x, r.y, r.w, r.h, pgc_mix(BAR_BG, TAB_BG, 128));
        }
        pgc_text_fit(c, PGF_BODY, r.x + 14, r.y + (BAR_H - pgc_font_h(PGF_BODY)) / 2, r.w - 46, tab_label(i),
                     sel ? TAB_FG_ACTIVE : TAB_FG);
        struct pg_rect x = close_rect(r);
        if (i == app.hot_close)
            pgc_rrect(c, x.x, x.y, x.w, x.h, 5, 0x313244, 255);
        float cx = (float)x.x + 10.0f, cy = (float)x.y + 10.0f;
        uint32_t col = i == app.hot_close ? 0xF38BA8 : TAB_FG;
        pgc_line(c, cx - 4, cy - 4, cx + 4, cy + 4, 1.3f, col);
        pgc_line(c, cx + 4, cy - 4, cx - 4, cy + 4, 1.3f, col);
    }
}

static void select_tab(int index);

static bool bar_event(struct pg_widget *w, const struct pg_event *e)
{
    int hot = -1, hot_close = -1;
    for (int i = 0; i < app.count; i++) {
        struct pg_rect r = tab_rect(w, i);
        if (inside(r, e->x, e->y)) {
            hot = i;
            if (inside(close_rect(r), e->x, e->y))
                hot_close = i;
        }
    }
    switch (e->type) {
    case PG_EVENT_POINTER_MOVE:
        if (hot != app.hot || hot_close != app.hot_close) {
            app.hot = hot;
            app.hot_close = hot_close;
            pg_widget_queue_draw(w);
        }
        return true;
    case PG_EVENT_LEAVE:
        app.hot = app.hot_close = -1;
        pg_widget_queue_draw(w);
        return false;
    case PG_EVENT_POINTER_DOWN:
        if (e->button == PG_BUTTON_MIDDLE && hot >= 0) {
            close_tab(hot);
            return true;
        }
        if (e->button != PG_BUTTON_LEFT)
            return false;
        if (hot_close >= 0)
            close_tab(hot_close);
        else if (hot >= 0)
            select_tab(hot);
        return true;
    case PG_EVENT_POINTER_UP:
        return true;
    default:
        return false;
    }
}

static const struct pg_widget_class bar_class = {
    .name = "tab-bar",
    .measure = bar_measure,
    .draw = bar_draw,
    .event = bar_event,
};

static void sync_title(void)
{
    const char *t = app.count ? term_title(app.tabs[app.current]) : "";
    pg_window_set_title(app.win, t && *t ? t : "Terminal");
    pg_widget_set_visible(app.bar, app.count > 1);
    pg_widget_queue_draw(app.bar);
}

static void select_tab(int index)
{
    if (index < 0 || index >= app.count)
        return;
    app.current = index;
    pg_stack_set_index(app.stack, index);
    pg_widget_grab_focus(app.tabs[index]);
    sync_title();
}

static int index_of(struct pg_widget *term)
{
    for (int i = 0; i < app.count; i++)
        if (app.tabs[i] == term)
            return i;
    return -1;
}

static void on_title(struct pg_widget *term, void *data)
{
    (void)term;
    (void)data;
    sync_title();
}

static void on_exit(struct pg_widget *term, void *data)
{
    (void)data;
    int i = index_of(term);
    if (i >= 0)
        close_tab(i);
}

static void close_tab(int index)
{
    if (index < 0 || index >= app.count)
        return;
    struct pg_widget *term = app.tabs[index];
    memmove(&app.tabs[index], &app.tabs[index + 1], sizeof(app.tabs[0]) * (size_t)(app.count - index - 1));
    app.count--;
    app.hot = app.hot_close = -1;
    pg_widget_destroy(term);
    if (!app.count) {
        pg_window_destroy(app.win);
        return;
    }
    if (app.current >= app.count || app.current > index)
        app.current = app.current > 0 ? app.current - 1 : 0;
    select_tab(app.current);
}

static void new_tab(void)
{
    if (app.count >= MAX_TABS)
        return;
    struct pg_widget *term = term_new(on_title, on_exit, NULL);
    if (!term)
        return;
    pg_widget_add(app.stack, term);
    if (!term_start(term)) {
        pg_widget_destroy(term);
        return;
    }
    app.tabs[app.count++] = term;
    select_tab(app.count - 1);
}

static bool on_key(struct pg_window *win, const struct pg_event *e, void *data)
{
    (void)win;
    (void)data;
    bool ctrl = e->mods & PG_MOD_CTRL, shift = e->mods & PG_MOD_SHIFT;
    if (ctrl && shift && e->key == 0x17) {
        new_tab();
        return true;
    }
    if (ctrl && shift && e->key == PG_KEY_W) {
        close_tab(app.current);
        return true;
    }
    if (ctrl && !shift && e->key == PG_KEY_PAGE_DOWN && app.count > 1) {
        select_tab((app.current + 1) % app.count);
        return true;
    }
    if (ctrl && !shift && e->key == PG_KEY_PAGE_UP && app.count > 1) {
        select_tab((app.current + app.count - 1) % app.count);
        return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (!pg_init(argc, argv)) {
        fprintf(stderr, "terminal: cannot connect to videod\n");
        return 1;
    }
    app.hot = app.hot_close = -1;
    int cw, ch;
    term_cell_size(&cw, &ch);
    int w = 100 * cw + 8, h = 30 * ch + 8;
    if (w < 860)
        w = 860;
    if (h < 540)
        h = 540;
    app.win = pg_window_new("Terminal", w, h, PG_WINDOW_RESIZABLE);
    if (!app.win) {
        fprintf(stderr, "terminal: cannot create a window\n");
        return 1;
    }
    pg_window_set_min_size(app.win, 20 * cw + 8, 6 * ch + 8);
    pg_window_set_background(app.win, TAB_BG);
    pg_window_on_key(app.win, on_key, NULL);
    struct pg_widget *root = pg_vbox(0);
    app.bar = pg_widget_new(&bar_class);
    pg_widget_add(root, app.bar);
    app.stack = pg_stack_new();
    pg_widget_set_expand(app.stack, true, true);
    pg_widget_add(root, app.stack);
    pg_window_set_child(app.win, root);
    new_tab();
    if (!app.count) {
        fprintf(stderr, "terminal: cannot start a shell\n");
        return 1;
    }
    int code = pg_run();
    pg_shutdown();
    return code;
}
