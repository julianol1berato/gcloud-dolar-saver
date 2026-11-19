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
 *
 * Ha dois desenhistas. O padrao usa OpenGL: a placa de video pinta o quadro e
 * o custo de CPU cai para menos da metade. Se nao houver OpenGL, entra o de
 * reserva, com GTK e cairo. O painel (textos e graficos) e sempre do cairo.
 *
 * Variaveis de ambiente, para teste e medicao:
 *   GCLOUD_DOLAR_CACHE   outro arquivo de dados
 *   GCLOUD_DOLAR_FPS     quadros por segundo fixos (ignora tomada/bateria)
 *   GCLOUD_DOLAR_RENDER  "gl" ou "gtk": obriga um dos desenhistas
 *   GCLOUD_DOLAR_POWER_DIR  outro diretorio no lugar de /sys/class/power_supply
 */

#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <poll.h>
#include <signal.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOGICAL_H     360.0   /* o painel e desenhado nesta altura logica */
#define LOGICAL_W_MIN 560.0   /* e precisa de pelo menos esta largura */
#define LINK_DIST     95.0
#define MAX_NODES     140
#define N_PULSES      10
#define MAX_HIST      64
#define STALE_SECONDS (3 * 3600)
#ifndef RELOAD_SECONDS
#define RELOAD_SECONDS 30
#endif
/*
 * Quadros por segundo. O custo de CPU e proporcional a este numero: cada
 * quadro faz o compositor de janelas recompor a tela. Na tomada, 30 da
 * movimento suave; na bateria, 10 corta o consumo para menos da metade (a
 * rede fica visivelmente mais "aos passos", o painel nao muda).
 */
#define FPS_AC        30
#define FPS_BATTERY   10
#define POWER_CHECK_SECONDS 5
#define NET_ALPHA     0.35    /* opacidade geral da rede de fundo */

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
    double  logical_w, logical_h;
    double  t;               /* segundos de animacao */

    Quote   dolar;
    Quote   ibov;
    Weather tempo;
} State;

static State st;

/*
 * O painel (textos e graficos) so muda quando chegam dados novos ou vira o
 * minuto, entao e desenhado uma vez numa superficie e so copiado a cada quadro.
 * Desenhar brilho e degrades 30 vezes por segundo custava um nucleo inteiro
 * do servidor X. So o halo do ponto final, que pulsa, e desenhado por quadro.
 */
static cairo_surface_t *panel_cache;
static int              panel_w, panel_h;
static gint64           panel_minute = -1;
static gboolean         panel_dirty = TRUE;

typedef struct { double x, y; gboolean up; } Halo;
static Halo halos[2];
static int  n_halos;

/* Medidas do quadro: escala logica -> pixels e onde o painel fica */
typedef struct {
    int    w, h;       /* tamanho da area, em pixels */
    double scale;      /* pixels por unidade logica */
    double lw, lh;     /* tamanho da area em unidades logicas */
    double oy;         /* deslocamento vertical do painel (telas em pe) */
} Layout;

static void
layout_for (Layout *l, int w, int h)
{
    l->w = w;
    l->h = h;
    l->scale = MIN (h / LOGICAL_H, w / LOGICAL_W_MIN);
    l->lw = w / l->scale;
    l->lh = h / l->scale;
    l->oy = (l->lh - LOGICAL_H) / 2;
}

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

/* Numero utilizavel: nan, inf e valores absurdos quebrariam as contas do desenho. */
#define SANE(v) (isfinite (v) && fabs (v) < 1e12)

static void
load_quote (GKeyFile *kf, const char *grupo, Quote *q)
{
    GError *err = NULL;
    double  v = g_key_file_get_double (kf, grupo, "valor", &err);

    /* se a leitura falhar, o ultimo valor em memoria continua valendo */
    if (err == NULL && SANE (v) && v > 0) {
        gsize    n = 0;
        gdouble *h;
        char    *f;

        q->valor = v;
        q->pct = g_key_file_get_double (kf, grupo, "pct", NULL);
        if (!SANE (q->pct))
            q->pct = 0;
        q->atualizado_em = g_key_file_get_int64 (kf, grupo, "atualizado_em", NULL);

        f = g_key_file_get_string (kf, grupo, "fonte", NULL);
        g_strlcpy (q->fonte, f != NULL ? f : "", sizeof q->fonte);
        g_free (f);

        h = g_key_file_get_double_list (kf, grupo, "hist", &n, NULL);
        q->n_hist = 0;
        if (h != NULL) {
            gsize start = n > MAX_HIST ? n - MAX_HIST : 0;
            for (gsize i = start; i < n; i++)
                if (SANE (h[i]))
                    q->hist[q->n_hist++] = h[i];
            g_free (h);
        }
        q->have_data = TRUE;
    }
    g_clear_error (&err);
}

#define TEMP_OK(v) (isfinite (v) && fabs (v) < 100)   /* graus Celsius */

static void
load_weather (GKeyFile *kf, Weather *w)
{
    GError *err = NULL;
    double  t = g_key_file_get_double (kf, "tempo", "temp", &err);
    char   *c = g_key_file_get_string (kf, "tempo", "cidade", NULL);

    if (err == NULL && TEMP_OK (t) && c != NULL && *c != '\0') {
        char *d = g_key_file_get_string (kf, "tempo", "descricao", NULL);

        g_strlcpy (w->cidade, c, sizeof w->cidade);
        g_strlcpy (w->descricao, d != NULL ? d : "", sizeof w->descricao);
        g_free (d);
        w->temp = t;
        w->max = g_key_file_get_double (kf, "tempo", "max", NULL);
        w->min = g_key_file_get_double (kf, "tempo", "min", NULL);
        if (!TEMP_OK (w->max))
            w->max = t;
        if (!TEMP_OK (w->min))
            w->min = t;
        w->chuva = g_key_file_has_key (kf, "tempo", "chuva", NULL)
                   ? g_key_file_get_integer (kf, "tempo", "chuva", &err) : -1;
        if (err != NULL || w->chuva > 100)
            w->chuva = -1;
        w->atualizado_em = g_key_file_get_int64 (kf, "tempo", "atualizado_em", NULL);
        w->have_data = TRUE;
    }
    g_clear_error (&err);
    g_free (c);
}

/* Le o arquivo de dados, mas so se ele mudou desde a ultima leitura. */
static void
load_data (void)
{
    static GStatBuf last;
    static gboolean have_last;
    char           *path = cache_path ();
    GStatBuf        sb;
    GKeyFile       *kf;

    if (g_stat (path, &sb) != 0) {
        g_free (path);
        return;
    }
    if (have_last && sb.st_mtime == last.st_mtime && sb.st_size == last.st_size &&
        sb.st_ino == last.st_ino) {
        g_free (path);
        return;
    }
    kf = g_key_file_new ();
    if (g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL)) {
        load_quote (kf, "dolar", &st.dolar);
        load_quote (kf, "ibov", &st.ibov);
        load_weather (kf, &st.tempo);
        panel_dirty = TRUE;
        last = sb;
        have_last = TRUE;
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
init_net (double lw, double lh)
{
    st.logical_w = lw;
    st.logical_h = lh;
    st.n_nodes = CLAMP ((int) (40.0 * lw * lh / (680.0 * LOGICAL_H)), 12, MAX_NODES);
    for (int i = 0; i < st.n_nodes; i++) {
        st.nodes[i].x = frand (0, lw);
        st.nodes[i].y = frand (0, lh);
        st.nodes[i].vx = frand (-12, 12);      /* unidades logicas por segundo */
        st.nodes[i].vy = frand (-12, 12);
    }
    for (int i = 0; i < N_PULSES; i++)
        pulse_reset (&st.pulses[i]);
}

/*
 * Avanca a animacao em dt segundos. O movimento e medido em tempo, nao em
 * quadros: mudar a taxa de quadros (ou perder um quadro) nao muda a velocidade.
 */
static void
step (double dt)
{
    st.t += dt;
    for (int i = 0; i < st.n_nodes; i++) {
        Node *p = &st.nodes[i];
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        if (p->x < 0)
            p->vx = fabs (p->vx);
        else if (p->x > st.logical_w)
            p->vx = -fabs (p->vx);
        if (p->y < 0)
            p->vy = fabs (p->vy);
        else if (p->y > st.logical_h)
            p->vy = -fabs (p->vy);
    }
    for (int i = 0; i < N_PULSES; i++) {
        Pulse *q = &st.pulses[i];
        if (q->b < 0) {
            pulse_reset (q);
            continue;
        }
        q->k += 0.6 * dt;
        if (q->k >= 1 ||
            hypot (st.nodes[q->a].x - st.nodes[q->b].x,
                   st.nodes[q->a].y - st.nodes[q->b].y) > LINK_DIST)
            pulse_reset (q);
    }
}

/*
 * Rede de particulas, rasterizada direto numa mascara de 1 byte por pixel.
 * Sao so linhas finas e pontos, entao um rasterizador proprio da conta; o
 * tracador generico do cairo levava 6 ms por quadro para o mesmo desenho.
 */
#define NET_DOWNSCALE 2      /* a mascara tem 1/2 da resolucao da tela */

typedef struct {
    guchar *data;            /* pixels da mascara (pertencem a superficie) */
    int     stride, w, h;
} NetMask;

static inline void
mask_over (NetMask *m, int x, int y, double a)
{
    guchar *p;

    if (a <= 0 || x < 0 || y < 0 || x >= m->w || y >= m->h)
        return;
    p = m->data + (gsize) y * m->stride + x;
    *p = (guchar) (*p + (255 - *p) * MIN (a, 1.0));
}

/* cobertura de um pixel a distancia d do eixo de um traco de meia largura hw */
#define COVER(hw, d) CLAMP ((hw) + 0.5 - (d), 0.0, 1.0)

static void
mask_line (NetMask *m, double x0, double y0, double x1, double y1, double width, double alpha)
{
    double dx = x1 - x0, dy = y1 - y0, len = hypot (dx, dy);
    double hw = width / 2;
    int    pad = (int) ceil (hw + 1);

    if (len < 1e-6)
        return;
    if (fabs (dx) >= fabs (dy)) {          /* anda em x, espalha em y */
        double slope, k = fabs (dx) / len;
        if (x0 > x1) {
            double t;
            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
        }
        slope = (y1 - y0) / (x1 - x0);
        for (int x = MAX ((int) floor (x0), 0); x <= MIN ((int) ceil (x1), m->w - 1); x++) {
            double yc = y0 + (CLAMP (x + 0.5, x0, x1) - x0) * slope;
            for (int y = (int) floor (yc) - pad; y <= (int) floor (yc) + pad; y++)
                mask_over (m, x, y, COVER (hw, fabs (y + 0.5 - yc) * k) * alpha);
        }
    } else {                               /* anda em y, espalha em x */
        double slope, k = fabs (dy) / len;
        if (y0 > y1) {
            double t;
            t = x0; x0 = x1; x1 = t;
            t = y0; y0 = y1; y1 = t;
        }
        slope = (x1 - x0) / (y1 - y0);
        for (int y = MAX ((int) floor (y0), 0); y <= MIN ((int) ceil (y1), m->h - 1); y++) {
            double xc = x0 + (CLAMP (y + 0.5, y0, y1) - y0) * slope;
            for (int x = (int) floor (xc) - pad; x <= (int) floor (xc) + pad; x++)
                mask_over (m, x, y, COVER (hw, fabs (x + 0.5 - xc) * k) * alpha);
        }
    }
}

static void
mask_dot (NetMask *m, double cx, double cy, double r, double alpha)
{
    int pad = (int) ceil (r + 1);

    for (int y = (int) floor (cy) - pad; y <= (int) floor (cy) + pad; y++)
        for (int x = (int) floor (cx) - pad; x <= (int) floor (cx) + pad; x++)
            mask_over (m, x, y, COVER (r, hypot (x + 0.5 - cx, y + 0.5 - cy)) * alpha);
}

/* linhas e pontos da rede; k converte unidades logicas em pixels da mascara */
static void
draw_net (NetMask *m, double k, double alpha)
{
    memset (m->data, 0, (gsize) m->stride * m->h);

    for (int i = 0; i < st.n_nodes; i++) {
        for (int j = i + 1; j < st.n_nodes; j++) {
            double d = hypot (st.nodes[i].x - st.nodes[j].x, st.nodes[i].y - st.nodes[j].y);
            if (d < LINK_DIST)
                mask_line (m, st.nodes[i].x * k, st.nodes[i].y * k,
                           st.nodes[j].x * k, st.nodes[j].y * k,
                           MAX (k, 1.0), (1 - d / LINK_DIST) * 0.5 * alpha);
        }
    }
    for (int i = 0; i < st.n_nodes; i++)
        mask_dot (m, st.nodes[i].x * k, st.nodes[i].y * k, 2.0 * k, 0.8 * alpha);
}

#define PULSE_R 3.0

static void
pulse_pos (const Pulse *q, double *x, double *y)
{
    *x = st.nodes[q->a].x + (st.nodes[q->b].x - st.nodes[q->a].x) * q->k;
    *y = st.nodes[q->a].y + (st.nodes[q->b].y - st.nodes[q->a].y) * q->k;
}

/* pulsos brancos que correm pelas ligacoes */
static void
draw_pulses (cairo_t *cr, double alpha)
{
    for (int i = 0; i < N_PULSES; i++) {
        double x, y;

        if (st.pulses[i].b < 0)
            continue;
        pulse_pos (&st.pulses[i], &x, &y);
        cairo_new_sub_path (cr);
        cairo_arc (cr, x, y, PULSE_R, 0, 2 * G_PI);
    }
    cairo_set_source_rgba (cr, 1, 1, 1, alpha);
    cairo_fill (cr);
}

/* As duas fontes do painel, criadas uma vez e reaproveitadas em todo texto. */
static cairo_font_face_t *font_faces[2];

static void
set_font (cairo_t *cr, gboolean bold, double size)
{
    int i = bold ? 1 : 0;

    if (font_faces[i] == NULL)
        font_faces[i] = cairo_toy_font_face_create ("Sans", CAIRO_FONT_SLANT_NORMAL,
                                                    bold ? CAIRO_FONT_WEIGHT_BOLD
                                                         : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_face (cr, font_faces[i]);
    cairo_set_font_size (cr, size);
}

/* escreve e devolve o avanco horizontal do texto */
static double
text (cairo_t *cr, const char *s, double x, double y, double size,
      gboolean bold, gboolean right)
{
    cairo_text_extents_t ext;

    set_font (cr, bold, size);
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
          gboolean bold, gboolean right, double maxw)
{
    cairo_text_extents_t ext;

    if (maxw <= 0)
        return;
    set_font (cr, bold, size);
    cairo_text_extents (cr, s, &ext);
    if (ext.x_advance > maxw)
        size *= maxw / ext.x_advance;
    text (cr, s, x, y, size, bold, right);
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

    /* ponto de agora; o halo que respira em volta dele sai em draw_halos() */
    if (n_halos < (int) G_N_ELEMENTS (halos))
        halos[n_halos++] = (Halo) { ex, ey, up };
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
    text_fit (cr, rotulo, x, 50, 14, FALSE, FALSE, w);

    if (!q->have_data) {
        cairo_set_source_rgb (cr, 1, 1, 1);
        text_fit (cr, vazio, x, 120, 64, TRUE, FALSE, w);
        cairo_set_source_rgba (cr, 1, 1, 1, 0.6);
        text_fit (cr, "aguardando a primeira cotação", x, 146, 14, FALSE, FALSE, w);
    } else {
        double adv;

        cairo_set_source_rgb (cr, 1, 1, 1);
        text_fit (cr, valor, x, 120, 64, TRUE, FALSE, w);

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

#define HALO_R 7.5

static void
draw_halos (cairo_t *cr)
{
    double r = HALO_R + 1.5 * sin (st.t * 2 * G_PI / 3.3);

    for (int i = 0; i < n_halos; i++) {
        set_trend_color (cr, halos[i].up, 0.20);
        cairo_arc (cr, halos[i].x, halos[i].y, r, 0, 2 * G_PI);
        cairo_fill (cr);
    }
}

static void
draw_panel (cairo_t *cr, double lw)
{
    double     left = 48, right = lw - 48;
    double     colw = (right - left - COL_GAP) / 2;
    GDateTime *now = g_date_time_new_now_local ();
    gint64     agora = g_date_time_to_unix (now);
    char      *v, *s;

    n_halos = 0;
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
            text_fit (cr, l->str, right, 324, 13, FALSE, TRUE, colw);

            g_string_printf (l, "Hoje  máx %ld°  mín %ld°", lround (w->max), lround (w->min));
            if (w->chuva >= 0)
                g_string_append_printf (l, "  ·  chuva %d%%", w->chuva);
            cairo_set_source_rgba (cr, 1, 1, 1, 0.6);
            text_fit (cr, l->str, right, 339, 12, FALSE, TRUE, colw);
            g_string_free (l, TRUE);
        }
    }

    g_date_time_unref (now);
}

/*
 * O computador esta gastando bateria? Vale a bateria do sistema descarregando;
 * baterias de perifericos (mouse, teclado: scope=Device) nao contam. Sem
 * bateria nenhuma (desktop), a resposta e nao.
 */
static gboolean
on_battery (void)
{
    const char *base = g_getenv ("GCLOUD_DOLAR_POWER_DIR");
    GDir       *dir;
    const char *name;
    gboolean    discharging = FALSE;

    if (base == NULL || *base == '\0')
        base = "/sys/class/power_supply";
    dir = g_dir_open (base, 0, NULL);
    if (dir == NULL)
        return FALSE;
    while (!discharging && (name = g_dir_read_name (dir)) != NULL) {
        char *f, *type = NULL, *scope = NULL, *status = NULL;

        f = g_build_filename (base, name, "type", NULL);
        g_file_get_contents (f, &type, NULL, NULL);
        g_free (f);
        f = g_build_filename (base, name, "scope", NULL);
        g_file_get_contents (f, &scope, NULL, NULL);
        g_free (f);
        f = g_build_filename (base, name, "status", NULL);
        g_file_get_contents (f, &status, NULL, NULL);
        g_free (f);

        if (type != NULL && g_str_has_prefix (type, "Battery") &&
            (scope == NULL || !g_str_has_prefix (scope, "Device")) &&
            status != NULL && g_str_has_prefix (status, "Discharging"))
            discharging = TRUE;
        g_free (type);
        g_free (scope);
        g_free (status);
    }
    g_dir_close (dir);
    return discharging;
}

/* taxa de quadros do momento; a fonte de energia e conferida de tempos em tempos */
static int
target_fps (void)
{
    static int    fixed = -1, fps = FPS_AC;
    static gint64 checked;
    gint64        now = g_get_monotonic_time ();

    if (fixed < 0) {
        const char *e = g_getenv ("GCLOUD_DOLAR_FPS");
        fixed = e != NULL && *e != '\0' ? CLAMP (atoi (e), 1, 60) : 0;
    }
    if (fixed > 0)
        return fixed;
    if (checked == 0 || now - checked >= POWER_CHECK_SECONDS * G_USEC_PER_SEC) {
        fps = on_battery () ? FPS_BATTERY : FPS_AC;
        checked = now;
    }
    return fps;
}

/* o painel guardado ainda serve para este tamanho e este minuto? */
static gboolean
panel_fresh (int w, int h)
{
    return panel_cache != NULL && !panel_dirty && w == panel_w && h == panel_h &&
           panel_minute == g_get_real_time () / (60 * G_USEC_PER_SEC);
}

/* superficie do painel: refeita so quando ha dado novo, o minuto virou ou o tamanho mudou */
static void
update_panel (cairo_surface_t *like, const Layout *l)
{
    cairo_t *pc;

    if (panel_fresh (l->w, l->h))
        return;
    if (panel_cache != NULL && (l->w != panel_w || l->h != panel_h)) {
        cairo_surface_destroy (panel_cache);
        panel_cache = NULL;
    }
    if (panel_cache == NULL)
        panel_cache = cairo_surface_create_similar (like, CAIRO_CONTENT_COLOR_ALPHA, l->w, l->h);

    /* mesmo tamanho: reaproveita a superficie, so limpa e redesenha */
    pc = cairo_create (panel_cache);
    cairo_set_operator (pc, CAIRO_OPERATOR_CLEAR);
    cairo_paint (pc);
    cairo_set_operator (pc, CAIRO_OPERATOR_OVER);
    cairo_scale (pc, l->scale, l->scale);
    cairo_translate (pc, 0, l->oy);
    draw_panel (pc, l->lw);
    cairo_destroy (pc);
    panel_w = l->w;
    panel_h = l->h;
    panel_minute = g_get_real_time () / (60 * G_USEC_PER_SEC);
    panel_dirty = FALSE;
}

/*
 * Quem pinta o que:
 *  - fundo, pulsos, halos e a copia do painel: o servidor X, que faz isso barato;
 *  - rede de particulas: aqui no processo, numa mascara de 1 byte por pixel e
 *    metade da resolucao, que o servidor amplia e pinta com a cor da rede.
 *    Linhas com suavizacao sao caras no servidor, e mandar uma imagem colorida
 *    do tamanho da tela a cada quadro tambem; a mascara pesa 1/16 disso e, na
 *    rede de fundo, a meia resolucao quase nao se nota.
 *
 * Tudo aqui e alocado uma vez por tamanho de tela e reaproveitado a cada
 * quadro; frame_free() devolve.
 */
static struct {
    Layout           l;
    cairo_surface_t *net;         /* mascara A8 da rede */
    cairo_pattern_t *net_pat;     /* a mesma, ja com a escala de ampliacao */
    NetMask          mask;
    int              dx, dy;      /* deriva do painel, em pixels */
    gboolean         ready;       /* a mascara corresponde ao estado atual */
} fr;

static void
frame_free (void)
{
    g_clear_pointer (&fr.net_pat, cairo_pattern_destroy);
    g_clear_pointer (&fr.net, cairo_surface_destroy);
    g_clear_pointer (&panel_cache, cairo_surface_destroy);
    g_clear_pointer (&font_faces[0], cairo_font_face_destroy);
    g_clear_pointer (&font_faces[1], cairo_font_face_destroy);
    fr.mask.data = NULL;
    fr.ready = FALSE;
}

/* medidas do quadro e rede de particulas para este tamanho de area */
static void
scene_resize (int w, int h)
{
    layout_for (&fr.l, w, h);
    if (st.n_nodes == 0 || fabs (fr.l.lw - st.logical_w) > 1.0 ||
        fabs (fr.l.lh - st.logical_h) > 1.0)
        init_net (fr.l.lw, fr.l.lh);
}

/* deriva lenta do painel: evita imagem parada no mesmo ponto do monitor.
 * Em pixels inteiros, para a copia ser direta e o texto nao borrar. */
static void
scene_drift (void)
{
    fr.dx = (int) round (10.0 * sin (st.t / 30.0) * fr.l.scale);
    fr.dy = (int) round (6.0 * cos (st.t / 23.3) * fr.l.scale);
}

/* (re)aloca a mascara da rede quando o tamanho da area muda (so no cairo) */
static void
frame_resize (int w, int h)
{
    cairo_matrix_t m;
    int            nw = (w + NET_DOWNSCALE - 1) / NET_DOWNSCALE;
    int            nh = (h + NET_DOWNSCALE - 1) / NET_DOWNSCALE;

    if (fr.net != NULL && fr.l.w == w && fr.l.h == h)
        return;

    g_clear_pointer (&fr.net_pat, cairo_pattern_destroy);
    g_clear_pointer (&fr.net, cairo_surface_destroy);

    scene_resize (w, h);
    fr.net = cairo_image_surface_create (CAIRO_FORMAT_A8, nw, nh);
    fr.net_pat = cairo_pattern_create_for_surface (fr.net);
    cairo_matrix_init_scale (&m, 1.0 / NET_DOWNSCALE, 1.0 / NET_DOWNSCALE);
    cairo_pattern_set_matrix (fr.net_pat, &m);
    cairo_pattern_set_filter (fr.net_pat, CAIRO_FILTER_BILINEAR);

    fr.mask.data = cairo_image_surface_get_data (fr.net);
    fr.mask.stride = cairo_image_surface_get_stride (fr.net);
    fr.mask.w = nw;
    fr.mask.h = nh;
    fr.ready = FALSE;
}

/* rasteriza a rede do estado atual na mascara e calcula a deriva do painel */
static void
frame_update (int w, int h)
{
    frame_resize (w, h);
    if (cairo_surface_status (fr.net) != CAIRO_STATUS_SUCCESS)
        return;

    scene_drift ();
    cairo_surface_flush (fr.net);
    draw_net (&fr.mask, fr.l.scale / NET_DOWNSCALE, NET_ALPHA);
    cairo_surface_mark_dirty (fr.net);
    fr.ready = TRUE;
}

/* pinta o quadro inteiro */
static void
frame_paint (cairo_t *cr)
{
    const Layout *l = &fr.l;

    update_panel (cairo_get_target (cr), l);

    cairo_set_source_rgb (cr, BG_R, BG_G, BG_B);
    cairo_paint (cr);

    cairo_set_source_rgb (cr, AC_R, AC_G, AC_B);
    cairo_mask (cr, fr.net_pat);

    cairo_save (cr);
    cairo_scale (cr, l->scale, l->scale);
    draw_pulses (cr, NET_ALPHA);
    cairo_translate (cr, fr.dx / l->scale, fr.dy / l->scale + l->oy);
    draw_halos (cr);
    cairo_restore (cr);

    cairo_set_source_surface (cr, panel_cache, fr.dx, fr.dy);
    cairo_paint (cr);
}

static gboolean
on_draw (GtkWidget *widget, cairo_t *cr, gpointer data)
{
    int w = gtk_widget_get_allocated_width (widget);
    int h = gtk_widget_get_allocated_height (widget);

    /* primeira exposicao ou mudanca de tamanho antes do proximo quadro */
    if (!fr.ready || fr.l.w != w || fr.l.h != h)
        frame_update (w, h);
    if (fr.ready)
        frame_paint (cr);
    return TRUE;
}

/* ------------------------------------------------------------------------
 * Desenhista de reserva: GTK + cairo
 * ------------------------------------------------------------------------ */

static gboolean
on_frame (gpointer data)
{
    static gint64 last;
    GtkWidget    *area = GTK_WIDGET (data);
    gint64        now = g_get_monotonic_time ();
    int           w = gtk_widget_get_allocated_width (area);
    int           h = gtk_widget_get_allocated_height (area);

    /* depois de uma pausa longa (suspensao), nao da um salto na animacao */
    step (last != 0 ? CLAMP ((double) (now - last) / 1e6, 0.0, 0.1) : 0.0);
    last = now;

    if (gtk_widget_get_mapped (area) && w > 1 && h > 1) {
        frame_update (w, h);
        gtk_widget_queue_draw (area);
    }
    /* reagenda a cada quadro: a taxa muda quando a fonte de energia muda */
    g_timeout_add ((guint) (1000 / target_fps ()), on_frame, data);
    return G_SOURCE_REMOVE;
}

static gboolean
on_reload (gpointer data)
{
    load_data ();
    return G_SOURCE_CONTINUE;
}

/* o xfce4-screensaver encerra o tema com SIGTERM: sai pelo caminho normal */
static gboolean
on_signal (gpointer data)
{
    gtk_main_quit ();
    return G_SOURCE_REMOVE;
}

static int
run_gtk (int *argc, char ***argv)
{
    GtkWidget  *window;
    GtkWidget  *area;
    const char *wid;

    gtk_init (argc, argv);
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
    g_timeout_add (1, on_frame, area);
    g_timeout_add_seconds (RELOAD_SECONDS, on_reload, NULL);
    g_unix_signal_add (SIGTERM, on_signal, NULL);
    g_unix_signal_add (SIGINT, on_signal, NULL);

    gtk_widget_show_all (window);
    gtk_main ();
    frame_free ();
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------------
 * Desenhista padrao: OpenGL (GLX), direto no Xlib
 *
 * A placa de video pinta fundo, rede, pulsos e halos. O painel e desenhado
 * pelo cairo numa imagem e vira uma textura, reenviada so quando o conteudo
 * muda. Medido em tres monitores a 24 quadros por segundo: 31% de um nucleo,
 * contra 65% do desenhista GTK. O preco e memoria: ~90 MB por processo
 * (as bibliotecas do OpenGL), contra ~32 MB.
 * ------------------------------------------------------------------------ */

static volatile sig_atomic_t gl_quit;

static void
gl_on_signal (int sig)
{
    gl_quit = 1;
}

/* erro do servidor X (a janela do screensaver sumiu, por exemplo): encerra
 * com calma em vez de deixar o Xlib abortar o processo */
static int
gl_on_xerror (Display *dpy, XErrorEvent *ev)
{
    gl_quit = 1;
    return 0;
}

static void
gl_circle (double cx, double cy, double r)
{
    glBegin (GL_TRIANGLE_FAN);
    glVertex2d (cx, cy);
    for (int i = 0; i <= 20; i++)
        glVertex2d (cx + r * cos (i * G_PI / 10), cy + r * sin (i * G_PI / 10));
    glEnd ();
}

/* um quadro; tex e a textura do painel, like serve de molde para a imagem dele */
static void
gl_paint (int w, int h, cairo_surface_t *like)
{
    const Layout *l;
    double        hr;

    scene_resize (w, h);
    scene_drift ();
    l = &fr.l;

    if (!panel_fresh (w, h)) {
        update_panel (like, l);
        cairo_surface_flush (panel_cache);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, cairo_image_surface_get_stride (panel_cache) / 4);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                      cairo_image_surface_get_data (panel_cache));
    }

    glViewport (0, 0, w, h);
    glClearColor ((float) BG_R, (float) BG_G, (float) BG_B, 1);
    glClear (GL_COLOR_BUFFER_BIT);

    /* cena em unidades logicas, com y para baixo como no cairo */
    glMatrixMode (GL_PROJECTION);
    glLoadIdentity ();
    glOrtho (0, l->lw, l->lh, 0, -1, 1);
    glDisable (GL_TEXTURE_2D);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glLineWidth ((float) MAX (l->scale, 1.0));
    glBegin (GL_LINES);
    for (int i = 0; i < st.n_nodes; i++) {
        for (int j = i + 1; j < st.n_nodes; j++) {
            double d = hypot (st.nodes[i].x - st.nodes[j].x, st.nodes[i].y - st.nodes[j].y);
            if (d < LINK_DIST) {
                glColor4d (AC_R, AC_G, AC_B, (1 - d / LINK_DIST) * 0.5 * NET_ALPHA);
                glVertex2d (st.nodes[i].x, st.nodes[i].y);
                glVertex2d (st.nodes[j].x, st.nodes[j].y);
            }
        }
    }
    glEnd ();

    glColor4d (AC_R, AC_G, AC_B, 0.8 * NET_ALPHA);
    for (int i = 0; i < st.n_nodes; i++)
        gl_circle (st.nodes[i].x, st.nodes[i].y, 2.0);

    glColor4d (1, 1, 1, NET_ALPHA);
    for (int i = 0; i < N_PULSES; i++) {
        double x, y;

        if (st.pulses[i].b < 0)
            continue;
        pulse_pos (&st.pulses[i], &x, &y);
        gl_circle (x, y, PULSE_R);
    }

    hr = HALO_R + 1.5 * sin (st.t * 2 * G_PI / 3.3);
    for (int i = 0; i < n_halos; i++) {
        if (halos[i].up)
            glColor4d (UP_R, UP_G, UP_B, 0.20);
        else
            glColor4d (DN_R, DN_G, DN_B, 0.20);
        gl_circle (halos[i].x + fr.dx / l->scale, halos[i].y + l->oy + fr.dy / l->scale, hr);
    }

    /* painel: textura com alfa pre-multiplicado, posta em pixels inteiros */
    glLoadIdentity ();
    glOrtho (0, w, h, 0, -1, 1);
    glEnable (GL_TEXTURE_2D);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4d (1, 1, 1, 1);
    glBegin (GL_QUADS);
    glTexCoord2d (0, 0); glVertex2d (fr.dx, fr.dy);
    glTexCoord2d (1, 0); glVertex2d (fr.dx + w, fr.dy);
    glTexCoord2d (1, 1); glVertex2d (fr.dx + w, fr.dy + h);
    glTexCoord2d (0, 1); glVertex2d (fr.dx, fr.dy + h);
    glEnd ();
}

/* espera ate o instante do proximo quadro, acordando se chegar evento do X */
static void
gl_wait (Display *dpy, gint64 until)
{
    while (!gl_quit && XPending (dpy) == 0) {
        gint64        now = g_get_monotonic_time ();
        struct pollfd pfd = { ConnectionNumber (dpy), POLLIN, 0 };

        if (now >= until)
            return;
        poll (&pfd, 1, (int) ((until - now + 999) / 1000));
    }
}

/*
 * Devolve o codigo de saida, ou -1 se nao deu para usar OpenGL (sem servidor
 * X, sem GLX...). Nesse caso nada foi mostrado e o chamador usa o GTK.
 */
static int
run_gl (void)
{
    static const int base[] = { GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
                                GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                                GLX_BLUE_SIZE, 8 };
    int                  attrs[G_N_ELEMENTS (base) + 5];
    Display             *dpy = XOpenDisplay (NULL);
    const char          *wid = g_getenv ("XSCREENSAVER_WINDOW");
    gboolean             embedded = wid != NULL && *wid != '\0';
    gboolean             smooth = FALSE;
    Window               parent, win;
    XWindowAttributes    pa;
    XSetWindowAttributes swa;
    GLXFBConfig         *cfgs = NULL;
    GLXFBConfig          cfg;
    XVisualInfo         *vi = NULL;
    GLXContext           ctx = NULL;
    Atom                 wm_delete, xembed;
    unsigned long        xembed_data[2] = { 0, 1 /* XEMBED_MAPPED */ };
    cairo_surface_t     *like;
    GLuint               tex = 0;
    int                  n = 0, w, h, major, minor;
    gint64               next, last = 0, last_reload;
    void               (*swap_interval) (Display *, GLXDrawable, int);

    if (dpy == NULL)
        return -1;
    XSetErrorHandler (gl_on_xerror);
    if (!glXQueryVersion (dpy, &major, &minor) || (major == 1 && minor < 3))
        goto unavailable;

    /* de preferencia com 4 amostras por pixel, que suavizam linhas e circulos */
    memcpy (attrs, base, sizeof base);
    attrs[G_N_ELEMENTS (base)] = GLX_SAMPLE_BUFFERS;
    attrs[G_N_ELEMENTS (base) + 1] = 1;
    attrs[G_N_ELEMENTS (base) + 2] = GLX_SAMPLES;
    attrs[G_N_ELEMENTS (base) + 3] = 4;
    attrs[G_N_ELEMENTS (base) + 4] = None;
    cfgs = glXChooseFBConfig (dpy, DefaultScreen (dpy), attrs, &n);
    if (cfgs == NULL || n == 0) {
        if (cfgs != NULL)
            XFree (cfgs);
        attrs[G_N_ELEMENTS (base)] = None;
        cfgs = glXChooseFBConfig (dpy, DefaultScreen (dpy), attrs, &n);
        smooth = TRUE;                 /* sem multiamostragem: suaviza as linhas */
    }
    if (cfgs == NULL || n == 0)
        goto unavailable;
    cfg = cfgs[0];
    vi = glXGetVisualFromFBConfig (dpy, cfg);
    if (vi == NULL)
        goto unavailable;
    ctx = glXCreateNewContext (dpy, cfg, GLX_RGBA_TYPE, NULL, True);
    if (ctx == NULL)
        goto unavailable;

    /* janela-mae: a do screensaver ou, em teste, uma janela comum */
    if (embedded) {
        parent = (Window) strtoul (wid, NULL, 0);
        if (!XGetWindowAttributes (dpy, parent, &pa))
            goto unavailable;
        w = pa.width;
        h = pa.height;
        XSelectInput (dpy, parent, StructureNotifyMask);
    } else {
        parent = DefaultRootWindow (dpy);
        w = 1020;
        h = 540;
    }

    swa.colormap = XCreateColormap (dpy, parent, vi->visual, AllocNone);
    swa.border_pixel = 0;
    swa.background_pixel = 0;
    swa.event_mask = StructureNotifyMask;
    win = XCreateWindow (dpy, parent, 0, 0, (unsigned) w, (unsigned) h, 0, vi->depth,
                         InputOutput, vi->visual,
                         CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &swa);
    wm_delete = XInternAtom (dpy, "WM_DELETE_WINDOW", False);
    if (embedded) {
        /* avisa o GtkSocket do screensaver que a janela quer aparecer (XEMBED) */
        xembed = XInternAtom (dpy, "_XEMBED_INFO", False);
        XChangeProperty (dpy, win, xembed, xembed, 32, PropModeReplace,
                         (unsigned char *) xembed_data, 2);
    } else {
        XStoreName (dpy, win, "GCLOUD - dolar (teste)");
        XSetWMProtocols (dpy, win, &wm_delete, 1);
    }
    XMapWindow (dpy, win);

    if (!glXMakeCurrent (dpy, win, ctx)) {
        XDestroyWindow (dpy, win);
        XFreeColormap (dpy, swa.colormap);
        goto unavailable;
    }
    /* quem marca o ritmo e o relogio daqui, nao a troca de quadros da placa */
    swap_interval = (void (*) (Display *, GLXDrawable, int))
                    glXGetProcAddressARB ((const GLubyte *) "glXSwapIntervalEXT");
    if (swap_interval != NULL &&
        strstr (glXQueryExtensionsString (dpy, DefaultScreen (dpy)), "GLX_EXT_swap_control") != NULL)
        swap_interval (dpy, win, 0);

    glEnable (GL_BLEND);
    if (smooth) {
        glEnable (GL_LINE_SMOOTH);
        glHint (GL_LINE_SMOOTH_HINT, GL_NICEST);
    } else {
        glEnable (GL_MULTISAMPLE);
    }
    glGenTextures (1, &tex);
    glBindTexture (GL_TEXTURE_2D, tex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glMatrixMode (GL_PROJECTION);
    like = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);

    signal (SIGTERM, gl_on_signal);
    signal (SIGINT, gl_on_signal);
    load_data ();
    next = last_reload = g_get_monotonic_time ();

    while (!gl_quit) {
        gint64 now, period;

        while (XPending (dpy) > 0) {
            XEvent ev;

            XNextEvent (dpy, &ev);
            if (ev.type == ConfigureNotify) {
                if (ev.xconfigure.window == parent && embedded)
                    XResizeWindow (dpy, win, (unsigned) ev.xconfigure.width,
                                   (unsigned) ev.xconfigure.height);
                if (ev.xconfigure.window == win || embedded) {
                    w = ev.xconfigure.width;
                    h = ev.xconfigure.height;
                }
            } else if (ev.type == DestroyNotify ||
                       (ev.type == ClientMessage && (Atom) ev.xclient.data.l[0] == wm_delete)) {
                gl_quit = 1;
            }
        }
        now = g_get_monotonic_time ();
        if (now < next) {
            gl_wait (dpy, next);
            continue;
        }
        period = G_USEC_PER_SEC / target_fps ();
        next += period;
        if (next < now)                 /* atrasou (suspensao, sistema ocupado): recomeca */
            next = now + period;

        if (now - last_reload >= RELOAD_SECONDS * G_USEC_PER_SEC) {
            load_data ();
            last_reload = now;
        }
        /* depois de uma pausa longa (suspensao), nao da um salto na animacao */
        step (last != 0 ? CLAMP ((double) (now - last) / 1e6, 0.0, 0.1) : 0.0);
        last = now;

        if (w > 1 && h > 1) {
            gl_paint (w, h, like);
            glXSwapBuffers (dpy, win);
        }
    }

    cairo_surface_destroy (like);
    frame_free ();
    cairo_debug_reset_static_data ();   /* aqui nada mais do cairo esta vivo */
    glDeleteTextures (1, &tex);
    glXMakeCurrent (dpy, None, NULL);
    glXDestroyContext (dpy, ctx);
    XDestroyWindow (dpy, win);
    XFreeColormap (dpy, swa.colormap);
    XFree (vi);
    XFree (cfgs);
    XCloseDisplay (dpy);
    return EXIT_SUCCESS;

unavailable:
    if (ctx != NULL)
        glXDestroyContext (dpy, ctx);
    if (vi != NULL)
        XFree (vi);
    if (cfgs != NULL)
        XFree (cfgs);
    XSetErrorHandler (NULL);
    XCloseDisplay (dpy);
    gl_quit = 0;
    return -1;
}

/* ------------------------------------------------------------------------ */

static int
snapshot (const char *file, int w, int h)
{
    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_RGB24, w, h);
    cairo_t         *cr = cairo_create (surf);
    cairo_status_t   status;

    load_data ();
    frame_resize (w, h);
    for (int i = 0; i < 200; i++)
        step (1.0 / 30);
    frame_update (w, h);
    if (fr.ready)
        frame_paint (cr);
    status = cairo_surface_write_to_png (surf, file);
    cairo_destroy (cr);
    cairo_surface_destroy (surf);
    frame_free ();
    /* nada do cairo continua vivo aqui: devolve tambem os caches internos dele,
     * para um verificador de vazamento terminar limpo */
    cairo_debug_reset_static_data ();
    return status == CAIRO_STATUS_SUCCESS ? EXIT_SUCCESS : EXIT_FAILURE;
}

int
main (int argc, char **argv)
{
    const char *render = g_getenv ("GCLOUD_DOLAR_RENDER");

    if (argc >= 3 && g_str_equal (argv[1], "--snapshot")) {
        int w = argc >= 5 ? atoi (argv[3]) : 1360;
        int h = argc >= 5 ? atoi (argv[4]) : 720;
        return snapshot (argv[2], CLAMP (w, 320, 16384), CLAMP (h, 180, 16384));
    }

    if (g_strcmp0 (render, "gtk") != 0) {
        int rc = run_gl ();

        if (rc >= 0)
            return rc;
        if (g_strcmp0 (render, "gl") == 0) {
            g_printerr ("gcloud-dolar: OpenGL indisponivel\n");
            return EXIT_FAILURE;
        }
    }
    return run_gtk (&argc, &argv);
}
