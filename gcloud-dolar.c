/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Juliano Liberato
 *
 * gcloud-dolar: tema de screensaver para o xfce4-screensaver.
 *
 * Mostra a cotacao do dolar, o Ibovespa e a previsao do tempo sobre uma rede
 * de particulas. Nao acessa a rede:
 * le ~/.cache/gcloud-dolar/dolar.ini, gravado pelo gcloud-dolar-fetch.
 *
 * Uso:
 *   gcloud-dolar                       janela de teste (ou embutido, quando o
 *                                      xfce4-screensaver define XSCREENSAVER_WINDOW)
 *   gcloud-dolar --snapshot a.png [L A]  grava um quadro em PNG e sai
 */

#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOGICAL_H     360.0   /* todo o desenho usa esta altura logica */
#define LINK_DIST     95.0
#define MAX_NODES     140
#define N_PULSES      10
#define MAX_HIST      64
#define STALE_SECONDS (3 * 3600)
#define RELOAD_SECONDS 30
#define FRAME_MS      33

/* Identidade visual: troque aqui pelas cores oficiais */
#define BG_R 0.020
#define BG_G 0.027
#define BG_B 0.059
#define AC_R 0.220
#define AC_G 0.839
#define AC_B 0.910

/* Cores de dado: so aparecem onde ha alta ou queda (variacao e grafico) */
#define UP_R 0.204   /* alta:  #34D399 */
#define UP_G 0.827
#define UP_B 0.600
#define DN_R 0.973   /* queda: #F87171 */
#define DN_G 0.443
#define DN_B 0.443

#define COL_GAP        40.0   /* espaco entre as colunas dolar / Ibovespa */
#define CHART_TOP      176.0
#define CHART_BOTTOM   256.0
/* Faixa vertical minima do grafico, como fracao do valor. Sem isso, uma
 * oscilacao de 0,2% ocuparia a altura toda e pareceria um tombo. */
#define CHART_MIN_SPAN 0.03

typedef struct { double x, y, vx, vy; } Node;
typedef struct { int a, b; double k; } Pulse;

typedef struct {
    gboolean have_data;
    double   valor;
    double   pct;
    char     fonte[32];
    gint64   atualizado_em;
    double   hist[MAX_HIST];
    int      n_hist;
} Quote;

typedef struct {
    gboolean have_data;
    char     cidade[64];
    char     descricao[48];
    double   temp, max, min;
    int      chuva;          /* probabilidade em %, -1 se desconhecida */
    gint64   atualizado_em;
} Weather;

typedef struct {
    Node    nodes[MAX_NODES];
    int     n_nodes;
    Pulse   pulses[N_PULSES];
    double  logical_w;
    long    tick;

    Quote   dolar;
    Quote   ibov;
    Weather tempo;
} State;

static State st;

static double
frand (double a, double b)
{
    return a + g_random_double () * (b - a);
}

static char *
cache_path (void)
{
    const char *env = g_getenv ("GCLOUD_DOLAR_CACHE");
    if (env != NULL && *env != '\0')
        return g_strdup (env);
    return g_build_filename (g_get_home_dir (), ".cache", "gcloud-dolar", "dolar.ini", NULL);
}

static void
load_quote (GKeyFile *kf, const char *grupo, Quote *q)
{
    GError *err = NULL;
    double  v = g_key_file_get_double (kf, grupo, "valor", &err);

    /* se a leitura falhar, o ultimo valor em memoria continua valendo */
    if (err == NULL && v > 0) {
        gsize    n = 0;
        gdouble *h;
        char    *f;

        q->valor = v;
        q->pct = g_key_file_get_double (kf, grupo, "pct", NULL);
        q->atualizado_em = g_key_file_get_int64 (kf, grupo, "atualizado_em", NULL);

        f = g_key_file_get_string (kf, grupo, "fonte", NULL);
        g_strlcpy (q->fonte, f != NULL ? f : "", sizeof q->fonte);
        g_free (f);

        h = g_key_file_get_double_list (kf, grupo, "hist", &n, NULL);
        q->n_hist = 0;
        if (h != NULL) {
            gsize start = n > MAX_HIST ? n - MAX_HIST : 0;
            for (gsize i = start; i < n; i++)
                q->hist[q->n_hist++] = h[i];
            g_free (h);
        }
        q->have_data = TRUE;
    }
    g_clear_error (&err);
}

static void
load_weather (GKeyFile *kf, Weather *w)
{
    GError *err = NULL;
    double  t = g_key_file_get_double (kf, "tempo", "temp", &err);
    char   *c = g_key_file_get_string (kf, "tempo", "cidade", NULL);

    if (err == NULL && c != NULL && *c != '\0') {
        char *d = g_key_file_get_string (kf, "tempo", "descricao", NULL);

        g_strlcpy (w->cidade, c, sizeof w->cidade);
        g_strlcpy (w->descricao, d != NULL ? d : "", sizeof w->descricao);
        g_free (d);
        w->temp = t;
        w->max = g_key_file_get_double (kf, "tempo", "max", NULL);
        w->min = g_key_file_get_double (kf, "tempo", "min", NULL);
        w->chuva = g_key_file_has_key (kf, "tempo", "chuva", NULL)
                   ? g_key_file_get_integer (kf, "tempo", "chuva", &err) : -1;
        if (err != NULL)
            w->chuva = -1;
        w->atualizado_em = g_key_file_get_int64 (kf, "tempo", "atualizado_em", NULL);
        w->have_data = TRUE;
    }
    g_clear_error (&err);
    g_free (c);
}

static void
load_data (void)
{
    char     *path = cache_path ();
    GKeyFile *kf = g_key_file_new ();

    if (g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL)) {
        load_quote (kf, "dolar", &st.dolar);
        load_quote (kf, "ibov", &st.ibov);
        load_weather (kf, &st.tempo);
    }
    g_key_file_free (kf);
    g_free (path);
}

static int
nearest (int a)
{
    int    best = -1;
    double bd = LINK_DIST;

    for (int j = 0; j < st.n_nodes; j++) {
        double d;
        if (j == a)
            continue;
        d = hypot (st.nodes[a].x - st.nodes[j].x, st.nodes[a].y - st.nodes[j].y);
        if (d < bd) {
            bd = d;
            best = j;
        }
    }
    return best;
}

static void
pulse_reset (Pulse *p)
{
    p->a = g_random_int_range (0, st.n_nodes);
    p->b = nearest (p->a);
    p->k = 0;
}

static void
init_net (double lw)
{
    st.logical_w = lw;
    st.n_nodes = CLAMP ((int) (40.0 * lw / 680.0), 12, MAX_NODES);
    for (int i = 0; i < st.n_nodes; i++) {
        st.nodes[i].x = frand (0, lw);
        st.nodes[i].y = frand (0, LOGICAL_H);
        st.nodes[i].vx = frand (-0.4, 0.4);
        st.nodes[i].vy = frand (-0.4, 0.4);
    }
    for (int i = 0; i < N_PULSES; i++)
        pulse_reset (&st.pulses[i]);
}

static void
step (void)
{
    st.tick++;
    for (int i = 0; i < st.n_nodes; i++) {
        Node *p = &st.nodes[i];
        p->x += p->vx;
        p->y += p->vy;
        if (p->x < 0 || p->x > st.logical_w)
            p->vx = -p->vx;
        if (p->y < 0 || p->y > LOGICAL_H)
            p->vy = -p->vy;
    }
    for (int i = 0; i < N_PULSES; i++) {
        Pulse *q = &st.pulses[i];
        if (q->b < 0) {
            pulse_reset (q);
            continue;
        }
        q->k += 0.02;
        if (q->k >= 1 ||
            hypot (st.nodes[q->a].x - st.nodes[q->b].x,
                   st.nodes[q->a].y - st.nodes[q->b].y) > LINK_DIST)
            pulse_reset (q);
    }
}

static void
draw_net (cairo_t *cr, double alpha)
{
    cairo_set_line_width (cr, 1.0);
    for (int i = 0; i < st.n_nodes; i++) {
        for (int j = i + 1; j < st.n_nodes; j++) {
            double d = hypot (st.nodes[i].x - st.nodes[j].x, st.nodes[i].y - st.nodes[j].y);
            if (d < LINK_DIST) {
                cairo_set_source_rgba (cr, AC_R, AC_G, AC_B, (1 - d / LINK_DIST) * 0.5 * alpha);
                cairo_move_to (cr, st.nodes[i].x, st.nodes[i].y);
                cairo_line_to (cr, st.nodes[j].x, st.nodes[j].y);
                cairo_stroke (cr);
            }
        }
    }
    cairo_set_source_rgba (cr, AC_R, AC_G, AC_B, 0.8 * alpha);
    for (int i = 0; i < st.n_nodes; i++) {
        cairo_arc (cr, st.nodes[i].x, st.nodes[i].y, 2.0, 0, 2 * G_PI);
        cairo_fill (cr);
    }
    cairo_set_source_rgba (cr, 1, 1, 1, alpha);
    for (int i = 0; i < N_PULSES; i++) {
        Pulse *q = &st.pulses[i];
        if (q->b < 0)
            continue;
        cairo_arc (cr,
                   st.nodes[q->a].x + (st.nodes[q->b].x - st.nodes[q->a].x) * q->k,
                   st.nodes[q->a].y + (st.nodes[q->b].y - st.nodes[q->a].y) * q->k,
                   3.0, 0, 2 * G_PI);
        cairo_fill (cr);
    }
}

/* escreve e devolve o avanco horizontal do texto */
static double
text (cairo_t *cr, const char *s, double x, double y, double size,
      gboolean bold, gboolean right)
{
    cairo_text_extents_t ext;

    cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, size);
    cairo_text_extents (cr, s, &ext);
    if (right)
        x -= ext.x_advance;
    cairo_move_to (cr, x, y);
    cairo_show_text (cr, s);
    return ext.x_advance;
}

/* como text(), mas reduz a fonte se o texto passar de maxw */
static void
text_fit (cairo_t *cr, const char *s, double x, double y, double size,
          gboolean bold, double maxw)
{
    cairo_text_extents_t ext;

    cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, size);
    cairo_text_extents (cr, s, &ext);
    if (ext.x_advance > maxw)
        size *= maxw / ext.x_advance;
    text (cr, s, x, y, size, bold, FALSE);
}

/*
 * Numero com virgula decimal, sem depender do locale. Com sinal=TRUE poe
 * "+" na frente dos positivos; um valor que arredonda para zero sai sem sinal.
 */
static char *
fmt_br (double v, int casas, gboolean sinal)
{
    char        buf[G_ASCII_DTOSTR_BUF_SIZE];
    char        fmt[8];
    char       *p;
    double      esc = pow (10, casas);
    double      r = round (v * esc) / esc;
    const char *pre = "";   /* "-" ASCII: a fonte nao tem o menos tipografico */

    g_snprintf (fmt, sizeof fmt, "%%.%df", casas);
    g_ascii_formatd (buf, sizeof buf, fmt, fabs (r));
    p = strchr (buf, '.');
    if (p != NULL)
        *p = ',';
    if (r < 0)
        pre = "-";
    else if (r > 0 && sinal)
        pre = "+";
    return g_strconcat (pre, buf, NULL);
}

/* inteiro com ponto de milhar: 165954 -> "165.954" */
static char *
fmt_milhar (double v)
{
    char     buf[32];
    GString *out = g_string_new (NULL);
    int      len;

    g_snprintf (buf, sizeof buf, "%ld", lround (v));
    len = (int) strlen (buf);
    for (int i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0 && buf[i - 1] != '-')
            g_string_append_c (out, '.');
        g_string_append_c (out, buf[i]);
    }
    return g_string_free (out, FALSE);
}

static void
set_trend_color (cairo_t *cr, gboolean up, double alpha)
{
    if (up)
        cairo_set_source_rgba (cr, UP_R, UP_G, UP_B, alpha);
    else
        cairo_set_source_rgba (cr, DN_R, DN_G, DN_B, alpha);
}

/*
 * Seta + percentual. A direcao aparece tres vezes (seta, sinal e cor) para
 * nao depender so de distinguir vermelho de verde. Devolve a largura usada.
 */
static double
draw_change (cairo_t *cr, double pct, int casas, double x, double y, double size)
{
    gboolean up = pct >= 0;
    char    *p = fmt_br (pct, casas, TRUE);
    char    *s = g_strconcat (p, "%", NULL);
    double   tw = size * 0.56, th = size * 0.50, adv;

    set_trend_color (cr, up, 1.0);
    if (up) {
        cairo_move_to (cr, x, y - 1);
        cairo_line_to (cr, x + tw, y - 1);
        cairo_line_to (cr, x + tw / 2, y - 1 - th);
    } else {
        cairo_move_to (cr, x, y - 1 - th);
        cairo_line_to (cr, x + tw, y - 1 - th);
        cairo_line_to (cr, x + tw / 2, y - 1);
    }
    cairo_close_path (cr);
    cairo_fill (cr);

    adv = tw + size * 0.30;
    adv += text (cr, s, x + adv, y, size, FALSE, FALSE);
    g_free (s);
    g_free (p);
    return adv;
}

/*
 * Curva suave que passa por todos os pontos sem inventar picos nem vales
 * (interpolacao cubica monotona, Fritsch-Carlson). xs tem passo constante.
 */
static void
chart_path (cairo_t *cr, const double *xs, const double *ys, int n)
{
    double m[MAX_HIST], d[MAX_HIST];
    double h = xs[1] - xs[0];

    for (int i = 0; i < n - 1; i++)
        d[i] = (ys[i + 1] - ys[i]) / h;
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (int i = 1; i < n - 1; i++)
        m[i] = d[i - 1] * d[i] > 0 ? (d[i - 1] + d[i]) / 2 : 0;
    for (int i = 0; i < n - 1; i++) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
        } else {
            double a = m[i] / d[i], b = m[i + 1] / d[i];
            double q = a * a + b * b;
            if (q > 9) {
                double t = 3 / sqrt (q);
                m[i] = t * a * d[i];
                m[i + 1] = t * b * d[i];
            }
        }
    }
    cairo_move_to (cr, xs[0], ys[0]);
    for (int i = 0; i < n - 1; i++)
        cairo_curve_to (cr,
                        xs[i] + h / 3, ys[i] + m[i] * h / 3,
                        xs[i + 1] - h / 3, ys[i + 1] - m[i + 1] * h / 3,
                        xs[i + 1], ys[i + 1]);
}

/*
 * Grafico do periodo. A cor diz se o periodo mostrado fechou em alta ou em
 * queda (ultimo ponto contra o primeiro) - a mesma conta do percentual da
 * legenda, para a cor da linha nunca contradizer o desenho dela.
 */
static void
draw_chart (cairo_t *cr, const Quote *q, double x, double w)
{
    /* muitas camadas finas: o brilho cai aos poucos, sem virar um contorno */
    static const double glow_w[] = { 20.0, 15.0, 11.0, 8.0, 5.5, 3.6 };
    static const double glow_a[] = { 0.022, 0.03, 0.04, 0.055, 0.075, 0.10 };
    const double     top = CHART_TOP, bottom = CHART_BOTTOM;
    double           xs[MAX_HIST], ys[MAX_HIST];
    int              n = q->n_hist;
    double           mn = q->hist[0], mx = q->hist[0];
    double           mid, span, lo, ex, ey, adv;
    double           var = q->hist[0] != 0 ? (q->hist[n - 1] / q->hist[0] - 1) * 100 : 0;
    gboolean         up = q->hist[n - 1] >= q->hist[0];
    cairo_pattern_t *grad;
    char            *s;

    for (int i = 1; i < n; i++) {
        mn = MIN (mn, q->hist[i]);
        mx = MAX (mx, q->hist[i]);
    }
    mid = (mn + mx) / 2;
    span = MAX (mx - mn, fabs (mid) * CHART_MIN_SPAN);
    lo = mid - span / 2;
    for (int i = 0; i < n; i++) {
        xs[i] = x + i * w / (n - 1);
        ys[i] = bottom - (q->hist[i] - lo) / span * (bottom - top);
    }
    ex = xs[n - 1];
    ey = ys[n - 1];

    /* referencia: o nivel em que o periodo comecou */
    cairo_save (cr);
    cairo_set_source_rgba (cr, 1, 1, 1, 0.20);
    cairo_set_line_width (cr, 1.0);
    cairo_set_dash (cr, (const double[]) { 1.5, 4.0 }, 2, 0);
    cairo_move_to (cr, x, ys[0]);
    cairo_line_to (cr, x + w, ys[0]);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* area sob a curva, sumindo para baixo */
    chart_path (cr, xs, ys, n);
    cairo_line_to (cr, ex, bottom + 8);
    cairo_line_to (cr, x, bottom + 8);
    cairo_close_path (cr);
    grad = cairo_pattern_create_linear (0, top, 0, bottom + 8);
    if (up) {
        cairo_pattern_add_color_stop_rgba (grad, 0, UP_R, UP_G, UP_B, 0.14);
        cairo_pattern_add_color_stop_rgba (grad, 1, UP_R, UP_G, UP_B, 0.0);
    } else {
        cairo_pattern_add_color_stop_rgba (grad, 0, DN_R, DN_G, DN_B, 0.14);
        cairo_pattern_add_color_stop_rgba (grad, 1, DN_R, DN_G, DN_B, 0.0);
    }
    /* as laterais da area somem aos poucos, em vez de terminar num corte reto */
    cairo_push_group (cr);
    cairo_set_source (cr, grad);
    cairo_fill (cr);
    cairo_pop_group_to_source (cr);
    cairo_pattern_destroy (grad);
    grad = cairo_pattern_create_linear (x, 0, x + w, 0);
    cairo_pattern_add_color_stop_rgba (grad, 0.00, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba (grad, 0.10, 0, 0, 0, 1);
    cairo_pattern_add_color_stop_rgba (grad, 0.90, 0, 0, 0, 1);
    cairo_pattern_add_color_stop_rgba (grad, 1.00, 0, 0, 0, 0);
    cairo_mask (cr, grad);
    cairo_pattern_destroy (grad);

    /* brilho: a mesma curva, mais larga e bem transparente, em camadas */
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    for (guint k = 0; k < G_N_ELEMENTS (glow_w); k++) {
        chart_path (cr, xs, ys, n);
        cairo_set_line_width (cr, glow_w[k]);
        set_trend_color (cr, up, glow_a[k]);
        cairo_stroke (cr);
    }
    chart_path (cr, xs, ys, n);
    cairo_set_line_width (cr, 1.8);
    set_trend_color (cr, up, 1.0);
    cairo_stroke (cr);

    /* ponto de agora, com um halo que respira devagar */
    set_trend_color (cr, up, 0.20);
    cairo_arc (cr, ex, ey, 7.5 + 1.5 * sin (st.tick * 2 * G_PI / 100.0), 0, 2 * G_PI);
    cairo_fill (cr);
    set_trend_color (cr, up, 1.0);
    cairo_arc (cr, ex, ey, 3.2, 0, 2 * G_PI);
    cairo_fill (cr);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_arc (cr, ex, ey, 1.3, 0, 2 * G_PI);
    cairo_fill (cr);

    /* legenda: o que e o grafico e quanto variou */
    s = g_strdup_printf ("%d pregões", n);
    cairo_set_source_rgba (cr, 1, 1, 1, 0.55);
    adv = text (cr, s, x, bottom + 22, 11, FALSE, FALSE);
    g_free (s);
    draw_change (cr, var, 1, x + adv + 10, bottom + 22, 11);
}

/* uma coluna do painel: rotulo, valor, variacao do dia e grafico do periodo */
static void
draw_quote (cairo_t *cr, const Quote *q, double x, double w, gint64 agora,
            const char *rotulo, const char *vazio, char *valor)
{
    cairo_set_source_rgba (cr, 1, 1, 1, 0.6);
    text_fit (cr, rotulo, x, 50, 14, FALSE, w);

    if (!q->have_data) {
        cairo_set_source_rgb (cr, 1, 1, 1);
        text_fit (cr, vazio, x, 120, 64, TRUE, w);
        cairo_set_source_rgba (cr, 1, 1, 1, 0.6);
        text_fit (cr, "aguardando a primeira cotação", x, 146, 14, FALSE, w);
    } else {
        double adv;

        cairo_set_source_rgb (cr, 1, 1, 1);
        text_fit (cr, valor, x, 120, 64, TRUE, w);

        adv = draw_change (cr, q->pct, 2, x, 146, 16);
        cairo_set_source_rgba (cr, 1, 1, 1, 0.6);
        adv += text (cr, " hoje", x + adv, 146, 16, FALSE, FALSE);

        if (agora - q->atualizado_em > STALE_SECONDS) {
            cairo_set_source_rgb (cr, 0.980, 0.780, 0.459);
            text (cr, "desatualizado", x + adv + 12, 146, 12, FALSE, FALSE);
        }
    }
    g_free (valor);

    if (q->n_hist >= 2)
        draw_chart (cr, q, x, w);
}

static void
draw_panel (cairo_t *cr, double lw)
{
    double     left = 48, right = lw - 48;
    double     colw = (right - left - COL_GAP) / 2;
    GDateTime *now = g_date_time_new_now_local ();
    gint64     agora = g_date_time_to_unix (now);
    char      *v, *s;

    v = fmt_br (st.dolar.valor, 2, FALSE);
    draw_quote (cr, &st.dolar, left, colw, agora,
                g_str_equal (st.dolar.fonte, "BCB") ? "Dólar PTAX  ·  Banco Central" : "Dólar comercial",
                "R$ --,--", g_strconcat ("R$ ", v, NULL));
    g_free (v);

    draw_quote (cr, &st.ibov, left + colw + COL_GAP, colw, agora,
                "Ibovespa  ·  pontos", "--", fmt_milhar (st.ibov.valor));

    /* rodape esquerdo: de quando sao os numeros (o mais antigo dos dois) */
    if (st.dolar.have_data || st.ibov.have_data) {
        gint64     t = st.dolar.have_data ? st.dolar.atualizado_em : st.ibov.atualizado_em;
        GDateTime *dt;

        if (st.ibov.have_data)
            t = MIN (t, st.ibov.atualizado_em);
        dt = g_date_time_new_from_unix_local (t);
        if (dt != NULL) {
            s = g_date_time_format (dt, "Cotações atualizadas às %H:%M");
            cairo_set_source_rgba (cr, 1, 1, 1, 0.45);
            text (cr, s, left, 339, 11, FALSE, FALSE);
            g_free (s);
            g_date_time_unref (dt);
        }
    }

    /* rodape direito: relogio e, abaixo dele, a previsao do tempo */
    {
        gboolean tempo_ok = st.tempo.have_data &&
                            agora - st.tempo.atualizado_em <= STALE_SECONDS;

        cairo_set_source_rgba (cr, 1, 1, 1, 0.85);
        s = g_date_time_format (now, "%H:%M");
        text (cr, s, right, tempo_ok ? 308 : 339, 18, FALSE, TRUE);
        g_free (s);

        if (tempo_ok) {
            const Weather *w = &st.tempo;
            GString       *l = g_string_new (w->cidade);

            g_string_append_printf (l, "  ·  %ld°", lround (w->temp));
            if (w->descricao[0] != '\0')
                g_string_append_printf (l, "  %s", w->descricao);
            cairo_set_source_rgba (cr, 1, 1, 1, 0.85);
            text (cr, l->str, right, 324, 13, FALSE, TRUE);

            g_string_printf (l, "Hoje  máx %ld°  mín %ld°", lround (w->max), lround (w->min));
            if (w->chuva >= 0)
                g_string_append_printf (l, "  ·  chuva %d%%", w->chuva);
            cairo_set_source_rgba (cr, 1, 1, 1, 0.6);
            text (cr, l->str, right, 339, 12, FALSE, TRUE);
            g_string_free (l, TRUE);
        }
    }

    g_date_time_unref (now);
}

static void
render (cairo_t *cr, int w, int h)
{
    double scale = h / LOGICAL_H;
    double lw = w / scale;

    if (st.n_nodes == 0 || fabs (lw - st.logical_w) > 1.0)
        init_net (lw);

    cairo_set_source_rgb (cr, BG_R, BG_G, BG_B);
    cairo_paint (cr);

    cairo_scale (cr, scale, scale);
    draw_net (cr, 0.35);

    /* deriva lenta do painel: evita imagem parada no mesmo ponto do monitor */
    cairo_translate (cr, 10.0 * sin (st.tick / 900.0), 6.0 * cos (st.tick / 700.0));
    draw_panel (cr, lw);
}

static gboolean
on_draw (GtkWidget *widget, cairo_t *cr, gpointer data)
{
    render (cr, gtk_widget_get_allocated_width (widget),
            gtk_widget_get_allocated_height (widget));
    return TRUE;
}

static gboolean
on_frame (gpointer data)
{
    step ();
    gtk_widget_queue_draw (GTK_WIDGET (data));
    return G_SOURCE_CONTINUE;
}

static gboolean
on_reload (gpointer data)
{
    load_data ();
    return G_SOURCE_CONTINUE;
}

static int
snapshot (const char *file, int w, int h)
{
    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_RGB24, w, h);
    cairo_t         *cr = cairo_create (surf);
    cairo_status_t   status;

    load_data ();
    init_net ((double) w / (h / LOGICAL_H));
    for (int i = 0; i < 200; i++)
        step ();
    render (cr, w, h);
    status = cairo_surface_write_to_png (surf, file);
    cairo_destroy (cr);
    cairo_surface_destroy (surf);
    return status == CAIRO_STATUS_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
main (int argc, char **argv)
{
    GtkWidget  *window;
    GtkWidget  *area;
    const char *wid;

    if (argc >= 3 && g_str_equal (argv[1], "--snapshot")) {
        int w = argc >= 5 ? atoi (argv[3]) : 1360;
        int h = argc >= 5 ? atoi (argv[4]) : 720;
        return snapshot (argv[2], MAX (w, 320), MAX (h, 180));
    }

    gtk_init (&argc, &argv);
    g_set_prgname ("gcloud-dolar");

    wid = g_getenv ("XSCREENSAVER_WINDOW");
    if (wid != NULL && *wid != '\0') {
        if (!GDK_IS_X11_DISPLAY (gdk_display_get_default ())) {
            g_warning ("Este MVP so funciona em sessao X11");
            return EXIT_FAILURE;
        }
        window = gtk_plug_new ((Window) strtoul (wid, NULL, 0));
    } else {
        window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title (GTK_WINDOW (window), "GCLOUD - dólar (teste)");
        gtk_window_set_default_size (GTK_WINDOW (window), 1020, 540);
    }
    g_signal_connect (window, "destroy", G_CALLBACK (gtk_main_quit), NULL);

    area = gtk_drawing_area_new ();
    g_signal_connect (area, "draw", G_CALLBACK (on_draw), NULL);
    gtk_container_add (GTK_CONTAINER (window), area);

    load_data ();
    g_timeout_add (FRAME_MS, on_frame, area);
    g_timeout_add_seconds (RELOAD_SECONDS, on_reload, NULL);

    gtk_widget_show_all (window);
    gtk_main ();
    return EXIT_SUCCESS;
}
