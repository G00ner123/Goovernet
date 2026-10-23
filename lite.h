#ifndef LITE_H
#define LITE_H

#include <gtk/gtk.h>

typedef struct _LiteView LiteView;

typedef struct {
    void (*changed)(LiteView *lv, gpointer data);
    void (*open_link)(LiteView *lv, const char *url, gboolean background, gpointer data);
    gpointer data;
} LiteHooks;

LiteView *lite_view_new(const LiteHooks *hooks);
LiteView *lite_from_widget(GtkWidget *w);
GtkWidget *lite_view_widget(LiteView *lv);
void lite_view_load(LiteView *lv, const char *url);
void lite_view_reload(LiteView *lv);
void lite_view_back(LiteView *lv);
void lite_view_forward(LiteView *lv);
gboolean lite_view_can_back(LiteView *lv);
gboolean lite_view_can_forward(LiteView *lv);
const char *lite_view_url(LiteView *lv);
const char *lite_view_title(LiteView *lv);
gboolean lite_view_loading(LiteView *lv);
double lite_view_progress(LiteView *lv);
void lite_view_zoom(LiteView *lv, double factor, gboolean reset);
void lite_view_find(LiteView *lv, const char *text, gboolean forward);
void lite_view_find_clear(LiteView *lv);
void lite_view_jump_change(LiteView *lv, gboolean forward);
void lite_view_focus(LiteView *lv);
void lite_set_options(gboolean delta, gboolean auto_images);
void lite_delta_clear_all(void);

#endif
