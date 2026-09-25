/*
 * Output test: draws frames with the real output code, then replays the
 * bytes through a tiny terminal emulator and checks that after every frame
 * the emulated screen shows exactly the cells planets meant to draw. Covers
 * the changed-cells-only redraw, cursor jumps, color codes, mode switches
 * and resizes.
 */
#define main planets_main
#include "../planets.c"
#undef main
#include <ctype.h>

#define MAXF 600
#define EW 400                      /* emulated screen, larger than any test size */
#define EH 200
#define DEF 0xFFFFFFFEu             /* the terminal's default color */

static Cell *g_snap[MAXF];
static int g_snap_w[MAXF], g_snap_h[MAXF], g_nsnap;

typedef struct { uint32_t fg, bg, ch; } ECell;
static ECell grid[EH][EW];
static long errors;

static void setup(int cols, int rows, int ascii)
{
    g_ascii = ascii;
    g_cols = cols;
    g_rows = rows;
    g_pw = cols;
    g_ph = ascii ? rows : rows * 2;
    g_yasp = ascii ? 0.5f : 1.0f;
    g_focal = fminf((float)g_pw, g_ph / g_yasp);
    size_t np = (size_t)g_pw * g_ph, nc = (size_t)cols * rows;
    g_color = xrealloc(g_color, np * sizeof *g_color);
    g_color2 = xrealloc(g_color2, np * sizeof *g_color2);
    g_id = xrealloc(g_id, np * sizeof *g_id);
    g_cells = xrealloc(g_cells, nc * sizeof *g_cells);
    g_prev = xrealloc(g_prev, nc * sizeof *g_prev);
    g_full_redraw = 1;
}

/* draw one frame and remember what it was supposed to look like */
static void frame(int scene, float t, float fade, int with_hud)
{
    render_scene(scene, t);
    g_nspans = 0;
    if (with_hud) {
        hud(1, 1, v3(1, 1, 1), 0.7f, " 3/9  EARTH ");
        hud(1, -2, v3(0.7f, 0.8f, 1.0f), 0.4f, " our home - 71% of the surface is ocean ");
    }
    present(fade);
    size_t nc = (size_t)g_cols * g_rows;
    g_snap[g_nsnap] = xrealloc(NULL, nc * sizeof(Cell));
    memcpy(g_snap[g_nsnap], g_cells, nc * sizeof(Cell));
    g_snap_w[g_nsnap] = g_cols;
    g_snap_h[g_nsnap] = g_rows;
    g_nsnap++;
}

static void fail(const char *what, long pos)
{
    if (errors++ < 8)
        fprintf(stderr, "ERROR at byte %ld: %s\n", pos, what);
}

static void compare(int k)
{
    int w = g_snap_w[k], h = g_snap_h[k], shown = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            Cell e = g_snap[k][y * w + x];
            ECell g = grid[y][x];
            if (g.ch != e.ch || g.bg != e.bg || (e.fg != FG_ANY && g.fg != e.fg)) {
                if (shown++ < 3)
                    fprintf(stderr, "frame %d cell %d,%d: want ch=%u fg=%x bg=%x, got ch=%u fg=%x bg=%x\n",
                            k, x, y, e.ch, e.fg, e.bg, g.ch, g.fg, g.bg);
                errors++;
            }
        }
}

/* SGR parameters: 0 resets, 38/48 set foreground/background (24-bit or palette) */
static void set_colors(const int *p, int np, uint32_t *fg, uint32_t *bg, long start)
{
    for (int q = 0; q < np;) {
        int v = p[q];
        if (v <= 0) {
            *fg = *bg = DEF;
            q++;
        } else if ((v == 38 || v == 48) && q + 4 < np && p[q + 1] == 2) {
            if (p[q + 2] > 255 || p[q + 3] > 255 || p[q + 4] > 255)
                fail("color out of range", start);
            uint32_t col = (uint32_t)(p[q + 2] << 16 | p[q + 3] << 8 | p[q + 4]);
            *(v == 38 ? fg : bg) = col;
            q += 5;
        } else if ((v == 38 || v == 48) && q + 2 < np && p[q + 1] == 5) {
            if (p[q + 2] > 255)
                fail("palette index out of range", start);
            *(v == 38 ? fg : bg) = (uint32_t)p[q + 2];
            q += 3;
        } else {
            fail("unknown color code", start);
            return;
        }
    }
}

static void replay(const unsigned char *b, long n)
{
    for (int y = 0; y < EH; y++)
        for (int x = 0; x < EW; x++)
            grid[y][x] = (ECell){ DEF, DEF, ' ' };
    uint32_t fg = DEF, bg = DEF;
    int x = 0, y = 0, k = 0;
    long i = 0;
    while (i < n) {
        unsigned char ch = b[i];
        if (ch == 0x1b) {
            long start = i, j = i + 2;
            if (b[i + 1] != '[') {
                fail("ESC without [", i);
                i++;
                continue;
            }
            int priv = b[j] == '?';
            if (priv)
                j++;
            int p[40], np = 0, cur = -1;
            while (j < n && (isdigit(b[j]) || b[j] == ';')) {
                if (b[j] == ';') {
                    if (np < 39)
                        p[np++] = cur;
                    cur = -1;
                } else {
                    cur = (cur < 0 ? 0 : cur) * 10 + (b[j] - '0');
                }
                j++;
            }
            p[np++] = cur;
            unsigned char fin = b[j];
            i = j + 1;
            if (priv) {
                if (fin == 'l' && p[0] == 2026) {       /* end of a frame */
                    if (k >= g_nsnap)
                        fail("more frames than were drawn", start);
                    else
                        compare(k);
                    k++;
                } else if (!(fin == 'h' && p[0] == 2026)) {
                    fail("unexpected private sequence", start);
                }
                continue;
            }
            switch (fin) {
            case 'm':
                set_colors(p, np, &fg, &bg, start);
                break;
            case 'H':
                y = (p[0] < 1 ? 1 : p[0]) - 1;
                x = (np > 1 && p[1] >= 1 ? p[1] : 1) - 1;
                break;
            case 'C':
                x += p[0] < 1 ? 1 : p[0];
                break;
            case 'J':
                if (p[0] != 2)
                    fail("unexpected erase", start);
                for (int yy = 0; yy < EH; yy++)
                    for (int xx = 0; xx < EW; xx++)
                        grid[yy][xx] = (ECell){ DEF, bg, ' ' };
                break;
            default:
                fail("unknown escape sequence", start);
            }
            continue;
        }

        uint32_t cp;
        int len;
        if (ch < 0x80) {
            cp = ch;
            len = 1;
        } else if ((ch & 0xE0) == 0xC0) {
            cp = (uint32_t)(ch & 0x1F) << 6 | (b[i + 1] & 0x3F);
            len = 2;
        } else if ((ch & 0xF0) == 0xE0) {
            cp = (uint32_t)(ch & 0x0F) << 12 | (uint32_t)(b[i + 1] & 0x3F) << 6 | (b[i + 2] & 0x3F);
            len = 3;
        } else {
            fail("bad UTF-8", i);
            i++;
            continue;
        }
        if (ch < 0x20)
            fail("control character in the output", i);
        int w = k < g_nsnap ? g_snap_w[k] : 0, h = k < g_nsnap ? g_snap_h[k] : 0;
        if (x < 0 || y < 0 || x >= w || y >= h)
            fail("drawing outside the screen", i);
        else
            grid[y][x] = (ECell){ fg, bg, cp };
        x++;
        i += len;
    }
    if (k != g_nsnap)
        fail("frame count does not match", n);
}

int main(void)
{
    static const struct { int cols, rows, ascii, c256; } cfg[] = {
        { 80, 24, 0, 0 }, { 80, 24, 0, 1 }, { 80, 24, 1, 0 }, { 37, 11, 0, 0 }, { 200, 50, 0, 0 },
        { 5, 2, 0, 0 }, { 1, 1, 0, 0 }, { 120, 34, 1, 1 }, { 160, 45, 0, 0 },
    };
    int ncfg = (int)(sizeof cfg / sizeof cfg[0]);

    init_tables();
    g_nthreads = MAXTHREADS;

    /* everything planets writes to stdout goes into a temporary file */
    char path[] = "/tmp/planets-output-XXXXXX";
    int fd = mkstemp(path), saved = dup(STDOUT_FILENO);
    if (fd < 0 || saved < 0) {
        perror("output test");
        return 2;
    }
    dup2(fd, STDOUT_FILENO);
    for (int c = 0; c < ncfg; c++) {
        setup(cfg[c].cols, cfg[c].rows, cfg[c].ascii);     /* like a resize */
        g_256 = cfg[c].c256;
        for (int f = 0; f < 40; f++) {
            if (f == 20) {          /* like pressing 'c' */
                g_256 = !g_256;
                g_full_redraw = 1;
            }
            frame((f / 6 + c) % NSCENES, f * 0.3f, f < 5 ? f / 5.0f : 1.0f, f % 3 == 0);
        }
    }
    dup2(saved, STDOUT_FILENO);
    close(saved);

    long n = lseek(fd, 0, SEEK_END);
    unsigned char *b = xrealloc(NULL, (size_t)n + 64);
    if (pread(fd, b, (size_t)n, 0) != n) {
        perror("output test");
        return 2;
    }
    memset(b + n, 0, 64);
    close(fd);
    unlink(path);

    replay(b, n);
    printf("output test: %d frames in %d terminal setups, %ld bytes replayed, %ld errors\n",
           g_nsnap, ncfg, n, errors);
    free(b);
    for (int s = 0; s < g_nsnap; s++)
        free(g_snap[s]);
    return errors != 0;
}
