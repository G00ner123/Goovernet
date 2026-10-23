#include "lite.h"
#include <libsoup/soup.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <string.h>
#include <time.h>

#define MAX_BODY (4 * 1024 * 1024)
#define MAX_IMG (8 * 1024 * 1024)
#define MAX_TEXT_W 860
#define MAX_BLOCKS 6000
#define MAX_AUTO_IMG 24
#define MAX_IMG_W 760

enum {
    F_B = 1 << 0,
    F_I = 1 << 1,
    F_CODE = 1 << 2,
    F_U = 1 << 3,
    F_S = 1 << 4,
    F_MARK = 1 << 5,
    F_SUP = 1 << 6,
    F_SUB = 1 << 7,
    F_SMALL = 1 << 8,
    F_DIM = 1 << 9
};

#define NFLAGS 10

typedef struct {
    GString *text;
    guint flags;
    GtkTextTag *link;
} Run;

typedef struct {
    guint64 hash;
    int start;
} BlockRec;

typedef struct {
    GtkTextTag *tag;
    GtkTextMark *mark;
} ImgRef;

typedef struct {
    GtkTextBuffer *buf;
    GHashTable *paras;
    GHashTable *links;
    GPtrArray *scaled;
    GtkTextTag *inl[NFLAGS];
    GtkTextTag *body;
    GtkTextTag *delta;
    GtkTextTag *spacer;
    GtkTextTag *fall;
    GtkTextTag *fcur;
    GPtrArray *changes;
    GPtrArray *fs;
    GPtrArray *fe;
    int fidx;
    char *fquery;
} Doc;

typedef struct {
    char *url;
    double scroll;
} HistEntry;

struct _LiteView {
    LiteHooks hooks;
    GtkWidget *root;
    GtkWidget *bar;
    GtkWidget *barlabel;
    GtkWidget *scroll;
    GtkWidget *tv;
    GtkWidget *status;
    Doc *doc;
    char *url;
    char *title;
    gboolean loading;
    double progress;
    GPtrArray *hist;
    int hpos;
    GCancellable *cancel;
    double zoom;
    GHashTable *baseline;
    char *baseline_url;
    gint64 baseline_time;
    int cidx;
    char *barbase;
    char *hover;
    int last_w;
};

typedef struct {
    LiteView *lv;
    Doc *doc;
    GtkTextBuffer *buf;
    GArray *runs;
    guint flagcnt[NFLAGS];
    guint flags;
    GtkTextTag *link;
    char *base;
    GString *title;
    gboolean in_title;
    gboolean got_title;
    gboolean in_head;
    int pre;
    gboolean pre_first;
    int heading;
    GArray *lists;
    int quote;
    int dd;
    char *marker;
    gboolean row_has_cell;
    char *skip_name;
    int skip_depth;
    GPtrArray *pending_ids;
    gboolean last_space;
    GArray *blocks;
    GArray *imgs;
    int nimg;
} Parser;

typedef struct {
    LiteView *lv;
    GCancellable *cancel;
    SoupMessage *msg;
    char *url;
    char *frag;
    gboolean push;
    double scroll;
    gboolean restore;
} Job;

typedef struct {
    LiteView *lv;
    GCancellable *cancel;
    SoupMessage *msg;
    GtkTextTag *tag;
    GtkTextMark *mark;
} ImgJob;

typedef struct {
    LiteView *lv;
    double target;
    int frames;
} Restore;

static gboolean opt_delta = TRUE;
static gboolean opt_images = FALSE;
static SoupSession *session;

static const struct {
    const char *n;
    const char *v;
} ENT[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
    { "nbsp", "\xc2\xa0" }, { "copy", "\xc2\xa9" }, { "reg", "\xc2\xae" },
    { "trade", "\xe2\x84\xa2" }, { "hellip", "\xe2\x80\xa6" }, { "mdash", "\xe2\x80\x94" },
    { "ndash", "\xe2\x80\x93" }, { "lsquo", "\xe2\x80\x98" }, { "rsquo", "\xe2\x80\x99" },
    { "ldquo", "\xe2\x80\x9c" }, { "rdquo", "\xe2\x80\x9d" }, { "laquo", "\xc2\xab" },
    { "raquo", "\xc2\xbb" }, { "bull", "\xe2\x80\xa2" }, { "middot", "\xc2\xb7" },
    { "times", "\xc3\x97" }, { "deg", "\xc2\xb0" }, { "euro", "\xe2\x82\xac" },
    { "pound", "\xc2\xa3" }, { "yen", "\xc2\xa5" }, { "cent", "\xc2\xa2" },
    { "sect", "\xc2\xa7" }, { "para", "\xc2\xb6" }, { "larr", "\xe2\x86\x90" },
    { "rarr", "\xe2\x86\x92" }, { "uarr", "\xe2\x86\x91" }, { "darr", "\xe2\x86\x93" },
    { "auml", "\xc3\xa4" }, { "ouml", "\xc3\xb6" }, { "uuml", "\xc3\xbc" },
    { "Auml", "\xc3\x84" }, { "Ouml", "\xc3\x96" }, { "Uuml", "\xc3\x9c" },
    { "szlig", "\xc3\x9f" }, { "shy", "" }, { "zwj", "" }, { "zwnj", "" },
};

static const char *const VOID_TAGS[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr", NULL
};

static const char *const SKIP_TAGS[] = {
    "script", "style", "template", "svg", "iframe", "canvas", "object", "audio", "video", "select",
    "textarea", "button", "math", "dialog", NULL
};

static const char *const BLOCK_TAGS[] = {
    "address", "article", "aside", "details", "div", "dl", "fieldset", "figcaption", "figure", "footer",
    "form", "header", "main", "nav", "p", "section", "table", "thead", "tbody", "tfoot", "caption",
    "summary", "center", "body", "html", "tr", "menu", "optgroup", NULL
};

static gboolean in_list(const char *name, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (strcmp(name, list[i]) == 0)
            return TRUE;
    return FALSE;
}

static void rgba(GdkRGBA *c, const char *s)
{
    gdk_rgba_parse(c, s);
}

static void notify(LiteView *lv)
{
    if (lv->hooks.changed)
        lv->hooks.changed(lv, lv->hooks.data);
}

static void decode_entities(GString *out, const char *s, gsize len)
{
    gsize i = 0;
    while (i < len) {
        if (s[i] != '&') {
            g_string_append_c(out, s[i]);
            i++;
            continue;
        }
        gsize lim = MIN(len, i + 12);
        gsize j = i + 1;
        while (j < lim && s[j] != ';' && s[j] != ' ' && s[j] != '&' && s[j] != '<')
            j++;
        gboolean done = FALSE;
        if (j < len && s[j] == ';' && j > i + 1) {
            const char *nm = s + i + 1;
            gsize nl = j - i - 1;
            if (nm[0] == '#' && nl > 1 && nl < 10) {
                char tmp[12];
                memcpy(tmp, nm + 1, nl - 1);
                tmp[nl - 1] = 0;
                gunichar u;
                if (tmp[0] == 'x' || tmp[0] == 'X')
                    u = (gunichar)g_ascii_strtoull(tmp + 1, NULL, 16);
                else
                    u = (gunichar)g_ascii_strtoull(tmp, NULL, 10);
                if (u && g_unichar_validate(u)) {
                    g_string_append_unichar(out, u);
                    done = TRUE;
                }
            } else {
                for (guint k = 0; k < G_N_ELEMENTS(ENT); k++) {
                    if (strlen(ENT[k].n) == nl && strncmp(ENT[k].n, nm, nl) == 0) {
                        g_string_append(out, ENT[k].v);
                        done = TRUE;
                        break;
                    }
                }
            }
        }
        if (done) {
            i = j + 1;
        } else {
            g_string_append_c(out, '&');
            i++;
        }
    }
}

static char *attr_get(const char *raw, const char *name)
{
    if (!raw)
        return NULL;
    gsize nl = strlen(name);
    const char *p = raw;
    while (*p) {
        const char *start = p;
        while (*p && (g_ascii_isspace(*p) || *p == '/'))
            p++;
        const char *ns = p;
        while (*p && !g_ascii_isspace(*p) && *p != '=' && *p != '/')
            p++;
        gsize len = (gsize)(p - ns);
        const char *q = p;
        while (g_ascii_isspace(*q))
            q++;
        char *val = NULL;
        if (len > 0 && *q == '=') {
            q++;
            while (g_ascii_isspace(*q))
                q++;
            GString *g = g_string_new(NULL);
            if (*q == '"' || *q == '\'') {
                char qc = *q++;
                const char *vs = q;
                while (*q && *q != qc)
                    q++;
                decode_entities(g, vs, (gsize)(q - vs));
                if (*q)
                    q++;
            } else {
                const char *vs = q;
                while (*q && !g_ascii_isspace(*q))
                    q++;
                decode_entities(g, vs, (gsize)(q - vs));
            }
            val = g_string_free(g, FALSE);
            p = q;
        } else if (len > 0) {
            val = g_strdup("");
        }
        if (val && len == nl && g_ascii_strncasecmp(ns, name, nl) == 0)
            return val;
        g_free(val);
        if (p == start)
            p++;
    }
    return NULL;
}

static guint64 fnv(const char *s, gsize n)
{
    guint64 h = 1469598103934665603ULL;
    for (gsize i = 0; i < n; i++) {
        h ^= (guchar)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static char *hex64(guint64 h)
{
    return g_strdup_printf("%016llx", (unsigned long long)h);
}

static char *key_of(const char *url)
{
    char *k = g_strdup(url);
    char *h = strchr(k, '#');
    if (h)
        *h = 0;
    return k;
}

static char *delta_dir(void)
{
    return g_build_filename(g_get_user_data_dir(), "goontern", "delta", NULL);
}

static char *delta_path(const char *key)
{
    char *d = delta_dir();
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
    char *p = g_build_filename(d, sum, NULL);
    g_free(d);
    g_free(sum);
    return p;
}

static GHashTable *delta_read(const char *key, gint64 *when)
{
    char *p = delta_path(key);
    char *data = NULL;
    gsize len = 0;
    gboolean ok = g_file_get_contents(p, &data, &len, NULL);
    g_free(p);
    if (!ok)
        return NULL;
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char **lines = g_strsplit(data, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (lines[i][0] == 't' && lines[i][1] == ' ')
            *when = g_ascii_strtoll(lines[i] + 2, NULL, 10);
        else if (strlen(lines[i]) == 16)
            g_hash_table_add(set, g_strdup(lines[i]));
    }
    g_strfreev(lines);
    g_free(data);
    return set;
}

static void delta_prune(void)
{
    static gboolean done;
    if (done)
        return;
    done = TRUE;
    char *d = delta_dir();
    GDir *dir = g_dir_open(d, 0, NULL);
    if (dir) {
        const char *n;
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        while ((n = g_dir_read_name(dir))) {
            char *p = g_build_filename(d, n, NULL);
            GStatBuf st;
            if (g_stat(p, &st) == 0 && now - (gint64)st.st_mtime > 90 * 24 * 3600)
                g_remove(p);
            g_free(p);
        }
        g_dir_close(dir);
    }
    g_free(d);
}

static void delta_write(const char *key, GArray *blocks)
{
    delta_prune();
    char *d = delta_dir();
    g_mkdir_with_parents(d, 0700);
    g_free(d);
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "t %lld\n", (long long)(g_get_real_time() / G_USEC_PER_SEC));
    for (guint i = 0; i < blocks->len; i++) {
        char *h = hex64(g_array_index(blocks, BlockRec, i).hash);
        g_string_append(s, h);
        g_string_append_c(s, '\n');
        g_free(h);
    }
    char *p = delta_path(key);
    g_file_set_contents(p, s->str, (gssize)s->len, NULL);
    g_free(p);
    g_string_free(s, TRUE);
}

void lite_delta_clear_all(void)
{
    char *d = delta_dir();
    GDir *dir = g_dir_open(d, 0, NULL);
    if (dir) {
        const char *n;
        while ((n = g_dir_read_name(dir))) {
            char *p = g_build_filename(d, n, NULL);
            g_remove(p);
            g_free(p);
        }
        g_dir_close(dir);
    }
    g_free(d);
}

void lite_set_options(gboolean delta, gboolean auto_images)
{
    opt_delta = delta;
    opt_images = auto_images;
}

static void set_scaled(GtkTextTag *t, double bs, double zoom)
{
    g_object_set_data(G_OBJECT(t), "bs", GINT_TO_POINTER((int)(bs * 1000)));
    g_object_set(t, "scale", bs * zoom, NULL);
}

static Doc *doc_new(LiteView *lv)
{
    Doc *d = g_new0(Doc, 1);
    GdkRGBA c;
    d->buf = gtk_text_buffer_new(NULL);
    d->paras = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    d->links = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    d->scaled = g_ptr_array_new();
    d->changes = g_ptr_array_new();
    d->fs = g_ptr_array_new();
    d->fe = g_ptr_array_new();
    d->fidx = -1;

    d->body = gtk_text_buffer_create_tag(d->buf, "body", NULL);
    set_scaled(d->body, 1.08, lv->zoom);

    d->inl[0] = gtk_text_buffer_create_tag(d->buf, "f-b", "weight", PANGO_WEIGHT_BOLD, NULL);
    d->inl[1] = gtk_text_buffer_create_tag(d->buf, "f-i", "style", PANGO_STYLE_ITALIC, NULL);
    rgba(&c, "rgba(255,255,255,0.09)");
    d->inl[2] = gtk_text_buffer_create_tag(d->buf, "f-code", "family", "monospace", "background-rgba", &c, NULL);
    set_scaled(d->inl[2], 0.92, lv->zoom);
    d->inl[3] = gtk_text_buffer_create_tag(d->buf, "f-u", "underline", PANGO_UNDERLINE_SINGLE, NULL);
    d->inl[4] = gtk_text_buffer_create_tag(d->buf, "f-s", "strikethrough", TRUE, NULL);
    rgba(&c, "rgb(235,203,139)");
    GdkRGBA dark;
    rgba(&dark, "rgb(46,52,64)");
    d->inl[5] = gtk_text_buffer_create_tag(d->buf, "f-mark", "background-rgba", &c, "foreground-rgba", &dark, NULL);
    d->inl[6] = gtk_text_buffer_create_tag(d->buf, "f-sup", "rise", 5000, NULL);
    set_scaled(d->inl[6], 0.75, lv->zoom);
    d->inl[7] = gtk_text_buffer_create_tag(d->buf, "f-sub", "rise", -2500, NULL);
    set_scaled(d->inl[7], 0.75, lv->zoom);
    d->inl[8] = gtk_text_buffer_create_tag(d->buf, "f-small", NULL);
    set_scaled(d->inl[8], 0.85, lv->zoom);
    rgba(&c, "rgb(123,134,154)");
    d->inl[9] = gtk_text_buffer_create_tag(d->buf, "f-dim", "foreground-rgba", &c, NULL);

    for (int i = 0; i < NFLAGS; i++)
        if (g_object_get_data(G_OBJECT(d->inl[i]), "bs"))
            g_ptr_array_add(d->scaled, d->inl[i]);
    g_ptr_array_add(d->scaled, d->body);

    d->spacer = gtk_text_buffer_create_tag(d->buf, "spacer", "scale", 0.45, "pixels-below-lines", 0, "pixels-above-lines", 0, NULL);
    rgba(&c, "rgba(136,192,208,0.15)");
    d->delta = gtk_text_buffer_create_tag(d->buf, "delta", "paragraph-background-rgba", &c, NULL);
    rgba(&c, "rgba(235,203,139,0.35)");
    d->fall = gtk_text_buffer_create_tag(d->buf, "find-all", "background-rgba", &c, NULL);
    rgba(&c, "rgb(235,203,139)");
    d->fcur = gtk_text_buffer_create_tag(d->buf, "find-cur", "background-rgba", &c, "foreground-rgba", &dark, NULL);
    return d;
}

static void doc_free(Doc *d)
{
    if (!d)
        return;
    g_object_unref(d->buf);
    g_hash_table_destroy(d->paras);
    g_hash_table_destroy(d->links);
    g_ptr_array_free(d->scaled, TRUE);
    g_ptr_array_free(d->changes, TRUE);
    g_ptr_array_free(d->fs, TRUE);
    g_ptr_array_free(d->fe, TRUE);
    g_free(d->fquery);
    g_free(d);
}

static void apply_zoom_tags(Doc *d, double zoom)
{
    for (guint i = 0; i < d->scaled->len; i++) {
        GtkTextTag *t = g_ptr_array_index(d->scaled, i);
        double bs = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "bs")) / 1000.0;
        g_object_set(t, "scale", bs * zoom, NULL);
    }
}

static GtkTextTag *para_tag(Parser *P, int kind, int units, int quote, gboolean hang)
{
    Doc *d = P->doc;
    char *key = g_strdup_printf("p%d_%d_%d_%d", kind, units, quote, hang);
    GtkTextTag *t = g_hash_table_lookup(d->paras, key);
    if (t) {
        g_free(key);
        return t;
    }
    t = gtk_text_buffer_create_tag(d->buf, key, NULL);
    int left = 6 + units * 24 + quote * 18;
    int above = 0;
    int below = 9;
    if (hang) {
        left += 20;
        g_object_set(t, "indent", -18, NULL);
        below = 3;
    }
    if (kind >= 1 && kind <= 6) {
        static const double hs[] = { 0, 1.9, 1.55, 1.3, 1.15, 1.05, 1.0 };
        g_object_set(t, "weight", PANGO_WEIGHT_BOLD, NULL);
        set_scaled(t, hs[kind], P->lv->zoom);
        g_ptr_array_add(d->scaled, t);
        above = kind <= 2 ? 18 : 12;
        below = 6;
    } else if (kind == 7) {
        GdkRGBA c;
        rgba(&c, "rgba(255,255,255,0.07)");
        left += 8;
        g_object_set(t, "family", "monospace", "paragraph-background-rgba", &c, "right-margin", 8, NULL);
        set_scaled(t, 0.92, P->lv->zoom);
        g_ptr_array_add(d->scaled, t);
        above = 0;
        below = 0;
    }
    g_object_set(t, "left-margin", left, "pixels-above-lines", above, "pixels-below-lines", below, NULL);
    g_hash_table_insert(d->paras, key, t);
    return t;
}

static char *detrack(char *url)
{
    if (!url)
        return NULL;
    if (strstr(url, "duckduckgo.com/l/") && strstr(url, "uddg=")) {
        const char *s = strstr(url, "uddg=") + 5;
        const char *e = strchr(s, '&');
        char *enc = e ? g_strndup(s, (gsize)(e - s)) : g_strdup(s);
        char *dec = g_uri_unescape_string(enc, NULL);
        g_free(enc);
        if (dec && (g_str_has_prefix(dec, "http://") || g_str_has_prefix(dec, "https://"))) {
            g_free(url);
            return dec;
        }
        g_free(dec);
    }
    return url;
}

static char *resolve(const char *base, const char *href)
{
    char *h = g_strstrip(g_strdup(href));
    if (!*h) {
        g_free(h);
        return NULL;
    }
    GError *e = NULL;
    char *r = g_uri_resolve_relative(base, h, G_URI_FLAGS_PARSE_RELAXED, &e);
    g_free(h);
    if (!r) {
        g_clear_error(&e);
        return NULL;
    }
    if (!g_str_has_prefix(r, "http://") && !g_str_has_prefix(r, "https://")) {
        g_free(r);
        return NULL;
    }
    return detrack(r);
}

static GtkTextTag *link_tag(Parser *P, const char *href, const char *kind)
{
    char *abs = resolve(P->base, href);
    if (!abs)
        return NULL;
    char *k = g_strconcat(kind, abs, NULL);
    GtkTextTag *t = g_hash_table_lookup(P->doc->links, k);
    if (t) {
        g_free(k);
        g_free(abs);
        return t;
    }
    GdkRGBA c;
    rgba(&c, "rgb(136,192,208)");
    if (strcmp(kind, "href") == 0)
        t = gtk_text_buffer_create_tag(P->doc->buf, NULL, "foreground-rgba", &c, "underline", PANGO_UNDERLINE_SINGLE, NULL);
    else
        t = gtk_text_buffer_create_tag(P->doc->buf, NULL, "foreground-rgba", &c, "style", PANGO_STYLE_ITALIC, NULL);
    g_object_set_data_full(G_OBJECT(t), kind, abs, g_free);
    g_hash_table_insert(P->doc->links, k, t);
    return t;
}

static void run_append(Parser *P, const char *s, gsize len)
{
    if (!len)
        return;
    Run *last = P->runs->len ? &g_array_index(P->runs, Run, P->runs->len - 1) : NULL;
    if (last && last->flags == P->flags && last->link == P->link) {
        g_string_append_len(last->text, s, (gssize)len);
        return;
    }
    Run r;
    r.text = g_string_new_len(s, (gssize)len);
    r.flags = P->flags;
    r.link = P->link;
    g_array_append_val(P->runs, r);
}

static void clear_runs(Parser *P)
{
    for (guint i = 0; i < P->runs->len; i++)
        g_string_free(g_array_index(P->runs, Run, i).text, TRUE);
    g_array_set_size(P->runs, 0);
}

static void add_text(Parser *P, const char *s, gsize len)
{
    if (P->skip_name || len == 0)
        return;
    if (P->in_head && !P->in_title)
        return;
    GString *dec = g_string_sized_new(len);
    decode_entities(dec, s, len);
    GString *o = g_string_sized_new(dec->len);
    if (P->pre && !P->in_title) {
        gsize st = 0;
        if (P->pre_first) {
            P->pre_first = FALSE;
            if (dec->len && dec->str[0] == '\n')
                st = 1;
        }
        for (gsize i = st; i < dec->len; i++)
            if (dec->str[i] != '\r')
                g_string_append_c(o, dec->str[i]);
        run_append(P, o->str, o->len);
        P->last_space = FALSE;
    } else if (P->in_title) {
        for (gsize i = 0; i < dec->len; i++) {
            char c = dec->str[i];
            if (g_ascii_isspace(c)) {
                if (P->title->len && P->title->str[P->title->len - 1] != ' ')
                    g_string_append_c(P->title, ' ');
            } else {
                g_string_append_c(P->title, c);
            }
        }
    } else {
        for (gsize i = 0; i < dec->len; i++) {
            char c = dec->str[i];
            if (g_ascii_isspace(c)) {
                if (!P->last_space) {
                    g_string_append_c(o, ' ');
                    P->last_space = TRUE;
                }
            } else {
                g_string_append_c(o, c);
                P->last_space = FALSE;
            }
        }
        run_append(P, o->str, o->len);
    }
    g_string_free(o, TRUE);
    g_string_free(dec, TRUE);
}

static void put(Doc *d, const char *text, gsize len, guint flags, GtkTextTag *link)
{
    GtkTextIter e, a;
    gtk_text_buffer_get_end_iter(d->buf, &e);
    int s = gtk_text_iter_get_offset(&e);
    gtk_text_buffer_insert(d->buf, &e, text, (gint)len);
    gtk_text_buffer_get_iter_at_offset(d->buf, &a, s);
    gtk_text_buffer_get_end_iter(d->buf, &e);
    for (int i = 0; i < NFLAGS; i++)
        if (flags & (1u << i))
            gtk_text_buffer_apply_tag(d->buf, d->inl[i], &a, &e);
    if (link)
        gtk_text_buffer_apply_tag(d->buf, link, &a, &e);
}

static void add_spacer(Doc *d)
{
    GtkTextIter e, a;
    gtk_text_buffer_get_end_iter(d->buf, &e);
    int s = gtk_text_iter_get_offset(&e);
    gtk_text_buffer_insert(d->buf, &e, "\n", 1);
    gtk_text_buffer_get_iter_at_offset(d->buf, &a, s);
    gtk_text_buffer_get_end_iter(d->buf, &e);
    gtk_text_buffer_apply_tag(d->buf, d->spacer, &a, &e);
}

static void flush_block(Parser *P)
{
    Doc *d = P->doc;
    gboolean has = FALSE;
    for (guint i = 0; i < P->runs->len && !has; i++) {
        Run *r = &g_array_index(P->runs, Run, i);
        for (gsize k = 0; k < r->text->len; k++) {
            if (!g_ascii_isspace(r->text->str[k])) {
                has = TRUE;
                break;
            }
        }
    }
    if (!has) {
        clear_runs(P);
        P->last_space = TRUE;
        g_clear_pointer(&P->marker, g_free);
        return;
    }
    for (int i = (int)P->runs->len - 1; i >= 0; i--) {
        Run *r = &g_array_index(P->runs, Run, i);
        while (r->text->len && (r->text->str[r->text->len - 1] == '\n' ||
                                (!P->pre && r->text->str[r->text->len - 1] == ' ')))
            g_string_truncate(r->text, r->text->len - 1);
        if (r->text->len)
            break;
    }

    gboolean is_pre = P->heading == 0 && P->pre > 0;
    if (is_pre)
        add_spacer(d);
    GtkTextIter it;
    gtk_text_buffer_get_end_iter(d->buf, &it);
    int bstart = gtk_text_iter_get_offset(&it);
    for (guint i = 0; i < P->pending_ids->len; i++) {
        char *mn = g_strconcat("id:", (char *)g_ptr_array_index(P->pending_ids, i), NULL);
        if (!gtk_text_buffer_get_mark(d->buf, mn))
            gtk_text_buffer_create_mark(d->buf, mn, &it, TRUE);
        g_free(mn);
    }
    g_ptr_array_set_size(P->pending_ids, 0);

    int kind = 0;
    gboolean hang = FALSE;
    if (P->heading > 0)
        kind = P->heading;
    else if (P->pre > 0)
        kind = 7;
    else if (P->marker)
        hang = TRUE;
    int ldepth = (int)P->lists->len;
    int units = ldepth + (P->dd ? 1 : 0);
    if (hang && units > 0)
        units--;

    if (P->quote > 0)
        put(d, "\xe2\x96\x8e ", 4, F_DIM, NULL);
    if (P->marker) {
        put(d, P->marker, strlen(P->marker), F_DIM, NULL);
        g_clear_pointer(&P->marker, g_free);
    }

    GString *plain = g_string_new(NULL);
    for (guint i = 0; i < P->runs->len; i++) {
        Run *r = &g_array_index(P->runs, Run, i);
        if (!r->text->len)
            continue;
        put(d, r->text->str, r->text->len, r->flags, r->link);
        g_string_append_len(plain, r->text->str, (gssize)r->text->len);
        if (r->link && g_object_get_data(G_OBJECT(r->link), "img")) {
            GtkTextIter e;
            gtk_text_buffer_get_end_iter(d->buf, &e);
            ImgRef ref;
            ref.tag = r->link;
            ref.mark = gtk_text_buffer_create_mark(d->buf, NULL, &e, TRUE);
            g_array_append_val(P->imgs, ref);
        }
    }
    GtkTextIter e;
    gtk_text_buffer_get_end_iter(d->buf, &e);
    gtk_text_buffer_insert(d->buf, &e, "\n", 1);
    gtk_text_buffer_get_end_iter(d->buf, &e);
    GtkTextIter s;
    gtk_text_buffer_get_iter_at_offset(d->buf, &s, bstart);
    gtk_text_buffer_apply_tag(d->buf, para_tag(P, kind, units, P->quote, hang), &s, &e);
    if (is_pre)
        add_spacer(d);

    gsize plen = plain->len;
    if ((plen >= 12 || (kind >= 1 && kind <= 6 && plen >= 3)) && P->blocks->len < MAX_BLOCKS) {
        BlockRec br;
        br.hash = fnv(plain->str, plen);
        br.start = bstart;
        g_array_append_val(P->blocks, br);
    }
    g_string_free(plain, TRUE);
    clear_runs(P);
    P->last_space = TRUE;
}

static void flag_inc(Parser *P, int bit)
{
    P->flagcnt[bit]++;
    P->flags |= (1u << bit);
}

static void flag_dec(Parser *P, int bit)
{
    if (P->flagcnt[bit] > 0)
        P->flagcnt[bit]--;
    if (P->flagcnt[bit] == 0)
        P->flags &= ~(1u << bit);
}

static int flag_of(const char *n)
{
    if (!strcmp(n, "b") || !strcmp(n, "strong") || !strcmp(n, "th") || !strcmp(n, "dt"))
        return 0;
    if (!strcmp(n, "i") || !strcmp(n, "em") || !strcmp(n, "cite") || !strcmp(n, "var") || !strcmp(n, "dfn"))
        return 1;
    if (!strcmp(n, "code") || !strcmp(n, "kbd") || !strcmp(n, "samp") || !strcmp(n, "tt"))
        return 2;
    if (!strcmp(n, "u") || !strcmp(n, "ins"))
        return 3;
    if (!strcmp(n, "s") || !strcmp(n, "strike") || !strcmp(n, "del"))
        return 4;
    if (!strcmp(n, "mark"))
        return 5;
    if (!strcmp(n, "sup"))
        return 6;
    if (!strcmp(n, "sub"))
        return 7;
    if (!strcmp(n, "small"))
        return 8;
    return -1;
}

static void add_dim(Parser *P, const char *s)
{
    guint keep = P->flags;
    GtkTextTag *kl = P->link;
    P->flags = F_DIM;
    P->link = NULL;
    run_append(P, s, strlen(s));
    P->flags = keep;
    P->link = kl;
    P->last_space = FALSE;
}

static gboolean looks_hidden(const char *raw)
{
    char *h = attr_get(raw, "hidden");
    if (h) {
        g_free(h);
        return TRUE;
    }
    char *st = attr_get(raw, "style");
    gboolean hid = FALSE;
    if (st) {
        GString *n = g_string_new(NULL);
        for (const char *c = st; *c; c++)
            if (!g_ascii_isspace(*c))
                g_string_append_c(n, g_ascii_tolower(*c));
        hid = strstr(n->str, "display:none") != NULL;
        g_string_free(n, TRUE);
        g_free(st);
    }
    return hid;
}

static void handle_img(Parser *P, const char *raw)
{
    if (P->nimg >= 400)
        return;
    char *src = attr_get(raw, "src");
    if (!src || !*src || g_str_has_prefix(src, "data:")) {
        g_free(src);
        src = attr_get(raw, "data-src");
    }
    if (!src || !*src || g_str_has_prefix(src, "data:")) {
        g_free(src);
        return;
    }
    char *w = attr_get(raw, "width");
    char *h = attr_get(raw, "height");
    gboolean tiny = (w && *w && atoi(w) > 0 && atoi(w) <= 2) || (h && *h && atoi(h) > 0 && atoi(h) <= 2);
    g_free(w);
    g_free(h);
    if (tiny) {
        g_free(src);
        return;
    }
    char *alt = attr_get(raw, "alt");
    GString *label = g_string_new("\xe2\x96\xa3 ");
    if (alt && *alt) {
        char *t = g_strstrip(alt);
        if (g_utf8_strlen(t, -1) > 80) {
            char *end = g_utf8_offset_to_pointer(t, 80);
            g_string_append_len(label, t, end - t);
            g_string_append(label, "\xe2\x80\xa6");
        } else {
            g_string_append(label, t);
        }
    } else {
        g_string_append(label, "Bild");
    }
    g_string_append_c(label, ' ');
    GtkTextTag *keep = P->link;
    if (!keep)
        P->link = link_tag(P, src, "img");
    run_append(P, label->str, label->len);
    P->link = keep;
    P->nimg++;
    P->last_space = TRUE;
    g_string_free(label, TRUE);
    g_free(alt);
    g_free(src);
}

static void on_start(Parser *P, const char *name, const char *raw, gboolean selfclose)
{
    gboolean isvoid = in_list(name, VOID_TAGS);
    if (P->skip_name) {
        if (!isvoid && !selfclose && strcmp(name, P->skip_name) == 0)
            P->skip_depth++;
        return;
    }
    if (in_list(name, SKIP_TAGS)) {
        if (!selfclose) {
            P->skip_name = g_strdup(name);
            P->skip_depth = 1;
        }
        return;
    }
    if (!isvoid && !selfclose && looks_hidden(raw)) {
        P->skip_name = g_strdup(name);
        P->skip_depth = 1;
        return;
    }
    char *id = attr_get(raw, "id");
    if (id && *id && strlen(id) < 128)
        g_ptr_array_add(P->pending_ids, id);
    else
        g_free(id);

    if (!strcmp(name, "head")) {
        P->in_head = TRUE;
    } else if (!strcmp(name, "body")) {
        P->in_head = FALSE;
        flush_block(P);
    } else if (!strcmp(name, "title")) {
        if (!P->got_title)
            P->in_title = TRUE;
    } else if (!strcmp(name, "base")) {
        char *href = attr_get(raw, "href");
        if (href) {
            char *b = resolve(P->base, href);
            if (b) {
                g_free(P->base);
                P->base = b;
            }
            g_free(href);
        }
    } else if (!strcmp(name, "br")) {
        run_append(P, "\n", 1);
        P->last_space = TRUE;
    } else if (!strcmp(name, "hr")) {
        flush_block(P);
        add_dim(P, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80");
        flush_block(P);
    } else if (!strcmp(name, "img")) {
        handle_img(P, raw);
    } else if (!strcmp(name, "a")) {
        char *href = attr_get(raw, "href");
        P->link = href ? link_tag(P, href, "href") : NULL;
        g_free(href);
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) {
        flush_block(P);
        P->heading = name[1] - '0';
    } else if (!strcmp(name, "ul") || !strcmp(name, "menu")) {
        flush_block(P);
        int v = -1;
        g_array_append_val(P->lists, v);
    } else if (!strcmp(name, "ol")) {
        flush_block(P);
        char *st = attr_get(raw, "start");
        int v = st && atoi(st) > 0 ? atoi(st) : 1;
        g_free(st);
        g_array_append_val(P->lists, v);
    } else if (!strcmp(name, "li")) {
        flush_block(P);
        g_clear_pointer(&P->marker, g_free);
        if (P->lists->len == 0) {
            P->marker = g_strdup("\xe2\x80\xa2 ");
        } else {
            int *top = &g_array_index(P->lists, int, P->lists->len - 1);
            if (*top < 0) {
                P->marker = g_strdup(P->lists->len > 1 ? "\xe2\x97\xa6 " : "\xe2\x80\xa2 ");
            } else {
                P->marker = g_strdup_printf("%d. ", *top);
                (*top)++;
            }
        }
    } else if (!strcmp(name, "dd")) {
        flush_block(P);
        P->dd = 1;
    } else if (!strcmp(name, "dt")) {
        flush_block(P);
        flag_inc(P, 0);
    } else if (!strcmp(name, "blockquote")) {
        flush_block(P);
        P->quote++;
    } else if (!strcmp(name, "pre")) {
        flush_block(P);
        P->pre++;
        P->pre_first = TRUE;
    } else if (!strcmp(name, "tr")) {
        flush_block(P);
        P->row_has_cell = FALSE;
    } else if (!strcmp(name, "td") || !strcmp(name, "th")) {
        if (P->row_has_cell)
            add_dim(P, "  \xe2\x94\x82  ");
        P->row_has_cell = TRUE;
        if (!strcmp(name, "th"))
            flag_inc(P, 0);
    } else if (!strcmp(name, "q")) {
        run_append(P, "\xe2\x80\x9e", 3);
    } else if (in_list(name, BLOCK_TAGS)) {
        flush_block(P);
    } else {
        int f = flag_of(name);
        if (f >= 0)
            flag_inc(P, f);
    }
}

static void on_end(Parser *P, const char *name)
{
    if (P->skip_name) {
        if (strcmp(name, P->skip_name) == 0 && --P->skip_depth <= 0)
            g_clear_pointer(&P->skip_name, g_free);
        return;
    }
    if (!strcmp(name, "title")) {
        P->in_title = FALSE;
        P->got_title = TRUE;
    } else if (!strcmp(name, "head")) {
        P->in_head = FALSE;
    } else if (!strcmp(name, "a")) {
        P->link = NULL;
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) {
        flush_block(P);
        P->heading = 0;
    } else if (!strcmp(name, "ul") || !strcmp(name, "ol") || !strcmp(name, "menu")) {
        flush_block(P);
        if (P->lists->len)
            g_array_set_size(P->lists, P->lists->len - 1);
        if (P->lists->len == 0)
            add_spacer(P->doc);
    } else if (!strcmp(name, "li") || !strcmp(name, "tr")) {
        flush_block(P);
    } else if (!strcmp(name, "dd")) {
        flush_block(P);
        P->dd = 0;
    } else if (!strcmp(name, "dt")) {
        flush_block(P);
        flag_dec(P, 0);
    } else if (!strcmp(name, "blockquote")) {
        flush_block(P);
        if (P->quote > 0)
            P->quote--;
        if (P->quote == 0)
            add_spacer(P->doc);
    } else if (!strcmp(name, "pre")) {
        flush_block(P);
        if (P->pre > 0)
            P->pre--;
    } else if (!strcmp(name, "th")) {
        flag_dec(P, 0);
    } else if (!strcmp(name, "q")) {
        run_append(P, "\xe2\x80\x9c", 3);
    } else if (in_list(name, BLOCK_TAGS)) {
        flush_block(P);
    } else {
        int f = flag_of(name);
        if (f >= 0)
            flag_dec(P, f);
    }
}

static const char *find_ci(const char *p, const char *end, const char *needle)
{
    gsize nl = strlen(needle);
    for (; p + nl <= end; p++)
        if (g_ascii_strncasecmp(p, needle, nl) == 0)
            return p;
    return NULL;
}

static void parse_html(Parser *P, const char *html, gsize len)
{
    const char *p = html;
    const char *end = html + len;
    while (p < end) {
        if (*p != '<') {
            const char *q = memchr(p, '<', (gsize)(end - p));
            if (!q)
                q = end;
            add_text(P, p, (gsize)(q - p));
            p = q;
            continue;
        }
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            const char *q = find_ci(p + 4, end, "-->");
            p = q ? q + 3 : end;
            continue;
        }
        if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
            const char *q = memchr(p, '>', (gsize)(end - p));
            p = q ? q + 1 : end;
            continue;
        }
        if (p + 1 < end && p[1] == '/') {
            const char *q = p + 2;
            const char *ns = q;
            while (q < end && !g_ascii_isspace(*q) && *q != '>' && *q != '/')
                q++;
            char *nm = g_ascii_strdown(ns, (gssize)(q - ns));
            const char *gt = memchr(q, '>', (gsize)(end - q));
            p = gt ? gt + 1 : end;
            if (*nm)
                on_end(P, nm);
            g_free(nm);
            continue;
        }
        if (p + 1 < end && g_ascii_isalpha(p[1])) {
            const char *q = p + 1;
            const char *ns = q;
            while (q < end && !g_ascii_isspace(*q) && *q != '>' && *q != '/')
                q++;
            char *nm = g_ascii_strdown(ns, (gssize)(q - ns));
            const char *as = q;
            char quote = 0;
            while (q < end) {
                if (quote) {
                    if (*q == quote)
                        quote = 0;
                } else if (*q == '"' || *q == '\'') {
                    quote = *q;
                } else if (*q == '>') {
                    break;
                }
                q++;
            }
            char *raw = g_strndup(as, (gsize)(q - as));
            gboolean self = strlen(raw) > 0 && raw[strlen(raw) - 1] == '/';
            p = q < end ? q + 1 : end;
            if (!strcmp(nm, "script") || !strcmp(nm, "style")) {
                char *needle = g_strconcat("</", nm, NULL);
                const char *e = find_ci(p, end, needle);
                g_free(needle);
                if (e) {
                    const char *gt = memchr(e, '>', (gsize)(end - e));
                    p = gt ? gt + 1 : end;
                } else {
                    p = end;
                }
            } else {
                on_start(P, nm, raw, self);
            }
            g_free(nm);
            g_free(raw);
            continue;
        }
        add_text(P, "<", 1);
        p++;
    }
    flush_block(P);
}

static void bar_set(LiteView *lv, const char *text)
{
    g_free(lv->barbase);
    lv->barbase = g_strdup(text);
    if (text)
        gtk_label_set_text(GTK_LABEL(lv->barlabel), text);
    gtk_widget_set_visible(lv->bar, text != NULL);
}

static void bar_refresh(LiteView *lv)
{
    if (!lv->barbase)
        return;
    Doc *d = lv->doc;
    if (d && d->changes->len && lv->cidx >= 0) {
        char *t = g_strdup_printf("%s  \xc2\xb7  %d/%u", lv->barbase, lv->cidx + 1, d->changes->len);
        gtk_label_set_text(GTK_LABEL(lv->barlabel), t);
        g_free(t);
    } else {
        gtk_label_set_text(GTK_LABEL(lv->barlabel), lv->barbase);
    }
}

static void delta_apply(LiteView *lv, Doc *d, Parser *P, const char *url)
{
    if (!opt_delta || P->blocks->len == 0) {
        bar_set(lv, NULL);
        return;
    }
    char *key = key_of(url);
    if (lv->baseline)
        g_hash_table_destroy(lv->baseline);
    lv->baseline_time = 0;
    lv->baseline = delta_read(key, &lv->baseline_time);
    g_free(lv->baseline_url);
    lv->baseline_url = g_strdup(key);
    guint nchanged = 0;
    if (lv->baseline) {
        for (guint i = 0; i < P->blocks->len; i++) {
            BlockRec *b = &g_array_index(P->blocks, BlockRec, i);
            char *hx = hex64(b->hash);
            gboolean known = g_hash_table_contains(lv->baseline, hx);
            g_free(hx);
            if (known)
                continue;
            int stop = (i + 1 < P->blocks->len) ? g_array_index(P->blocks, BlockRec, i + 1).start : -1;
            GtkTextIter s, e;
            gtk_text_buffer_get_iter_at_offset(d->buf, &s, b->start);
            if (stop >= 0)
                gtk_text_buffer_get_iter_at_offset(d->buf, &e, stop);
            else
                gtk_text_buffer_get_end_iter(d->buf, &e);
            gtk_text_buffer_apply_tag(d->buf, d->delta, &s, &e);
            g_ptr_array_add(d->changes, gtk_text_buffer_create_mark(d->buf, NULL, &s, TRUE));
            nchanged++;
        }
    }
    delta_write(key, P->blocks);
    g_free(key);
    gtk_text_tag_set_priority(d->delta, gtk_text_tag_table_get_size(gtk_text_buffer_get_tag_table(d->buf)) - 1);

    char *msg;
    if (!lv->baseline) {
        msg = g_strdup("Erster Besuch dieser Seite \xc2\xb7 beim n\xc3\xa4" "chsten Mal markiert Goontern hier, was sich ge\xc3\xa4ndert hat");
    } else {
        GDateTime *dt = g_date_time_new_from_unix_local(lv->baseline_time);
        char *when = g_date_time_format(dt, "%d.%m. %H:%M");
        g_date_time_unref(dt);
        if (nchanged == 0)
            msg = g_strdup_printf("Keine \xc3\x84nderungen seit %s", when);
        else
            msg = g_strdup_printf("\xce\x94 %u Abschnitte neu oder ge\xc3\xa4ndert seit %s  \xc2\xb7  Alt+\xe2\x86\x93 n\xc3\xa4" "chste \xc3\x84nderung", nchanged, when);
        g_free(when);
    }
    bar_set(lv, msg);
    g_free(msg);
}

static void fetch_image(LiteView *lv, GtkTextTag *tag, GtkTextMark *mark);

static gboolean restore_tick(GtkWidget *w, GdkFrameClock *fc, gpointer data)
{
    Restore *r = data;
    GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(r->lv->scroll));
    double upper = gtk_adjustment_get_upper(a);
    double page = gtk_adjustment_get_page_size(a);
    r->frames++;
    if (upper - page >= r->target - 1 || r->frames > 45) {
        gtk_adjustment_set_value(a, MAX(0, MIN(r->target, upper - page)));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void show_doc(LiteView *lv, Doc *d, const char *frag, gboolean restore, double scroll)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(d->buf, &s, &e);
    gtk_text_buffer_apply_tag(d->buf, d->body, &s, &e);
    Doc *old = lv->doc;
    lv->doc = d;
    lv->cidx = -1;
    gtk_text_view_set_buffer(GTK_TEXT_VIEW(lv->tv), d->buf);
    doc_free(old);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(lv->scroll)), 0);
    if (frag && *frag) {
        char *mn = g_strconcat("id:", frag, NULL);
        GtkTextMark *m = gtk_text_buffer_get_mark(d->buf, mn);
        g_free(mn);
        if (m)
            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(lv->tv), m, 0.0, TRUE, 0.0, 0.05);
    } else if (restore && scroll > 1) {
        Restore *r = g_new0(Restore, 1);
        r->lv = lv;
        r->target = scroll;
        gtk_widget_add_tick_callback(lv->tv, restore_tick, r, g_free);
    }
}

static void doc_message(LiteView *lv, const char *url, const char *head, const char *msg)
{
    char *eh = g_markup_escape_text(head, -1);
    char *em = g_markup_escape_text(msg, -1);
    char *eu = g_markup_escape_text(url ? url : "", -1);
    char *html = g_strdup_printf("<h1>%s</h1><p>%s</p><p>%s</p>", eh, em, eu);
    Doc *d = doc_new(lv);
    Parser P;
    memset(&P, 0, sizeof P);
    P.lv = lv;
    P.doc = d;
    P.runs = g_array_new(FALSE, FALSE, sizeof(Run));
    P.lists = g_array_new(FALSE, FALSE, sizeof(int));
    P.blocks = g_array_new(FALSE, FALSE, sizeof(BlockRec));
    P.imgs = g_array_new(FALSE, FALSE, sizeof(ImgRef));
    P.pending_ids = g_ptr_array_new_with_free_func(g_free);
    P.title = g_string_new(NULL);
    P.base = g_strdup("http://localhost/");
    P.last_space = TRUE;
    parse_html(&P, html, strlen(html));
    show_doc(lv, d, NULL, FALSE, 0);
    bar_set(lv, NULL);
    g_free(lv->title);
    lv->title = g_strdup(head);
    clear_runs(&P);
    g_array_free(P.runs, TRUE);
    g_array_free(P.lists, TRUE);
    g_array_free(P.blocks, TRUE);
    g_array_free(P.imgs, TRUE);
    g_ptr_array_free(P.pending_ids, TRUE);
    g_string_free(P.title, TRUE);
    g_free(P.base);
    g_free(eh);
    g_free(em);
    g_free(eu);
    g_free(html);
}

static void render_text(LiteView *lv, const char *url, const char *text, gsize len, gboolean plain,
                        const char *frag, gboolean restore, double scroll)
{
    Doc *d = doc_new(lv);
    Parser P;
    memset(&P, 0, sizeof P);
    P.lv = lv;
    P.doc = d;
    P.runs = g_array_new(FALSE, FALSE, sizeof(Run));
    P.lists = g_array_new(FALSE, FALSE, sizeof(int));
    P.blocks = g_array_new(FALSE, FALSE, sizeof(BlockRec));
    P.imgs = g_array_new(FALSE, FALSE, sizeof(ImgRef));
    P.pending_ids = g_ptr_array_new_with_free_func(g_free);
    P.title = g_string_new(NULL);
    P.base = key_of(url);
    P.last_space = TRUE;
    if (plain) {
        P.pre = 1;
        P.pre_first = FALSE;
        add_text(&P, text, len);
        flush_block(&P);
    } else {
        parse_html(&P, text, len);
    }
    delta_apply(lv, d, &P, url);
    char *title = NULL;
    if (P.title->len)
        title = g_strdup(g_strstrip(P.title->str));
    if (!title || !*title) {
        g_free(title);
        GUri *u = g_uri_parse(url, G_URI_FLAGS_PARSE_RELAXED, NULL);
        title = g_strdup(u && g_uri_get_host(u) ? g_uri_get_host(u) : url);
        if (u)
            g_uri_unref(u);
    }
    g_free(lv->title);
    lv->title = title;
    GArray *imgs = P.imgs;
    GPtrArray *auto_tags = g_ptr_array_new();
    GPtrArray *auto_marks = g_ptr_array_new();
    if (opt_images) {
        for (guint i = 0; i < imgs->len && i < MAX_AUTO_IMG; i++) {
            ImgRef *r = &g_array_index(imgs, ImgRef, i);
            g_ptr_array_add(auto_tags, r->tag);
            g_ptr_array_add(auto_marks, r->mark);
        }
    }
    show_doc(lv, d, frag, restore, scroll);
    for (guint i = 0; i < auto_tags->len; i++)
        fetch_image(lv, g_ptr_array_index(auto_tags, i), g_ptr_array_index(auto_marks, i));
    g_ptr_array_free(auto_tags, TRUE);
    g_ptr_array_free(auto_marks, TRUE);
    clear_runs(&P);
    g_array_free(P.runs, TRUE);
    g_array_free(P.lists, TRUE);
    g_array_free(P.blocks, TRUE);
    g_array_free(P.imgs, TRUE);
    g_ptr_array_free(P.pending_ids, TRUE);
    g_string_free(P.title, TRUE);
    g_free(P.base);
    g_free(P.marker);
    g_free(P.skip_name);
}

static SoupSession *get_session(void)
{
    if (!session) {
        session = soup_session_new();
        g_object_set(session, "user-agent", "Goontern-Lite/0.1", "timeout", 25u, "idle-timeout", 25u, NULL);
    }
    return session;
}

static char *to_utf8(const char *data, gsize len, const char *cs)
{
    if (cs && *cs && g_ascii_strcasecmp(cs, "utf-8") != 0 && g_ascii_strcasecmp(cs, "utf8") != 0) {
        char *r = g_convert(data, (gssize)len, "UTF-8", cs, NULL, NULL, NULL);
        if (r)
            return r;
    }
    if (g_utf8_validate(data, (gssize)len, NULL))
        return g_strndup(data, len);
    gsize probe = MIN(len, (gsize)2048);
    char *head = g_strndup(data, probe);
    char *found = NULL;
    const char *c = find_ci(head, head + probe, "charset=");
    if (c) {
        c += 8;
        while (*c == '"' || *c == '\'')
            c++;
        const char *e = c;
        while (*e && *e != '"' && *e != '\'' && *e != ';' && *e != ' ' && *e != '>' && *e != '/')
            e++;
        found = g_strndup(c, (gsize)(e - c));
    }
    g_free(head);
    if (found && *found) {
        char *r = g_convert(data, (gssize)len, "UTF-8", found, NULL, NULL, NULL);
        g_free(found);
        if (r)
            return r;
    } else {
        g_free(found);
    }
    char *r = g_convert(data, (gssize)len, "UTF-8", "CP1252", NULL, NULL, NULL);
    if (r)
        return r;
    return g_utf8_make_valid(data, (gssize)len);
}

static void job_free(Job *j)
{
    g_object_unref(j->cancel);
    g_object_unref(j->msg);
    g_free(j->url);
    g_free(j->frag);
    g_free(j);
}

static void render_image_bytes(LiteView *lv, const char *url, GBytes *bytes)
{
    GInputStream *st = g_memory_input_stream_new_from_bytes(bytes);
    GdkPixbuf *pb = gdk_pixbuf_new_from_stream(st, NULL, NULL);
    g_object_unref(st);
    if (!pb) {
        doc_message(lv, url, "Bild", "Das Bildformat wird nicht unterst\xc3\xbctzt.");
        return;
    }
    if (gdk_pixbuf_get_width(pb) > MAX_IMG_W * 2) {
        int w = MAX_IMG_W * 2;
        int h = gdk_pixbuf_get_height(pb) * w / gdk_pixbuf_get_width(pb);
        GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, w, MAX(h, 1), GDK_INTERP_BILINEAR);
        g_object_unref(pb);
        pb = sc;
    }
    GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
    g_object_unref(pb);
    Doc *d = doc_new(lv);
    GtkTextIter e;
    gtk_text_buffer_get_end_iter(d->buf, &e);
    gtk_text_buffer_insert_paintable(d->buf, &e, GDK_PAINTABLE(tex));
    g_object_unref(tex);
    show_doc(lv, d, NULL, FALSE, 0);
    bar_set(lv, NULL);
    g_free(lv->title);
    lv->title = g_strdup(url);
}

static void on_fetched(GObject *src, GAsyncResult *res, gpointer data)
{
    Job *j = data;
    GError *err = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (g_cancellable_is_cancelled(j->cancel)) {
        if (bytes)
            g_bytes_unref(bytes);
        g_clear_error(&err);
        job_free(j);
        return;
    }
    LiteView *lv = j->lv;
    lv->progress = 0.7;
    notify(lv);
    if (!bytes) {
        doc_message(lv, j->url, "Seite nicht geladen", err ? err->message : "Unbekannter Fehler");
        g_clear_error(&err);
    } else {
        GUri *fu = soup_message_get_uri(j->msg);
        char *final = fu ? g_uri_to_string(fu) : g_strdup(j->url);
        GHashTable *params = NULL;
        const char *ct = soup_message_headers_get_content_type(soup_message_get_response_headers(j->msg), &params);
        char *ctype = ct ? g_ascii_strdown(ct, -1) : NULL;
        const char *cs = params ? g_hash_table_lookup(params, "charset") : NULL;
        gsize len = 0;
        const char *raw = g_bytes_get_data(bytes, &len);
        guint status = soup_message_get_status(j->msg);
        g_free(lv->url);
        lv->url = g_strdup(final);
        if (j->push) {
            HistEntry *h = g_new0(HistEntry, 1);
            while ((int)lv->hist->len > lv->hpos + 1)
                g_ptr_array_remove_index(lv->hist, lv->hist->len - 1);
            h->url = g_strdup(final);
            g_ptr_array_add(lv->hist, h);
            lv->hpos = (int)lv->hist->len - 1;
        } else if (lv->hpos >= 0 && lv->hpos < (int)lv->hist->len) {
            HistEntry *h = g_ptr_array_index(lv->hist, lv->hpos);
            g_free(h->url);
            h->url = g_strdup(final);
        }
        if (ctype && g_str_has_prefix(ctype, "image/")) {
            render_image_bytes(lv, final, bytes);
        } else if (len == 0) {
            char *m = g_strdup_printf("Der Server hat keinen Inhalt geliefert (Status %u).", status);
            doc_message(lv, final, "Leere Antwort", m);
            g_free(m);
        } else if (!ctype || g_str_has_prefix(ctype, "text/html") || g_str_has_prefix(ctype, "application/xhtml") ||
                   g_str_has_prefix(ctype, "text/plain")) {
            gboolean plain = ctype && g_str_has_prefix(ctype, "text/plain");
            gsize use = MIN(len, (gsize)MAX_BODY);
            char *u8 = to_utf8(raw, use, cs);
            render_text(lv, final, u8, strlen(u8), plain, j->frag, j->restore, j->scroll);
            g_free(u8);
        } else {
            char *m = g_strdup_printf("Inhaltstyp %s kann Lite nicht anzeigen. Mit Strg+Umschalt+L im WebKit-Modus \xc3\xb6" "ffnen.", ctype);
            doc_message(lv, final, "Nicht darstellbar", m);
            g_free(m);
        }
        if (params)
            g_hash_table_destroy(params);
        g_free(ctype);
        g_free(final);
        g_bytes_unref(bytes);
    }
    lv->loading = FALSE;
    lv->progress = 0;
    notify(lv);
    job_free(j);
}

static void save_scroll(LiteView *lv)
{
    if (lv->hpos >= 0 && lv->hpos < (int)lv->hist->len) {
        HistEntry *h = g_ptr_array_index(lv->hist, lv->hpos);
        h->scroll = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(lv->scroll)));
    }
}

static void start_load(LiteView *lv, const char *url, gboolean push, gboolean restore, double scroll)
{
    char *key = key_of(url);
    const char *hash = strchr(url, '#');
    if (lv->url && !lv->loading && lv->doc && push) {
        char *cur = key_of(lv->url);
        gboolean same = strcmp(cur, key) == 0 && hash;
        g_free(cur);
        if (same) {
            char *mn = g_strconcat("id:", hash + 1, NULL);
            GtkTextMark *m = gtk_text_buffer_get_mark(lv->doc->buf, mn);
            g_free(mn);
            if (m) {
                save_scroll(lv);
                g_free(lv->url);
                lv->url = g_strdup(url);
                gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(lv->tv), m, 0.0, TRUE, 0.0, 0.05);
                notify(lv);
                g_free(key);
                return;
            }
        }
    }
    SoupMessage *msg = soup_message_new("GET", key);
    if (!msg) {
        doc_message(lv, url, "Ung\xc3\xbcltige Adresse", "Diese Adresse kann Lite nicht \xc3\xb6" "ffnen.");
        g_free(key);
        return;
    }
    save_scroll(lv);
    if (lv->cancel) {
        g_cancellable_cancel(lv->cancel);
        g_object_unref(lv->cancel);
    }
    lv->cancel = g_cancellable_new();
    soup_message_headers_append(soup_message_get_request_headers(msg), "Accept",
        "text/html,application/xhtml+xml,text/plain;q=0.8,image/*;q=0.6,*/*;q=0.3");
    Job *j = g_new0(Job, 1);
    j->lv = lv;
    j->cancel = g_object_ref(lv->cancel);
    j->msg = msg;
    j->url = g_strdup(url);
    j->frag = hash ? g_strdup(hash + 1) : NULL;
    j->push = push;
    j->restore = restore;
    j->scroll = scroll;
    lv->loading = TRUE;
    lv->progress = 0.15;
    notify(lv);
    soup_session_send_and_read_async(get_session(), msg, G_PRIORITY_DEFAULT, lv->cancel, on_fetched, j);
    g_free(key);
}

static void on_img_fetched(GObject *src, GAsyncResult *res, gpointer data)
{
    ImgJob *j = data;
    GError *err = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    gboolean dead = g_cancellable_is_cancelled(j->cancel);
    if (!dead && bytes && g_bytes_get_size(bytes) <= MAX_IMG) {
        GInputStream *st = g_memory_input_stream_new_from_bytes(bytes);
        GdkPixbuf *pb = gdk_pixbuf_new_from_stream(st, NULL, NULL);
        g_object_unref(st);
        if (pb) {
            GdkPixbuf *rot = gdk_pixbuf_apply_embedded_orientation(pb);
            if (rot) {
                g_object_unref(pb);
                pb = rot;
            }
            if (gdk_pixbuf_get_width(pb) > MAX_IMG_W) {
                int w = MAX_IMG_W;
                int h = gdk_pixbuf_get_height(pb) * w / gdk_pixbuf_get_width(pb);
                GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, w, MAX(h, 1), GDK_INTERP_BILINEAR);
                g_object_unref(pb);
                pb = sc;
            }
            GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
            g_object_unref(pb);
            GtkTextBuffer *buf = gtk_text_mark_get_buffer(j->mark);
            if (buf && buf == j->lv->doc->buf) {
                GtkTextIter it;
                gtk_text_buffer_get_iter_at_mark(buf, &it, j->mark);
                gtk_text_buffer_insert(buf, &it, "\n", 1);
                gtk_text_buffer_insert_paintable(buf, &it, GDK_PAINTABLE(tex));
                gtk_text_buffer_insert(buf, &it, "\n", 1);
                g_object_set_data(G_OBJECT(j->tag), "state", GINT_TO_POINTER(2));
            }
            g_object_unref(tex);
        } else {
            g_object_set_data(G_OBJECT(j->tag), "state", GINT_TO_POINTER(0));
        }
    } else if (!dead) {
        g_object_set_data(G_OBJECT(j->tag), "state", GINT_TO_POINTER(0));
    }
    if (bytes)
        g_bytes_unref(bytes);
    g_clear_error(&err);
    g_object_unref(j->cancel);
    g_object_unref(j->msg);
    g_free(j);
}

static void fetch_image(LiteView *lv, GtkTextTag *tag, GtkTextMark *mark)
{
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tag), "state")) != 0)
        return;
    const char *src = g_object_get_data(G_OBJECT(tag), "img");
    if (!src)
        return;
    SoupMessage *msg = soup_message_new("GET", src);
    if (!msg)
        return;
    g_object_set_data(G_OBJECT(tag), "state", GINT_TO_POINTER(1));
    ImgJob *j = g_new0(ImgJob, 1);
    j->lv = lv;
    if (!lv->cancel)
        lv->cancel = g_cancellable_new();
    j->cancel = g_object_ref(lv->cancel);
    j->msg = msg;
    j->tag = tag;
    j->mark = mark;
    soup_session_send_and_read_async(get_session(), msg, G_PRIORITY_DEFAULT, lv->cancel, on_img_fetched, j);
}

static GtkTextTag *tag_at(LiteView *lv, double x, double y, const char *key)
{
    GtkTextView *tv = GTK_TEXT_VIEW(lv->tv);
    int bx, by;
    gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    GtkTextIter it;
    if (!gtk_text_view_get_iter_at_location(tv, &it, bx, by))
        return NULL;
    GdkRectangle r;
    gtk_text_view_get_iter_location(tv, &it, &r);
    if (bx < r.x || bx > r.x + r.width || by < r.y || by > r.y + r.height)
        return NULL;
    GSList *tags = gtk_text_iter_get_tags(&it);
    GtkTextTag *hit = NULL;
    for (GSList *l = tags; l; l = l->next)
        if (g_object_get_data(G_OBJECT(l->data), key))
            hit = l->data;
    g_slist_free(tags);
    return hit;
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
    LiteView *lv = data;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(lv->tv));
    GtkTextIter a, b;
    if (gtk_text_buffer_get_selection_bounds(buf, &a, &b))
        return;
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    GtkTextTag *t = tag_at(lv, x, y, "href");
    if (t) {
        const char *href = g_object_get_data(G_OBJECT(t), "href");
        if (btn == 2 || (btn == 1 && (st & GDK_CONTROL_MASK))) {
            if (lv->hooks.open_link)
                lv->hooks.open_link(lv, href, TRUE, lv->hooks.data);
        } else if (btn == 1) {
            start_load(lv, href, TRUE, FALSE, 0);
        }
        return;
    }
    t = tag_at(lv, x, y, "img");
    if (t && btn == 1) {
        GtkTextView *tv = GTK_TEXT_VIEW(lv->tv);
        int bx, by;
        gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
        GtkTextIter it;
        gtk_text_view_get_iter_at_location(tv, &it, bx, by);
        if (!gtk_text_iter_ends_tag(&it, t))
            gtk_text_iter_forward_to_tag_toggle(&it, t);
        GtkTextMark *m = gtk_text_buffer_create_mark(buf, NULL, &it, TRUE);
        fetch_image(lv, t, m);
    }
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer data)
{
    LiteView *lv = data;
    GtkTextTag *t = tag_at(lv, x, y, "href");
    const char *href = t ? g_object_get_data(G_OBJECT(t), "href") : NULL;
    GtkTextTag *im = t ? NULL : tag_at(lv, x, y, "img");
    gboolean pointer = t != NULL || im != NULL;
    const char *show = href ? href : (im ? "Klicken zum Laden des Bildes" : NULL);
    if (g_strcmp0(show, lv->hover) == 0)
        return;
    g_free(lv->hover);
    lv->hover = g_strdup(show);
    gtk_widget_set_cursor_from_name(lv->tv, pointer ? "pointer" : NULL);
    if (show) {
        gtk_label_set_text(GTK_LABEL(lv->status), show);
        gtk_widget_set_visible(lv->status, TRUE);
    } else {
        gtk_widget_set_visible(lv->status, FALSE);
    }
}

static void on_leave(GtkEventControllerMotion *c, gpointer data)
{
    LiteView *lv = data;
    g_clear_pointer(&lv->hover, g_free);
    gtk_widget_set_visible(lv->status, FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
    LiteView *lv = data;
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
        return FALSE;
    GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(lv->scroll));
    double page = gtk_adjustment_get_page_size(a) * 0.9;
    double v = gtk_adjustment_get_value(a);
    double top = gtk_adjustment_get_upper(a) - gtk_adjustment_get_page_size(a);
    switch (keyval) {
    case GDK_KEY_space:
        v += (state & GDK_SHIFT_MASK) ? -page : page;
        break;
    case GDK_KEY_Page_Down:
        v += page;
        break;
    case GDK_KEY_Page_Up:
        v -= page;
        break;
    case GDK_KEY_Down:
    case GDK_KEY_j:
        v += 70;
        break;
    case GDK_KEY_Up:
    case GDK_KEY_k:
        v -= 70;
        break;
    case GDK_KEY_Home:
        v = 0;
        break;
    case GDK_KEY_End:
        v = top;
        break;
    default:
        return FALSE;
    }
    gtk_adjustment_set_value(a, CLAMP(v, 0, MAX(top, 0)));
    return TRUE;
}

static gboolean margin_tick(GtkWidget *w, GdkFrameClock *fc, gpointer data)
{
    LiteView *lv = data;
    int width = gtk_widget_get_width(lv->scroll);
    if (width == lv->last_w || width <= 0)
        return G_SOURCE_CONTINUE;
    lv->last_w = width;
    int m = MAX(18, (width - MAX_TEXT_W) / 2);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(lv->tv), m);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(lv->tv), m);
    return G_SOURCE_CONTINUE;
}

static void hist_free(gpointer p)
{
    HistEntry *h = p;
    g_free(h->url);
    g_free(h);
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    LiteView *lv = data;
    if (lv->cancel) {
        g_cancellable_cancel(lv->cancel);
        g_object_unref(lv->cancel);
    }
    lv->hooks.changed = NULL;
    lv->hooks.open_link = NULL;
    if (lv->baseline)
        g_hash_table_destroy(lv->baseline);
    g_ptr_array_free(lv->hist, TRUE);
    g_free(lv->url);
    g_free(lv->title);
    g_free(lv->baseline_url);
    g_free(lv->barbase);
    g_free(lv->hover);
    lv->doc = NULL;
    g_free(lv);
}

LiteView *lite_view_new(const LiteHooks *hooks)
{
    LiteView *lv = g_new0(LiteView, 1);
    if (hooks)
        lv->hooks = *hooks;
    lv->zoom = 1.0;
    lv->hpos = -1;
    lv->cidx = -1;
    lv->hist = g_ptr_array_new_with_free_func(hist_free);

    lv->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    lv->bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(lv->bar, "lite-bar");
    lv->barlabel = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lv->barlabel), 0);
    gtk_label_set_ellipsize(GTK_LABEL(lv->barlabel), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(lv->barlabel, TRUE);
    gtk_box_append(GTK_BOX(lv->bar), lv->barlabel);
    gtk_widget_set_visible(lv->bar, FALSE);

    lv->tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(lv->tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(lv->tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(lv->tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_pixels_inside_wrap(GTK_TEXT_VIEW(lv->tv), 3);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(lv->tv), 26);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(lv->tv), 90);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(lv->tv), 18);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(lv->tv), 18);

    lv->scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lv->scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lv->scroll), lv->tv);
    gtk_widget_set_vexpand(lv->scroll, TRUE);
    gtk_widget_set_hexpand(lv->scroll, TRUE);

    lv->status = gtk_label_new(NULL);
    gtk_widget_add_css_class(lv->status, "lite-status");
    gtk_widget_set_halign(lv->status, GTK_ALIGN_START);
    gtk_widget_set_valign(lv->status, GTK_ALIGN_END);
    gtk_label_set_ellipsize(GTK_LABEL(lv->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_visible(lv->status, FALSE);

    GtkWidget *ov = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(ov), lv->scroll);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), lv->status);
    gtk_widget_set_vexpand(ov, TRUE);

    gtk_box_append(GTK_BOX(lv->root), lv->bar);
    gtk_box_append(GTK_BOX(lv->root), ov);

    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
    g_signal_connect(g, "released", G_CALLBACK(on_click), lv);
    gtk_widget_add_controller(lv->tv, GTK_EVENT_CONTROLLER(g));

    GtkEventController *m = gtk_event_controller_motion_new();
    g_signal_connect(m, "motion", G_CALLBACK(on_motion), lv);
    g_signal_connect(m, "leave", G_CALLBACK(on_leave), lv);
    gtk_widget_add_controller(lv->tv, m);

    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_key), lv);
    gtk_widget_add_controller(lv->tv, k);

    gtk_widget_add_tick_callback(lv->scroll, margin_tick, lv, NULL);

    g_object_set_data(G_OBJECT(lv->root), "lite", lv);
    g_signal_connect(lv->root, "destroy", G_CALLBACK(on_destroy), lv);
    return lv;
}

LiteView *lite_from_widget(GtkWidget *w)
{
    return w ? g_object_get_data(G_OBJECT(w), "lite") : NULL;
}

GtkWidget *lite_view_widget(LiteView *lv)
{
    return lv->root;
}

void lite_view_load(LiteView *lv, const char *url)
{
    start_load(lv, url, TRUE, FALSE, 0);
}

void lite_view_reload(LiteView *lv)
{
    if (!lv->url)
        return;
    char *u = g_strdup(lv->url);
    start_load(lv, u, FALSE, TRUE, gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(lv->scroll))));
    g_free(u);
}

void lite_view_back(LiteView *lv)
{
    if (lv->hpos <= 0)
        return;
    save_scroll(lv);
    lv->hpos--;
    HistEntry *h = g_ptr_array_index(lv->hist, lv->hpos);
    char *u = g_strdup(h->url);
    start_load(lv, u, FALSE, TRUE, h->scroll);
    g_free(u);
}

void lite_view_forward(LiteView *lv)
{
    if (lv->hpos + 1 >= (int)lv->hist->len)
        return;
    save_scroll(lv);
    lv->hpos++;
    HistEntry *h = g_ptr_array_index(lv->hist, lv->hpos);
    char *u = g_strdup(h->url);
    start_load(lv, u, FALSE, TRUE, h->scroll);
    g_free(u);
}

gboolean lite_view_can_back(LiteView *lv)
{
    return lv->hpos > 0;
}

gboolean lite_view_can_forward(LiteView *lv)
{
    return lv->hpos + 1 < (int)lv->hist->len;
}

const char *lite_view_url(LiteView *lv)
{
    return lv->url;
}

const char *lite_view_title(LiteView *lv)
{
    return lv->title;
}

gboolean lite_view_loading(LiteView *lv)
{
    return lv->loading;
}

double lite_view_progress(LiteView *lv)
{
    return lv->progress;
}

void lite_view_zoom(LiteView *lv, double factor, gboolean reset)
{
    lv->zoom = reset ? 1.0 : CLAMP(lv->zoom * factor, 0.5, 3.0);
    if (lv->doc)
        apply_zoom_tags(lv->doc, lv->zoom);
}

void lite_view_focus(LiteView *lv)
{
    gtk_widget_grab_focus(lv->tv);
}

void lite_view_jump_change(LiteView *lv, gboolean forward)
{
    Doc *d = lv->doc;
    if (!d || d->changes->len == 0)
        return;
    int n = (int)d->changes->len;
    if (forward)
        lv->cidx = (lv->cidx + 1) % n;
    else
        lv->cidx = lv->cidx <= 0 ? n - 1 : lv->cidx - 1;
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(lv->tv), g_ptr_array_index(d->changes, lv->cidx), 0.0, TRUE, 0.0, 0.12);
    bar_refresh(lv);
}

static void find_clear_marks(Doc *d)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(d->buf, &s, &e);
    gtk_text_buffer_remove_tag(d->buf, d->fall, &s, &e);
    gtk_text_buffer_remove_tag(d->buf, d->fcur, &s, &e);
    for (guint i = 0; i < d->fs->len; i++) {
        gtk_text_buffer_delete_mark(d->buf, g_ptr_array_index(d->fs, i));
        gtk_text_buffer_delete_mark(d->buf, g_ptr_array_index(d->fe, i));
    }
    g_ptr_array_set_size(d->fs, 0);
    g_ptr_array_set_size(d->fe, 0);
    d->fidx = -1;
}

void lite_view_find_clear(LiteView *lv)
{
    Doc *d = lv->doc;
    if (!d)
        return;
    find_clear_marks(d);
    g_clear_pointer(&d->fquery, g_free);
}

void lite_view_find(LiteView *lv, const char *text, gboolean forward)
{
    Doc *d = lv->doc;
    if (!d)
        return;
    if (!text || !*text) {
        lite_view_find_clear(lv);
        return;
    }
    GtkTextView *tv = GTK_TEXT_VIEW(lv->tv);
    if (g_strcmp0(d->fquery, text) != 0) {
        find_clear_marks(d);
        g_free(d->fquery);
        d->fquery = g_strdup(text);
        GtkTextIter it, ms, me;
        gtk_text_buffer_get_start_iter(d->buf, &it);
        GtkTextSearchFlags fl = GTK_TEXT_SEARCH_CASE_INSENSITIVE | GTK_TEXT_SEARCH_VISIBLE_ONLY | GTK_TEXT_SEARCH_TEXT_ONLY;
        while (d->fs->len < 600 && gtk_text_iter_forward_search(&it, text, fl, &ms, &me, NULL)) {
            gtk_text_buffer_apply_tag(d->buf, d->fall, &ms, &me);
            g_ptr_array_add(d->fs, gtk_text_buffer_create_mark(d->buf, NULL, &ms, TRUE));
            g_ptr_array_add(d->fe, gtk_text_buffer_create_mark(d->buf, NULL, &me, FALSE));
            it = me;
        }
        gtk_text_tag_set_priority(d->fall, gtk_text_tag_table_get_size(gtk_text_buffer_get_tag_table(d->buf)) - 2);
        gtk_text_tag_set_priority(d->fcur, gtk_text_tag_table_get_size(gtk_text_buffer_get_tag_table(d->buf)) - 1);
        GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(lv->scroll));
        GtkTextIter top;
        gtk_text_view_get_iter_at_location(tv, &top, 0, (int)gtk_adjustment_get_value(a));
        int first = 0;
        for (guint i = 0; i < d->fs->len; i++) {
            GtkTextIter x;
            gtk_text_buffer_get_iter_at_mark(d->buf, &x, g_ptr_array_index(d->fs, i));
            if (gtk_text_iter_compare(&x, &top) >= 0) {
                first = (int)i;
                break;
            }
        }
        d->fidx = d->fs->len ? first : -1;
    } else if (d->fs->len) {
        int n = (int)d->fs->len;
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_mark(d->buf, &s, g_ptr_array_index(d->fs, d->fidx));
        gtk_text_buffer_get_iter_at_mark(d->buf, &e, g_ptr_array_index(d->fe, d->fidx));
        gtk_text_buffer_remove_tag(d->buf, d->fcur, &s, &e);
        d->fidx = forward ? (d->fidx + 1) % n : (d->fidx <= 0 ? n - 1 : d->fidx - 1);
    }
    if (d->fidx >= 0 && d->fs->len) {
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_mark(d->buf, &s, g_ptr_array_index(d->fs, d->fidx));
        gtk_text_buffer_get_iter_at_mark(d->buf, &e, g_ptr_array_index(d->fe, d->fidx));
        gtk_text_buffer_apply_tag(d->buf, d->fcur, &s, &e);
        gtk_text_view_scroll_to_mark(tv, g_ptr_array_index(d->fs, d->fidx), 0.1, TRUE, 0.0, 0.3);
    }
}
