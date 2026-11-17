#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VT_BOLD 1u
#define VT_FAINT 2u
#define VT_UNDERLINE 4u
#define VT_INVERSE 8u
#define VT_DEFAULT_FG 16u
#define VT_DEFAULT_BG 32u

#define VT_SCROLLBACK 5000

struct vt_cell {
    uint32_t ch;
    uint32_t fg;
    uint32_t bg;
    uint8_t attrs;
};

struct vt_line {
    int len;
    struct vt_cell *cells;
};

typedef void (*vt_reply_fn)(void *user, const char *data, int size);
typedef void (*vt_title_fn)(void *user, const char *title);

struct vt {
    int cols, rows;
    struct vt_cell **main_rows, **alt_rows, **screen;
    struct vt_line *scrollback;
    int sb_head, sb_count;
    int cx, cy;
    int saved_x, saved_y;
    struct vt_cell pen, saved_pen;
    bool wrap_pending;
    int top, bottom;
    bool cursor_visible;
    bool app_cursor;
    bool autowrap;
    bool bracketed_paste;
    bool alt;
    int state;
    char params[64];
    int plen;
    bool private_mode;
    int args[32];
    int nargs;
    char osc[512];
    int olen;
    char title[128];
    uint32_t utf8;
    int utf8_left;
    vt_reply_fn reply;
    vt_title_fn on_title;
    void *user;
};

bool vt_init(struct vt *vt, int cols, int rows);
void vt_free(struct vt *vt);
void vt_feed(struct vt *vt, const char *data, int size);
void vt_resize(struct vt *vt, int cols, int rows);
const struct vt_cell *vt_screen_line(const struct vt *vt, int y);
const struct vt_line *vt_scrollback_line(const struct vt *vt, int index);
