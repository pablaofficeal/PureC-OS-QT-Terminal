#pragma once
#include "pg/pg.h"

typedef void (*term_fn)(struct pg_widget *term, void *data);

struct pg_widget *term_new(term_fn on_title, term_fn on_exit, void *data);
bool term_start(struct pg_widget *term);
const char *term_title(const struct pg_widget *term);
void term_cell_size(int *width, int *height);
