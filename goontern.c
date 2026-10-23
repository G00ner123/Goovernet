#include <adwaita.h>
#include <webkit/webkit.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <string.h>
#include "lite.h"

#define START_URI "goontern://start"
#define BG_URI "goontern://bg"
#define CLOSED_MAX 20

#define THEME_CSS \
"@define-color accent_bg_color #88c0d0;" \
"@define-color accent_fg_color #2e3440;" \
"@define-color accent_color #88c0d0;" \
"@define-color window_bg_color #2e3440;" \
"@define-color view_bg_color #2e3440;" \
"@define-color headerbar_bg_color #2b303b;" \
"@define-color headerbar_fg_color #eceff4;" \
"entry.url{border-radius:999px;padding:4px 18px;min-height:32px;background:#3b4252;border:1px solid transparent;}" \
"entry.url:focus-within{border-color:#88c0d0;}" \
"entry.url progress{background:#88c0d0;min-height:2px;}" \
".lite-bar{background:#3b4252;color:#d8dee9;padding:5px 16px;font-size:0.9em;}" \
".lite-status{background:rgba(43,48,59,.94);color:#eceff4;padding:3px 10px;border-radius:0 8px 0 0;font-size:0.85em;}"

typedef struct {
    const char *name;
    const char *tmpl;
} Engine;

static const Engine ENGINES[] = {
    { "DuckDuckGo", "https://duckduckgo.com/?q=%s" },
    { "Mojeek", "https://www.mojeek.com/search?q=%s" },
    { "Brave Search", "https://search.brave.com/search?q=%s" },
};

typedef struct {
    char *name;
    char *tmpl;
} CustomEngine;

static GPtrArray *customs;

#define NBUILTIN ((int)G_N_ELEMENTS(ENGINES))

static void custom_free(gpointer p)
{
    CustomEngine *c = p;
    g_free(c->name);
    g_free(c->tmpl);
    g_free(c);
}

static void custom_add(const char *name, const char *tmpl)
{
    if (!customs)
        customs = g_ptr_array_new_with_free_func(custom_free);
    CustomEngine *c = g_new0(CustomEngine, 1);
    c->name = g_strdup(name);
    c->tmpl = g_strdup(tmpl);
    g_ptr_array_add(customs, c);
}

static int engine_count(void)
{
    return NBUILTIN + (customs ? (int)customs->len : 0);
}

static const char *engine_name(int i)
{
    if (i < NBUILTIN)
        return ENGINES[i].name;
    return ((CustomEngine *)g_ptr_array_index(customs, (guint)(i - NBUILTIN)))->name;
}

static const char *engine_tmpl(int i)
{
    if (i < NBUILTIN)
        return ENGINES[i].tmpl;
    return ((CustomEngine *)g_ptr_array_index(customs, (guint)(i - NBUILTIN)))->tmpl;
}

static int engine_find(const char *name)
{
    for (int i = 0; i < engine_count(); i++)
        if (strcmp(engine_name(i), name) == 0)
            return i;
    return 0;
}

static gboolean valid_tmpl(const char *t)
{
    return (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://")) && strstr(t, "%s");
}

static char *js_escape(const char *s)
{
    GString *g = g_string_new(NULL);
    for (; *s; s++) {
        guchar c = (guchar)*s;
        if (c == '\\' || c == '\'' || c == '<' || c == '>' || c == '&' || c == '"' || c < 32)
            g_string_append_printf(g, "\\x%02x", c);
        else
            g_string_append_c(g, (char)c);
    }
    return g_string_free(g, FALSE);
}

typedef struct {
    int engine;
    gboolean smooth;
    gboolean gpu;
    gboolean gesture;
    gboolean image;
    gboolean delta;
    gboolean lite_images;
    double dim;
    double blur;
    char bg[48];
    char accent[48];
    unsigned ver;
} Cfg;

typedef struct {
    GApplication *app;
    AdwApplicationWindow *win;
    AdwTabView *tabs;
    GtkEntry *entry;
    GtkSearchBar *sbar;
    GtkSearchEntry *sentry;
} Ui;

static Ui ui;
static Cfg cfg;
static char *closed[CLOSED_MAX];
static int n_closed;
static gboolean skip_record;
static int insert_pos = -1;

static WebKitWebView *open_tab(const char *uri, WebKitWebView *related);
static LiteView *open_lite_tab(const char *url, gboolean background);
static void replace_current(const char *uri, gboolean lite);

static char *conf_file(const char *name)
{
    return g_build_filename(g_get_user_config_dir(), "goontern", name, NULL);
}

static void conf_mkdir(void)
{
    char *d = g_build_filename(g_get_user_config_dir(), "goontern", NULL);
    g_mkdir_with_parents(d, 0700);
    g_free(d);
}

static gboolean kf_bool(GKeyFile *kf, const char *k, gboolean def)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, "main", k, &e);
    if (e) {
        g_error_free(e);
        return def;
    }
    return v;
}

static double kf_num(GKeyFile *kf, const char *k, double def)
{
    GError *e = NULL;
    double v = g_key_file_get_double(kf, "main", k, &e);
    if (e) {
        g_error_free(e);
        return def;
    }
    return v;
}

static void kf_str(GKeyFile *kf, const char *k, char *dst, const char *def)
{
    char *v = g_key_file_get_string(kf, "main", k, NULL);
    g_strlcpy(dst, v ? v : def, 48);
    g_free(v);
}

static void cfg_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *p = conf_file("settings.ini");
    g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL);
    g_free(p);
    int nc = (int)kf_num(kf, "ncustom", 0);
    for (int i = 0; i < nc && i < 20; i++) {
        char k1[24], k2[24];
        g_snprintf(k1, sizeof k1, "cname%d", i);
        g_snprintf(k2, sizeof k2, "curl%d", i);
        char *nm = g_key_file_get_string(kf, "main", k1, NULL);
        char *ur = g_key_file_get_string(kf, "main", k2, NULL);
        if (nm && ur && valid_tmpl(ur))
            custom_add(nm, ur);
        g_free(nm);
        g_free(ur);
    }
    char *en = g_key_file_get_string(kf, "main", "engine", NULL);
    cfg.engine = en ? engine_find(en) : 0;
    g_free(en);
    cfg.smooth = kf_bool(kf, "smooth", FALSE);
    cfg.gpu = kf_bool(kf, "gpu", TRUE);
    cfg.gesture = kf_bool(kf, "gesture", TRUE);
    cfg.image = kf_bool(kf, "image", FALSE);
    cfg.delta = kf_bool(kf, "delta", TRUE);
    cfg.lite_images = kf_bool(kf, "lite_images", FALSE);
    cfg.dim = kf_num(kf, "dim", 35);
    cfg.blur = kf_num(kf, "blur", 0);
    kf_str(kf, "bg", cfg.bg, "rgb(46,52,64)");
    kf_str(kf, "accent", cfg.accent, "rgb(136,192,208)");
    g_key_file_free(kf);
    char *img = conf_file("background.jpg");
    if (!g_file_test(img, G_FILE_TEST_EXISTS))
        cfg.image = FALSE;
    g_free(img);
}

static void cfg_save(void)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "main", "engine", engine_name(cfg.engine));
    guint nc = customs ? customs->len : 0;
    g_key_file_set_integer(kf, "main", "ncustom", (gint)nc);
    for (guint i = 0; i < nc; i++) {
        CustomEngine *c = g_ptr_array_index(customs, i);
        char k1[24], k2[24];
        g_snprintf(k1, sizeof k1, "cname%u", i);
        g_snprintf(k2, sizeof k2, "curl%u", i);
        g_key_file_set_string(kf, "main", k1, c->name);
        g_key_file_set_string(kf, "main", k2, c->tmpl);
    }
    g_key_file_set_boolean(kf, "main", "smooth", cfg.smooth);
    g_key_file_set_boolean(kf, "main", "gpu", cfg.gpu);
    g_key_file_set_boolean(kf, "main", "gesture", cfg.gesture);
    g_key_file_set_boolean(kf, "main", "image", cfg.image);
    g_key_file_set_boolean(kf, "main", "delta", cfg.delta);
    g_key_file_set_boolean(kf, "main", "lite_images", cfg.lite_images);
    g_key_file_set_double(kf, "main", "dim", cfg.dim);
    g_key_file_set_double(kf, "main", "blur", cfg.blur);
    g_key_file_set_string(kf, "main", "bg", cfg.bg);
    g_key_file_set_string(kf, "main", "accent", cfg.accent);
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, NULL);
    conf_mkdir();
    char *p = conf_file("settings.ini");
    g_file_set_contents(p, data, (gssize)len, NULL);
    g_free(p);
    g_free(data);
    g_key_file_free(kf);
}

static char *start_html(void)
{
    char dim[G_ASCII_DTOSTR_BUF_SIZE];
    char blur[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(dim, sizeof dim, "%.2f", cfg.dim / 100.0);
    g_ascii_formatd(blur, sizeof blur, "%.0f", cfg.blur);
    GString *s = g_string_new(NULL);
    g_string_append(s, "<!doctype html><html lang='de'><head><meta charset='utf-8'>"
                       "<meta name='color-scheme' content='dark'><title>Goontern</title><style>");
    g_string_append_printf(s,
        "html,body{height:100%%;margin:0;overflow:hidden}"
        "body{background:%s;display:flex;flex-direction:column;align-items:center;justify-content:center;"
        "gap:28px;font-family:system-ui,sans-serif;color:#eceff4}"
        "body::before{content:'';position:fixed;inset:0;"
        "background:radial-gradient(circle at 50%% 30%%,rgba(255,255,255,.08),transparent 70%%)}",
        cfg.bg);
    if (cfg.image) {
        g_string_append_printf(s,
            "body::before{background:url(" BG_URI "?v=%u) center/cover no-repeat;filter:blur(%spx);transform:scale(1.08)}"
            "body::after{content:'';position:fixed;inset:0;background:rgba(0,0,0,%s)}",
            cfg.ver, blur, dim);
    }
    g_string_append_printf(s,
        "h1,form{position:relative;z-index:1}"
        "h1{font-size:64px;font-weight:300;letter-spacing:6px;margin:0;text-shadow:0 2px 24px rgba(0,0,0,.45)}"
        "h1 span{color:%s}"
        "form{width:min(560px,86vw)}"
        ".hint{position:relative;z-index:1;margin:0;font-size:14px;color:rgba(236,239,244,.5)}"
        "input{width:100%%;box-sizing:border-box;padding:16px 24px;border-radius:999px;"
        "border:1px solid rgba(255,255,255,.18);background:rgba(46,52,64,.88);color:#eceff4;font-size:18px;outline:none}"
        "input:focus{border-color:%s}",
        cfg.accent, cfg.accent);
    char *jt = js_escape(engine_tmpl(cfg.engine));
    g_string_append_printf(s,
        "</style></head><body><h1>Goon<span>tern</span></h1>"
        "<form id='f'><input id='q' autofocus autocomplete='off' placeholder='Suche oder Adresse'></form>"
        "<script>var T='%s';document.getElementById('f').onsubmit=function(e){e.preventDefault();"
        "var v=document.getElementById('q').value.trim();if(!v)return;"
        "if(v.indexOf(' ')<0&&v.indexOf('.')>0){location.href=v.indexOf('://')>0?v:'https://'+v;return;}"
        "location.href=T.replace('%%s',encodeURIComponent(v));};</script>",
        jt);
    g_free(jt);
    g_string_append(s, "<p class='hint'>Tipp: <b>lite</b> vor einer Adresse oder Strg+Umschalt+L startet die schlanke Lite-Engine</p></body></html>");
    return g_string_free(s, FALSE);
}

static void on_scheme(WebKitURISchemeRequest *req, gpointer d)
{
    const char *uri = webkit_uri_scheme_request_get_uri(req);
    if (g_str_has_prefix(uri, BG_URI)) {
        char *p = conf_file("background.jpg");
        char *data = NULL;
        gsize len = 0;
        GError *err = NULL;
        if (g_file_get_contents(p, &data, &len, &err)) {
            GInputStream *st = g_memory_input_stream_new_from_data(data, (gssize)len, g_free);
            webkit_uri_scheme_request_finish(req, st, (gint64)len, "image/jpeg");
            g_object_unref(st);
        } else {
            webkit_uri_scheme_request_finish_error(req, err);
            g_error_free(err);
        }
        g_free(p);
        return;
    }
    char *html = start_html();
    gsize len = strlen(html);
    GInputStream *st = g_memory_input_stream_new_from_data(html, (gssize)len, g_free);
    webkit_uri_scheme_request_finish(req, st, (gint64)len, "text/html");
    g_object_unref(st);
}

static gboolean is_internal(const char *uri)
{
    return uri && g_str_has_prefix(uri, "goontern://");
}

static WebKitWebView *view_of(AdwTabPage *page)
{
    if (!page)
        return NULL;
    GtkWidget *c = adw_tab_page_get_child(page);
    return WEBKIT_IS_WEB_VIEW(c) ? WEBKIT_WEB_VIEW(c) : NULL;
}

static LiteView *lite_of(AdwTabPage *page)
{
    return page ? lite_from_widget(adw_tab_page_get_child(page)) : NULL;
}

static WebKitWebView *view_at(int i)
{
    return view_of(adw_tab_view_get_nth_page(ui.tabs, i));
}

static WebKitWebView *current_view(void)
{
    return view_of(adw_tab_view_get_selected_page(ui.tabs));
}

static LiteView *current_lite(void)
{
    return lite_of(adw_tab_view_get_selected_page(ui.tabs));
}

static void apply_view_settings(WebKitWebView *v)
{
    WebKitSettings *s = webkit_web_view_get_settings(v);
    webkit_settings_set_enable_smooth_scrolling(s, cfg.smooth);
    webkit_settings_set_hardware_acceleration_policy(s,
        cfg.gpu ? WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS : WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    webkit_settings_set_media_playback_requires_user_gesture(s, cfg.gesture);
    webkit_settings_set_enable_page_cache(s, TRUE);
    webkit_settings_set_enable_developer_extras(s, FALSE);
    webkit_settings_set_enable_back_forward_navigation_gestures(s, TRUE);
}

static void apply_all(void)
{
    int n = adw_tab_view_get_n_pages(ui.tabs);
    for (int i = 0; i < n; i++) {
        WebKitWebView *v = view_at(i);
        if (v)
            apply_view_settings(v);
    }
}

static void reload_start_tabs(void)
{
    int n = adw_tab_view_get_n_pages(ui.tabs);
    for (int i = 0; i < n; i++) {
        WebKitWebView *v = view_at(i);
        if (v && g_strcmp0(webkit_web_view_get_uri(v), START_URI) == 0)
            webkit_web_view_reload_bypass_cache(v);
    }
}

static void set_action_enabled(const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(ui.app), name);
    if (a)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

static void sync_ui(void)
{
    LiteView *lv = current_lite();
    if (lv) {
        const char *lu = lite_view_url(lv);
        const char *lt = lu ? lu : "";
        if (g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(ui.entry)), lt) != 0)
            gtk_editable_set_text(GTK_EDITABLE(ui.entry), lt);
        set_action_enabled("back", lite_view_can_back(lv));
        set_action_enabled("forward", lite_view_can_forward(lv));
        gtk_entry_set_progress_fraction(ui.entry, lite_view_loading(lv) ? lite_view_progress(lv) : 0.0);
        return;
    }
    WebKitWebView *v = current_view();
    if (!v)
        return;
    const char *uri = webkit_web_view_get_uri(v);
    const char *text = (uri && !is_internal(uri)) ? uri : "";
    if (g_strcmp0(gtk_editable_get_text(GTK_EDITABLE(ui.entry)), text) != 0)
        gtk_editable_set_text(GTK_EDITABLE(ui.entry), text);
    set_action_enabled("back", webkit_web_view_can_go_back(v));
    set_action_enabled("forward", webkit_web_view_can_go_forward(v));
    double p = webkit_web_view_get_estimated_load_progress(v);
    gtk_entry_set_progress_fraction(ui.entry, webkit_web_view_is_loading(v) ? p : 0.0);
}

static char *normalize(const char *in, gboolean lite)
{
    char *t = g_strstrip(g_strdup(in));
    char *out = NULL;
    if (*t) {
        if (strstr(t, "://")) {
            out = g_strdup(t);
        } else if (g_str_has_prefix(t, "localhost")) {
            out = g_strconcat("http://", t, NULL);
        } else if (!strchr(t, ' ') && strchr(t, '.')) {
            out = g_strconcat("https://", t, NULL);
        } else {
            char *q = g_uri_escape_string(t, NULL, FALSE);
            const char *tm = (lite && cfg.engine == 0) ? "https://html.duckduckgo.com/html/?q=%s" : engine_tmpl(cfg.engine);
            const char *pos = strstr(tm, "%s");
            char *pre = g_strndup(tm, (gsize)(pos - tm));
            out = g_strconcat(pre, q, pos + 2, NULL);
            g_free(pre);
            g_free(q);
        }
    }
    g_free(t);
    return out;
}

static void on_view_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    if ((gpointer)o == (gpointer)current_view())
        sync_ui();
}

static void on_tabs_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    sync_ui();
}

static void on_n_pages(GObject *o, GParamSpec *ps, gpointer d)
{
    if (adw_tab_view_get_n_pages(ui.tabs) == 0)
        gtk_window_close(GTK_WINDOW(ui.win));
}

static gboolean on_close_page(AdwTabView *tv, AdwTabPage *page, gpointer d)
{
    char *rec = NULL;
    LiteView *lv = lite_of(page);
    if (lv) {
        if (lite_view_url(lv))
            rec = g_strconcat("lite+", lite_view_url(lv), NULL);
    } else {
        WebKitWebView *v = view_of(page);
        const char *uri = v ? webkit_web_view_get_uri(v) : NULL;
        if (uri && !is_internal(uri))
            rec = g_strdup(uri);
    }
    if (rec && !skip_record) {
        if (n_closed == CLOSED_MAX) {
            g_free(closed[0]);
            memmove(closed, closed + 1, sizeof(char *) * (CLOSED_MAX - 1));
            n_closed--;
        }
        closed[n_closed++] = rec;
    } else {
        g_free(rec);
    }
    return GDK_EVENT_PROPAGATE;
}

static void on_title(GObject *o, GParamSpec *ps, gpointer d)
{
    WebKitWebView *v = WEBKIT_WEB_VIEW(o);
    AdwTabPage *page = adw_tab_view_get_page(ui.tabs, GTK_WIDGET(v));
    if (!page)
        return;
    const char *t = webkit_web_view_get_title(v);
    adw_tab_page_set_title(page, (t && *t) ? t : "Neuer Tab");
}

static void on_loading(GObject *o, GParamSpec *ps, gpointer d)
{
    WebKitWebView *v = WEBKIT_WEB_VIEW(o);
    AdwTabPage *page = adw_tab_view_get_page(ui.tabs, GTK_WIDGET(v));
    if (page)
        adw_tab_page_set_loading(page, webkit_web_view_is_loading(v));
    on_view_changed(o, ps, d);
}

static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer d)
{
    return GTK_WIDGET(open_tab(NULL, v));
}

static gboolean is_web(const char *u)
{
    return g_str_has_prefix(u, "http://") || g_str_has_prefix(u, "https://");
}

static void on_entry_activate(GtkEntry *e, gpointer d)
{
    char *t = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))));
    gboolean force = FALSE;
    if (g_str_has_prefix(t, "lite ")) {
        force = TRUE;
        memmove(t, t + 5, strlen(t + 5) + 1);
        g_strstrip(t);
    }
    LiteView *lv = current_lite();
    WebKitWebView *v = current_view();
    char *uri = normalize(t, force || lv != NULL);
    g_free(t);
    if (!uri)
        return;
    if (lv && is_web(uri)) {
        lite_view_load(lv, uri);
        lite_view_focus(lv);
    } else if (lv) {
        replace_current(uri, FALSE);
    } else if (force && is_web(uri)) {
        replace_current(uri, TRUE);
    } else if (v) {
        webkit_web_view_load_uri(v, uri);
        gtk_widget_grab_focus(GTK_WIDGET(v));
    }
    g_free(uri);
}

static WebKitWebView *open_tab(const char *uri, WebKitWebView *related)
{
    WebKitWebView *v;
    if (related)
        v = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, NULL));
    else
        v = WEBKIT_WEB_VIEW(webkit_web_view_new());

    gtk_widget_set_hexpand(GTK_WIDGET(v), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(v), TRUE);

    GdkRGBA bg;
    if (!gdk_rgba_parse(&bg, cfg.bg))
        gdk_rgba_parse(&bg, "#2e3440");
    webkit_web_view_set_background_color(v, &bg);

    apply_view_settings(v);
    webkit_settings_set_user_agent_with_application_details(webkit_web_view_get_settings(v), "Goontern", "0.2");

    g_signal_connect(v, "notify::uri", G_CALLBACK(on_view_changed), NULL);
    g_signal_connect(v, "notify::estimated-load-progress", G_CALLBACK(on_view_changed), NULL);
    g_signal_connect(v, "notify::is-loading", G_CALLBACK(on_loading), NULL);
    g_signal_connect(v, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(v, "create", G_CALLBACK(on_create), NULL);

    AdwTabPage *page = insert_pos >= 0 ? adw_tab_view_insert(ui.tabs, GTK_WIDGET(v), insert_pos)
                                       : adw_tab_view_append(ui.tabs, GTK_WIDGET(v));
    adw_tab_page_set_title(page, "Neuer Tab");
    adw_tab_view_set_selected_page(ui.tabs, page);

    if (!related) {
        if (uri) {
            webkit_web_view_load_uri(v, uri);
        } else {
            webkit_web_view_load_uri(v, START_URI);
            gtk_widget_grab_focus(GTK_WIDGET(ui.entry));
        }
    }
    return v;
}

static void lite_changed(LiteView *lv, gpointer d)
{
    AdwTabPage *page = adw_tab_view_get_page(ui.tabs, lite_view_widget(lv));
    if (!page)
        return;
    const char *t = lite_view_title(lv);
    adw_tab_page_set_title(page, (t && *t) ? t : "Lite");
    adw_tab_page_set_loading(page, lite_view_loading(lv));
    if (page == adw_tab_view_get_selected_page(ui.tabs))
        sync_ui();
}

static void lite_open_link(LiteView *lv, const char *url, gboolean bg, gpointer d)
{
    open_lite_tab(url, bg);
}

static LiteView *open_lite_tab(const char *url, gboolean background)
{
    LiteHooks h = { lite_changed, lite_open_link, NULL };
    LiteView *lv = lite_view_new(&h);
    GtkWidget *w = lite_view_widget(lv);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_widget_set_vexpand(w, TRUE);
    AdwTabPage *page = insert_pos >= 0 ? adw_tab_view_insert(ui.tabs, w, insert_pos)
                                       : adw_tab_view_append(ui.tabs, w);
    adw_tab_page_set_title(page, "Lite");
    GIcon *ic = g_themed_icon_new("text-x-generic-symbolic");
    adw_tab_page_set_icon(page, ic);
    g_object_unref(ic);
    if (!background)
        adw_tab_view_set_selected_page(ui.tabs, page);
    lite_view_load(lv, url);
    return lv;
}

static void replace_current(const char *uri, gboolean lite)
{
    AdwTabPage *old = adw_tab_view_get_selected_page(ui.tabs);
    if (!old)
        return;
    insert_pos = adw_tab_view_get_page_position(ui.tabs, old) + 1;
    if (lite)
        open_lite_tab(uri, FALSE);
    else
        open_tab(uri, NULL);
    insert_pos = -1;
    skip_record = TRUE;
    adw_tab_view_close_page(ui.tabs, old);
    skip_record = FALSE;
}

static void act_new_tab(GSimpleAction *a, GVariant *p, gpointer d)
{
    open_tab(NULL, NULL);
}

static void act_close_tab(GSimpleAction *a, GVariant *p, gpointer d)
{
    AdwTabPage *page = adw_tab_view_get_selected_page(ui.tabs);
    if (page)
        adw_tab_view_close_page(ui.tabs, page);
}

static void act_reopen_tab(GSimpleAction *a, GVariant *p, gpointer d)
{
    if (n_closed == 0)
        return;
    char *uri = closed[--n_closed];
    if (g_str_has_prefix(uri, "lite+"))
        open_lite_tab(uri + 5, FALSE);
    else
        open_tab(uri, NULL);
    g_free(uri);
}

static void act_focus_url(GSimpleAction *a, GVariant *p, gpointer d)
{
    gtk_widget_grab_focus(GTK_WIDGET(ui.entry));
    gtk_editable_select_region(GTK_EDITABLE(ui.entry), 0, -1);
}

static void act_reload(GSimpleAction *a, GVariant *p, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_reload(lv);
        return;
    }
    WebKitWebView *v = current_view();
    if (v)
        webkit_web_view_reload(v);
}

static void act_hard_reload(GSimpleAction *a, GVariant *p, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_reload(lv);
        return;
    }
    WebKitWebView *v = current_view();
    if (v)
        webkit_web_view_reload_bypass_cache(v);
}

static void act_back(GSimpleAction *a, GVariant *p, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_back(lv);
        return;
    }
    WebKitWebView *v = current_view();
    if (v)
        webkit_web_view_go_back(v);
}

static void act_forward(GSimpleAction *a, GVariant *p, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_forward(lv);
        return;
    }
    WebKitWebView *v = current_view();
    if (v)
        webkit_web_view_go_forward(v);
}

static void act_home(GSimpleAction *a, GVariant *p, gpointer d)
{
    if (current_lite()) {
        replace_current(START_URI, FALSE);
        return;
    }
    WebKitWebView *v = current_view();
    if (v)
        webkit_web_view_load_uri(v, START_URI);
}

static void zoom_by(double f)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_zoom(lv, f, FALSE);
        return;
    }
    WebKitWebView *v = current_view();
    if (!v)
        return;
    double z = webkit_web_view_get_zoom_level(v) * f;
    if (z < 0.3)
        z = 0.3;
    if (z > 5.0)
        z = 5.0;
    webkit_web_view_set_zoom_level(v, z);
}

static void act_zoom_in(GSimpleAction *a, GVariant *p, gpointer d)
{
    zoom_by(1.1);
}

static void act_zoom_out(GSimpleAction *a, GVariant *p, gpointer d)
{
    zoom_by(1.0 / 1.1);
}

static void act_zoom_reset(GSimpleAction *a, GVariant *p, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_zoom(lv, 1.0, TRUE);
        return;
    }
    WebKitWebView *v = current_view();
    if (v)
        webkit_web_view_set_zoom_level(v, 1.0);
}

static void act_find(GSimpleAction *a, GVariant *p, gpointer d)
{
    gtk_search_bar_set_search_mode(ui.sbar, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(ui.sentry));
    gtk_editable_select_region(GTK_EDITABLE(ui.sentry), 0, -1);
}

static void act_fullscreen(GSimpleAction *a, GVariant *p, gpointer d)
{
    if (gtk_window_is_fullscreen(GTK_WINDOW(ui.win)))
        gtk_window_unfullscreen(GTK_WINDOW(ui.win));
    else
        gtk_window_fullscreen(GTK_WINDOW(ui.win));
}

static void act_toggle_lite(GSimpleAction *a, GVariant *p, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        const char *u = lite_view_url(lv);
        if (u)
            replace_current(u, FALSE);
        return;
    }
    WebKitWebView *v = current_view();
    const char *u = v ? webkit_web_view_get_uri(v) : NULL;
    if (u && is_web(u))
        replace_current(u, TRUE);
}

static void act_next_change(GSimpleAction *a, GVariant *p, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv)
        lite_view_jump_change(lv, TRUE);
}

static void act_prev_change(GSimpleAction *a, GVariant *p, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv)
        lite_view_jump_change(lv, FALSE);
}

static void act_quit(GSimpleAction *a, GVariant *p, gpointer d)
{
    g_application_quit(ui.app);
}

static void act_settings(GSimpleAction *a, GVariant *p, gpointer d);

static const GActionEntry ACTIONS[] = {
    { "new-tab", act_new_tab, NULL, NULL, NULL },
    { "close-tab", act_close_tab, NULL, NULL, NULL },
    { "reopen-tab", act_reopen_tab, NULL, NULL, NULL },
    { "focus-url", act_focus_url, NULL, NULL, NULL },
    { "reload", act_reload, NULL, NULL, NULL },
    { "hard-reload", act_hard_reload, NULL, NULL, NULL },
    { "back", act_back, NULL, NULL, NULL },
    { "forward", act_forward, NULL, NULL, NULL },
    { "home", act_home, NULL, NULL, NULL },
    { "zoom-in", act_zoom_in, NULL, NULL, NULL },
    { "zoom-out", act_zoom_out, NULL, NULL, NULL },
    { "zoom-reset", act_zoom_reset, NULL, NULL, NULL },
    { "find", act_find, NULL, NULL, NULL },
    { "fullscreen", act_fullscreen, NULL, NULL, NULL },
    { "settings", act_settings, NULL, NULL, NULL },
    { "toggle-lite", act_toggle_lite, NULL, NULL, NULL },
    { "next-change", act_next_change, NULL, NULL, NULL },
    { "prev-change", act_prev_change, NULL, NULL, NULL },
    { "quit", act_quit, NULL, NULL, NULL },
};

static void on_search_changed(GtkSearchEntry *e, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_find(lv, gtk_editable_get_text(GTK_EDITABLE(e)), TRUE);
        return;
    }
    WebKitWebView *v = current_view();
    if (!v)
        return;
    const char *t = gtk_editable_get_text(GTK_EDITABLE(e));
    WebKitFindController *fc = webkit_web_view_get_find_controller(v);
    if (*t)
        webkit_find_controller_search(fc, t,
            WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, G_MAXUINT);
    else
        webkit_find_controller_search_finish(fc);
}

static void on_search_next(GtkSearchEntry *e, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_find(lv, gtk_editable_get_text(GTK_EDITABLE(e)), TRUE);
        return;
    }
    WebKitWebView *v = current_view();
    if (v)
        webkit_find_controller_search_next(webkit_web_view_get_find_controller(v));
}

static void on_search_prev(GtkSearchEntry *e, gpointer d)
{
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_find(lv, gtk_editable_get_text(GTK_EDITABLE(e)), FALSE);
        return;
    }
    WebKitWebView *v = current_view();
    if (v)
        webkit_find_controller_search_previous(webkit_web_view_get_find_controller(v));
}

static void on_search_mode(GObject *o, GParamSpec *ps, gpointer d)
{
    if (gtk_search_bar_get_search_mode(ui.sbar))
        return;
    LiteView *lv = current_lite();
    if (lv) {
        lite_view_find_clear(lv);
        lite_view_focus(lv);
        return;
    }
    WebKitWebView *v = current_view();
    if (v) {
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(v));
        gtk_widget_grab_focus(GTK_WIDGET(v));
    }
}

static void on_sw_smooth(GObject *o, GParamSpec *ps, gpointer d)
{
    cfg.smooth = adw_switch_row_get_active(ADW_SWITCH_ROW(o));
    cfg_save();
    apply_all();
}

static void on_sw_gpu(GObject *o, GParamSpec *ps, gpointer d)
{
    cfg.gpu = adw_switch_row_get_active(ADW_SWITCH_ROW(o));
    cfg_save();
    apply_all();
}

static void on_sw_gesture(GObject *o, GParamSpec *ps, gpointer d)
{
    cfg.gesture = adw_switch_row_get_active(ADW_SWITCH_ROW(o));
    cfg_save();
    apply_all();
}

static void on_sw_delta(GObject *o, GParamSpec *ps, gpointer d)
{
    cfg.delta = adw_switch_row_get_active(ADW_SWITCH_ROW(o));
    cfg_save();
    lite_set_options(cfg.delta, cfg.lite_images);
}

static void on_sw_limg(GObject *o, GParamSpec *ps, gpointer d)
{
    cfg.lite_images = adw_switch_row_get_active(ADW_SWITCH_ROW(o));
    cfg_save();
    lite_set_options(cfg.delta, cfg.lite_images);
}

static void on_delta_clear(GtkButton *b, gpointer d)
{
    lite_delta_clear_all();
    gtk_button_set_label(b, "Gel\xc3\xb6scht");
}

static struct {
    AdwComboRow *combo;
    GtkStringList *names;
    AdwPreferencesGroup *list;
    GPtrArray *rows;
    AdwEntryRow *ename;
    AdwEntryRow *eurl;
    gboolean building;
} se;

static void rebuild_engines(void);

static void on_engine(GObject *o, GParamSpec *ps, gpointer d)
{
    if (se.building)
        return;
    cfg.engine = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(o));
    cfg_save();
    reload_start_tabs();
}

static void on_remove_engine(GtkButton *b, gpointer d)
{
    guint idx;
    if (!customs || !g_ptr_array_find(customs, d, &idx))
        return;
    int gi = NBUILTIN + (int)idx;
    if (cfg.engine == gi)
        cfg.engine = 0;
    else if (cfg.engine > gi)
        cfg.engine--;
    g_ptr_array_remove_index(customs, idx);
    cfg_save();
    rebuild_engines();
    reload_start_tabs();
}

static void rebuild_engines(void)
{
    se.building = TRUE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(se.names));
    gtk_string_list_splice(se.names, 0, n, NULL);
    for (int i = 0; i < engine_count(); i++)
        gtk_string_list_append(se.names, engine_name(i));
    adw_combo_row_set_selected(se.combo, (guint)cfg.engine);
    se.building = FALSE;
    for (guint i = 0; i < se.rows->len; i++)
        adw_preferences_group_remove(se.list, g_ptr_array_index(se.rows, i));
    g_ptr_array_set_size(se.rows, 0);
    guint nc = customs ? customs->len : 0;
    for (guint i = 0; i < nc; i++) {
        CustomEngine *c = g_ptr_array_index(customs, i);
        AdwActionRow *r = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), c->name);
        adw_action_row_set_subtitle(r, c->tmpl);
        GtkWidget *b = gtk_button_new_with_label("Entfernen");
        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        g_signal_connect(b, "clicked", G_CALLBACK(on_remove_engine), c);
        adw_action_row_add_suffix(r, b);
        adw_preferences_group_add(se.list, GTK_WIDGET(r));
        g_ptr_array_add(se.rows, r);
    }
    gtk_widget_set_visible(GTK_WIDGET(se.list), nc > 0);
}

static void on_add_engine(GtkButton *b, gpointer d)
{
    char *nm = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(se.ename))));
    char *ur = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(se.eurl))));
    gboolean nok = *nm && strlen(nm) <= 40;
    gboolean uok = valid_tmpl(ur);
    if (nok)
        gtk_widget_remove_css_class(GTK_WIDGET(se.ename), "error");
    else
        gtk_widget_add_css_class(GTK_WIDGET(se.ename), "error");
    if (uok)
        gtk_widget_remove_css_class(GTK_WIDGET(se.eurl), "error");
    else
        gtk_widget_add_css_class(GTK_WIDGET(se.eurl), "error");
    if (nok && uok && engine_count() < NBUILTIN + 20) {
        custom_add(nm, ur);
        cfg.engine = engine_count() - 1;
        gtk_editable_set_text(GTK_EDITABLE(se.ename), "");
        gtk_editable_set_text(GTK_EDITABLE(se.eurl), "");
        cfg_save();
        rebuild_engines();
        reload_start_tabs();
    }
    g_free(nm);
    g_free(ur);
}

static void on_dim(GObject *o, GParamSpec *ps, gpointer d)
{
    cfg.dim = adw_spin_row_get_value(ADW_SPIN_ROW(o));
    cfg_save();
    reload_start_tabs();
}

static void on_blur(GObject *o, GParamSpec *ps, gpointer d)
{
    cfg.blur = adw_spin_row_get_value(ADW_SPIN_ROW(o));
    cfg_save();
    reload_start_tabs();
}

static void on_color(GObject *o, GParamSpec *ps, gpointer d)
{
    const GdkRGBA *c = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(o));
    char *s = gdk_rgba_to_string(c);
    g_strlcpy((char *)d, s, 48);
    g_free(s);
    cfg_save();
    reload_start_tabs();
}

static void set_background(GFile *f)
{
    char *path = g_file_get_path(f);
    if (!path)
        return;
    int w = 0, h = 0;
    GError *err = NULL;
    GdkPixbuf *pb = NULL;
    if (gdk_pixbuf_get_file_info(path, &w, &h) && (w > 2560 || h > 1440))
        pb = gdk_pixbuf_new_from_file_at_scale(path, 2560, 1440, TRUE, &err);
    else
        pb = gdk_pixbuf_new_from_file(path, &err);
    g_free(path);
    if (!pb) {
        g_clear_error(&err);
        return;
    }
    GdkPixbuf *rot = gdk_pixbuf_apply_embedded_orientation(pb);
    if (rot) {
        g_object_unref(pb);
        pb = rot;
    }
    conf_mkdir();
    char *out = conf_file("background.jpg");
    gboolean ok = gdk_pixbuf_save(pb, out, "jpeg", NULL, "quality", "88", NULL);
    g_free(out);
    g_object_unref(pb);
    if (!ok)
        return;
    cfg.image = TRUE;
    cfg.ver++;
    cfg_save();
    reload_start_tabs();
}

static void on_file_chosen(GObject *src, GAsyncResult *res, gpointer d)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        set_background(f);
        g_object_unref(f);
    }
    g_object_unref(src);
}

static void on_pick_image(GtkButton *b, gpointer d)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Hintergrundbild wählen");
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "Bilder");
    gtk_file_filter_add_mime_type(flt, "image/*");
    GListStore *fs = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(fs, flt);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(fs));
    g_object_unref(flt);
    g_object_unref(fs);
    gtk_file_dialog_open(dlg, GTK_WINDOW(ui.win), NULL, on_file_chosen, NULL);
}

static void on_clear_image(GtkButton *b, gpointer d)
{
    char *p = conf_file("background.jpg");
    g_remove(p);
    g_free(p);
    cfg.image = FALSE;
    cfg_save();
    reload_start_tabs();
}

static AdwSwitchRow *make_switch(const char *title, const char *sub, gboolean on, GCallback cb)
{
    AdwSwitchRow *r = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(r), sub);
    adw_switch_row_set_active(r, on);
    g_signal_connect(r, "notify::active", cb, NULL);
    return r;
}

static AdwActionRow *make_color_row(const char *title, char *target)
{
    AdwActionRow *r = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    GtkColorDialogButton *b = GTK_COLOR_DIALOG_BUTTON(gtk_color_dialog_button_new(gtk_color_dialog_new()));
    GdkRGBA c;
    if (gdk_rgba_parse(&c, target))
        gtk_color_dialog_button_set_rgba(b, &c);
    gtk_widget_set_valign(GTK_WIDGET(b), GTK_ALIGN_CENTER);
    g_signal_connect(b, "notify::rgba", G_CALLBACK(on_color), target);
    adw_action_row_add_suffix(r, GTK_WIDGET(b));
    return r;
}

static AdwSpinRow *make_spin(const char *title, const char *sub, double max, double step, double val, GCallback cb)
{
    AdwSpinRow *r = ADW_SPIN_ROW(adw_spin_row_new_with_range(0, max, step));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(r), sub);
    adw_spin_row_set_value(r, val);
    g_signal_connect(r, "notify::value", cb, NULL);
    return r;
}

static AdwPreferencesGroup *make_group(AdwPreferencesPage *page, const char *title)
{
    AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, title);
    adw_preferences_page_add(page, g);
    return g;
}

static void act_settings(GSimpleAction *a, GVariant *p, gpointer d)
{
    AdwPreferencesDialog *dlg = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_title(ADW_DIALOG(dlg), "Einstellungen");

    AdwPreferencesPage *p1 = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(p1, "Allgemein");
    adw_preferences_page_set_icon_name(p1, "preferences-system-symbolic");

    AdwPreferencesGroup *g = make_group(p1, "Suche");
    se.combo = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(se.combo), "Suchmaschine");
    se.names = gtk_string_list_new(NULL);
    adw_combo_row_set_model(se.combo, G_LIST_MODEL(se.names));
    g_object_unref(se.names);
    g_signal_connect(se.combo, "notify::selected", G_CALLBACK(on_engine), NULL);
    adw_preferences_group_add(g, GTK_WIDGET(se.combo));

    g = make_group(p1, "Eigene Suchmaschine hinzuf\xc3\xbcgen");
    adw_preferences_group_set_description(g, "Adresse mit %s als Platzhalter f\xc3\xbcr den Suchbegriff, zum Beispiel https://meine.suche/?q=%s");
    se.ename = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(se.ename), "Name");
    se.eurl = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(se.eurl), "Adresse mit %s");
    AdwActionRow *addrow = ADW_ACTION_ROW(adw_action_row_new());
    GtkWidget *addb = gtk_button_new_with_label("Hinzuf\xc3\xbcgen");
    gtk_widget_set_valign(addb, GTK_ALIGN_CENTER);
    g_signal_connect(addb, "clicked", G_CALLBACK(on_add_engine), NULL);
    adw_action_row_add_suffix(addrow, addb);
    adw_preferences_group_add(g, GTK_WIDGET(se.ename));
    adw_preferences_group_add(g, GTK_WIDGET(se.eurl));
    adw_preferences_group_add(g, GTK_WIDGET(addrow));
    se.list = make_group(p1, "Gespeicherte Suchmaschinen");
    se.rows = g_ptr_array_new();
    rebuild_engines();

    g = make_group(p1, "Geschwindigkeit");
    adw_preferences_group_add(g, GTK_WIDGET(make_switch("GPU-Beschleunigung",
        "Aus rendert nur mit der CPU und ist deutlich langsamer", cfg.gpu, G_CALLBACK(on_sw_gpu))));
    adw_preferences_group_add(g, GTK_WIDGET(make_switch("Sanftes Scrollen",
        "Aus ist schneller und direkter", cfg.smooth, G_CALLBACK(on_sw_smooth))));
    adw_preferences_group_add(g, GTK_WIDGET(make_switch("Autoplay blockieren",
        "Medien starten erst nach einem Klick und sparen Ressourcen", cfg.gesture, G_CALLBACK(on_sw_gesture))));

    g = make_group(p1, "Lite-Engine");
    adw_preferences_group_add(g, GTK_WIDGET(make_switch("Delta: \xc3\x84nderungen markieren",
        "Zeigt beim n\xc3\xa4" "chsten Besuch, was auf einer Seite neu oder ge\xc3\xa4ndert ist", cfg.delta, G_CALLBACK(on_sw_delta))));
    adw_preferences_group_add(g, GTK_WIDGET(make_switch("Bilder automatisch laden",
        "Aus spart Daten, Bilder laden dann per Klick", cfg.lite_images, G_CALLBACK(on_sw_limg))));
    AdwActionRow *dc = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dc), "Delta-Verlauf");
    adw_action_row_set_subtitle(dc, "Pro Seite werden nur Pr\xc3\xbc" "fsummen gespeichert, nie der Seiteninhalt");
    GtkWidget *dcb = gtk_button_new_with_label("L\xc3\xb6schen");
    gtk_widget_set_valign(dcb, GTK_ALIGN_CENTER);
    g_signal_connect(dcb, "clicked", G_CALLBACK(on_delta_clear), NULL);
    adw_action_row_add_suffix(dc, dcb);
    adw_preferences_group_add(g, GTK_WIDGET(dc));

    AdwPreferencesPage *p2 = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(p2, "Startseite");
    adw_preferences_page_set_icon_name(p2, "applications-graphics-symbolic");

    g = make_group(p2, "Hintergrund");
    AdwActionRow *img = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(img), "Eigenes Bild");
    adw_action_row_set_subtitle(img, "Wird für schnelles Laden auf maximal 2560x1440 verkleinert");
    GtkWidget *pick = gtk_button_new_with_label("Wählen");
    GtkWidget *clr = gtk_button_new_with_label("Entfernen");
    gtk_widget_set_valign(pick, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(clr, GTK_ALIGN_CENTER);
    g_signal_connect(pick, "clicked", G_CALLBACK(on_pick_image), NULL);
    g_signal_connect(clr, "clicked", G_CALLBACK(on_clear_image), NULL);
    adw_action_row_add_suffix(img, pick);
    adw_action_row_add_suffix(img, clr);
    adw_preferences_group_add(g, GTK_WIDGET(img));
    adw_preferences_group_add(g, GTK_WIDGET(make_spin("Abdunkeln", "Prozent, damit Text lesbar bleibt",
        90, 5, cfg.dim, G_CALLBACK(on_dim))));
    adw_preferences_group_add(g, GTK_WIDGET(make_spin("Unschärfe", "Pixel, 0 ist scharf",
        40, 2, cfg.blur, G_CALLBACK(on_blur))));

    g = make_group(p2, "Farben");
    adw_preferences_group_add(g, GTK_WIDGET(make_color_row("Hintergrundfarbe", cfg.bg)));
    adw_preferences_group_add(g, GTK_WIDGET(make_color_row("Akzentfarbe", cfg.accent)));

    AdwPreferencesPage *p3 = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(p3, "Tastenkürzel");
    adw_preferences_page_set_icon_name(p3, "input-keyboard-symbolic");
    g = make_group(p3, "Tabs");
    static const char *keys[][2] = {
        { "Neuer Tab", "Strg+E  /  Strg+T" },
        { "Tab schließen", "Strg+W" },
        { "Geschlossenen Tab wiederherstellen", "Strg+Umschalt+T" },
        { "Nächster / vorheriger Tab", "Strg+Tab  /  Strg+Umschalt+Tab" },
        { "Tab 1 bis 9", "Alt+1 bis Alt+9" },
        { "Lite-Engine f\xc3\xbcr diesen Tab ein / aus", "Strg+Umschalt+L" },
        { "N\xc3\xa4" "chste / vorherige \xc3\x84nderung (Lite)", "Alt+Runter  /  Alt+Hoch" },
        { "Tab nach links / rechts verschieben", "Strg+Umschalt+Bild auf / ab" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
        AdwActionRow *r = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), keys[i][0]);
        GtkWidget *l = gtk_label_new(keys[i][1]);
        gtk_widget_add_css_class(l, "dim-label");
        adw_action_row_add_suffix(r, l);
        adw_preferences_group_add(g, GTK_WIDGET(r));
    }
    g = make_group(p3, "Navigation");
    static const char *keys2[][2] = {
        { "Adressleiste", "Strg+L  /  F6  /  Alt+D" },
        { "Zurück / Vor", "Alt+Links  /  Alt+Rechts" },
        { "Startseite", "Alt+Pos1" },
        { "Adresse direkt in Lite \xc3\xb6" "ffnen", "lite <Adresse> in die Adressleiste" },
        { "Neu laden", "Strg+R  /  F5" },
        { "Neu laden ohne Cache", "Strg+Umschalt+R  /  Umschalt+F5" },
        { "Auf der Seite suchen", "Strg+F" },
        { "Zoom größer / kleiner / zurücksetzen", "Strg+Plus  /  Strg+Minus  /  Strg+0" },
        { "Vollbild", "F11" },
        { "Einstellungen", "Strg+Komma" },
        { "Beenden", "Strg+Q" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(keys2); i++) {
        AdwActionRow *r = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), keys2[i][0]);
        GtkWidget *l = gtk_label_new(keys2[i][1]);
        gtk_widget_add_css_class(l, "dim-label");
        adw_action_row_add_suffix(r, l);
        adw_preferences_group_add(g, GTK_WIDGET(r));
    }

    adw_preferences_dialog_add(dlg, p1);
    adw_preferences_dialog_add(dlg, p2);
    adw_preferences_dialog_add(dlg, p3);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(ui.win));
}

static GtkWidget *icon_button(const char *icon, const char *action, const char *tip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

static void set_accel(GtkApplication *app, const char *action, const char *a, const char *b, const char *c)
{
    const char *list[] = { a, b, c, NULL };
    gtk_application_set_accels_for_action(app, action, list);
}

static void build_window(GtkApplication *app)
{
    ui.app = G_APPLICATION(app);

    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, THEME_CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    WebKitWebContext *ctx = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(ctx, "goontern", on_scheme, NULL, NULL);
    webkit_web_context_set_cache_model(ctx, WEBKIT_CACHE_MODEL_WEB_BROWSER);

    WebKitNetworkSession *ns = webkit_network_session_get_default();
    webkit_network_session_set_itp_enabled(ns, TRUE);
    webkit_cookie_manager_set_accept_policy(webkit_network_session_get_cookie_manager(ns),
        WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);

    g_action_map_add_action_entries(G_ACTION_MAP(app), ACTIONS, G_N_ELEMENTS(ACTIONS), NULL);
    set_accel(app, "app.new-tab", "<Control>e", "<Control>t", NULL);
    set_accel(app, "app.close-tab", "<Control>w", NULL, NULL);
    set_accel(app, "app.reopen-tab", "<Control><Shift>t", NULL, NULL);
    set_accel(app, "app.focus-url", "<Control>l", "F6", "<Alt>d");
    set_accel(app, "app.reload", "<Control>r", "F5", NULL);
    set_accel(app, "app.hard-reload", "<Control><Shift>r", "<Shift>F5", NULL);
    set_accel(app, "app.back", "<Alt>Left", NULL, NULL);
    set_accel(app, "app.forward", "<Alt>Right", NULL, NULL);
    set_accel(app, "app.home", "<Alt>Home", NULL, NULL);
    set_accel(app, "app.zoom-in", "<Control>plus", "<Control>equal", "<Control>KP_Add");
    set_accel(app, "app.zoom-out", "<Control>minus", "<Control>KP_Subtract", NULL);
    set_accel(app, "app.zoom-reset", "<Control>0", "<Control>KP_0", NULL);
    set_accel(app, "app.find", "<Control>f", NULL, NULL);
    set_accel(app, "app.fullscreen", "F11", NULL, NULL);
    set_accel(app, "app.settings", "<Control>comma", NULL, NULL);
    set_accel(app, "app.quit", "<Control>q", NULL, NULL);
    set_accel(app, "app.toggle-lite", "<Control><Shift>l", NULL, NULL);
    set_accel(app, "app.next-change", "<Alt>Down", NULL, NULL);
    set_accel(app, "app.prev-change", "<Alt>Up", NULL, NULL);

    ui.win = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(ui.win), "Goontern");
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 1280, 800);

    ui.tabs = ADW_TAB_VIEW(adw_tab_view_new());
    AdwTabBar *bar = adw_tab_bar_new();
    adw_tab_bar_set_view(bar, ui.tabs);
    adw_tab_bar_set_autohide(bar, TRUE);

    ui.entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_add_css_class(GTK_WIDGET(ui.entry), "url");
    gtk_widget_set_hexpand(GTK_WIDGET(ui.entry), TRUE);
    gtk_entry_set_placeholder_text(ui.entry, "Suche oder Adresse eingeben");

    AdwClamp *clamp = ADW_CLAMP(adw_clamp_new());
    adw_clamp_set_maximum_size(clamp, 720);
    adw_clamp_set_child(clamp, GTK_WIDGET(ui.entry));

    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), icon_button("go-previous-symbolic", "app.back", "Zurück"));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), icon_button("go-next-symbolic", "app.forward", "Vor"));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), icon_button("view-refresh-symbolic", "app.reload", "Neu laden"));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(clamp));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), icon_button("preferences-system-symbolic", "app.settings", "Einstellungen"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), icon_button("tab-new-symbolic", "app.new-tab", "Neuer Tab"));

    ui.sentry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(ui.sentry), TRUE);
    ui.sbar = GTK_SEARCH_BAR(gtk_search_bar_new());
    gtk_search_bar_set_child(ui.sbar, GTK_WIDGET(ui.sentry));
    gtk_search_bar_connect_entry(ui.sbar, GTK_EDITABLE(ui.sentry));
    gtk_search_bar_set_show_close_button(ui.sbar, TRUE);

    AdwToolbarView *tv = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(tv, header);
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(bar));
    adw_toolbar_view_add_top_bar(tv, GTK_WIDGET(ui.sbar));
    adw_toolbar_view_set_content(tv, GTK_WIDGET(ui.tabs));
    adw_application_window_set_content(ui.win, GTK_WIDGET(tv));

    g_signal_connect(ui.entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    g_signal_connect(ui.tabs, "notify::selected-page", G_CALLBACK(on_tabs_changed), NULL);
    g_signal_connect(ui.tabs, "notify::n-pages", G_CALLBACK(on_n_pages), NULL);
    g_signal_connect(ui.tabs, "close-page", G_CALLBACK(on_close_page), NULL);
    g_signal_connect(ui.sentry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(ui.sentry, "activate", G_CALLBACK(on_search_next), NULL);
    g_signal_connect(ui.sentry, "next-match", G_CALLBACK(on_search_next), NULL);
    g_signal_connect(ui.sentry, "previous-match", G_CALLBACK(on_search_prev), NULL);
    g_signal_connect(ui.sbar, "notify::search-mode-enabled", G_CALLBACK(on_search_mode), NULL);

    gtk_window_present(GTK_WINDOW(ui.win));
}

static void ensure_window(GtkApplication *app)
{
    if (!ui.win)
        build_window(app);
    else
        gtk_window_present(GTK_WINDOW(ui.win));
}

static void on_activate(GApplication *app, gpointer d)
{
    gboolean fresh = (ui.win == NULL);
    ensure_window(GTK_APPLICATION(app));
    if (fresh)
        open_tab(NULL, NULL);
}

static void on_open(GApplication *app, GFile **files, int n, const char *hint, gpointer d)
{
    ensure_window(GTK_APPLICATION(app));
    for (int i = 0; i < n; i++) {
        char *u = g_file_get_uri(files[i]);
        if (g_str_has_prefix(u, "lite:"))
            open_lite_tab(u + 5, FALSE);
        else
            open_tab(u, NULL);
        g_free(u);
    }
}

int main(int argc, char **argv)
{
    cfg_load();
    lite_set_options(cfg.delta, cfg.lite_images);
    AdwApplication *app = adw_application_new("org.goontern.Browser", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
