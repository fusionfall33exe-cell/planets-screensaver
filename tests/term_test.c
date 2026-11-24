/*
 * Terminal test: the helpers that adapt planets to the terminal it runs in
 * (cell shape, color mode, charset), the text overlay on small screens, and
 * that planets come out round whatever the cell shape, and that output to a
 * non-blocking stdout is not lost.
 */
#define main planets_main
#include "../planets.c"
#undef main
#include <fcntl.h>
#include <sys/wait.h>

static int errors;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond);         \
            errors++;                                                       \
        }                                                                   \
    } while (0)

static int near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void test_cell_ratio(void)
{
    struct winsize ws = { .ws_row = 24, .ws_col = 80, .ws_xpixel = 720, .ws_ypixel = 456 };   /* 9x19 */
    CHECK(near(cell_ratio_from(&ws), 19.0f / 9.0f));
    ws.ws_xpixel = ws.ws_ypixel = 0;                    /* Windows Terminal, WSL: no pixel size */
    CHECK(cell_ratio_from(&ws) == 0);
    ws = (struct winsize){ .ws_row = 24, .ws_col = 80, .ws_xpixel = 8000, .ws_ypixel = 100 };
    CHECK(cell_ratio_from(&ws) == 0);                   /* implausible: tall cells of 0.1 */
    ws = (struct winsize){ .ws_row = 24, .ws_col = 80, .ws_xpixel = 80, .ws_ypixel = 24000 };
    CHECK(cell_ratio_from(&ws) == 0);                   /* implausible: 100 times taller than wide */
    ws = (struct winsize){ .ws_row = 0, .ws_col = 80, .ws_xpixel = 720, .ws_ypixel = 456 };
    CHECK(cell_ratio_from(&ws) == 0);                   /* no division by zero */
}

static void test_colors(void)
{
    CHECK(!color_256_by_default("truecolor", "xterm-256color", NULL, 0));
    CHECK(!color_256_by_default("24bit", "screen", "Apple_Terminal", 0));   /* its own claim wins */
    CHECK(!color_256_by_default(NULL, "xterm-256color", NULL, 0));          /* Windows Terminal in WSL */
    CHECK(!color_256_by_default("", "xterm-kitty", NULL, 0));
    CHECK(!color_256_by_default(NULL, "vte-256color", NULL, 0));            /* not mistaken for vt100 */
    CHECK(!color_256_by_default(NULL, NULL, NULL, 0));
    CHECK(color_256_by_default(NULL, "xterm-256color", "Apple_Terminal", 0));
    CHECK(color_256_by_default(NULL, "screen-256color", NULL, 0));          /* GNU screen */
    CHECK(!color_256_by_default(NULL, "screen-256color", NULL, 1));         /* tmux */
    CHECK(color_256_by_default(NULL, "rxvt-unicode-256color", NULL, 0));
    CHECK(color_256_by_default(NULL, "vt100", NULL, 0));
}

static void test_locale(void)
{
    CHECK(!locale_is_legacy(NULL, NULL, NULL));
    CHECK(!locale_is_legacy(NULL, NULL, "de_AT.UTF-8"));
    CHECK(!locale_is_legacy(NULL, NULL, "C.utf8"));
    CHECK(!locale_is_legacy(NULL, NULL, "C"));                  /* no charset named: do not guess */
    CHECK(!locale_is_legacy(NULL, NULL, "POSIX"));
    CHECK(locale_is_legacy(NULL, NULL, "de_AT.ISO-8859-1"));
    CHECK(locale_is_legacy(NULL, NULL, "ja_JP.eucJP"));
    CHECK(locale_is_legacy(NULL, NULL, "en_US.ISO-8859-15@euro"));
    CHECK(!locale_is_legacy("de_AT.UTF-8", NULL, "de_AT.ISO-8859-1"));   /* LC_ALL beats LANG */
    CHECK(locale_is_legacy("", "de_AT.ISO-8859-1", "de_AT.UTF-8"));      /* empty means unset */
}

static void test_hud(void)
{
    static const char fact[] = " rings reach 282,000 km out, but are only about 10 m thick ";
    V3 w = v3(1, 1, 1);

    g_cols = 80; g_rows = 24; g_nspans = 0;
    hud(1, -2, w, 1, fact);
    CHECK(g_nspans == 1 && g_spans[0].y == 22 && !strcmp(g_spans[0].text, fact));

    g_cols = 40; g_nspans = 0;                          /* narrower than the text: cut, fits the row */
    hud(1, -2, w, 1, fact);
    CHECK(g_nspans == 1 && (int)strlen(g_spans[0].text) == 38);
    CHECK(!strcmp(g_spans[0].text + 35, "..."));
    CHECK(g_spans[0].x + (int)strlen(g_spans[0].text) < g_cols);

    g_cols = 6; g_nspans = 0;                           /* no room for even a short label */
    hud(1, 1, w, 1, " 3/9  EARTH ");
    CHECK(g_nspans == 0);

    g_cols = 80; g_rows = 3; g_nspans = 0;              /* title and fact would share row 1 */
    hud(1, 1, w, 1, " 3/9  EARTH ");
    hud(1, -2, w, 1, fact);
    CHECK(g_nspans == 1 && !strcmp(g_spans[0].text, " 3/9  EARTH "));

    g_rows = 1; g_nspans = 0;                           /* row 1 is off screen */
    hud(1, 1, w, 1, " 3/9  EARTH ");
    CHECK(g_nspans == 0);
}

/* Earth (body 0) is round on screen when its width in cells equals its height in cells times the cell ratio */
static void test_round(void)
{
    static const float ratios[] = { 1.6f, 2.0f, 2.4f };
    init_tables();
    for (int ascii = 0; ascii <= 1; ascii++)
        for (size_t k = 0; k < sizeof ratios / sizeof ratios[0]; k++) {
            g_ascii = ascii;
            g_cell_ratio = ratios[k];
            set_size(100, 30);
            render_scene(2, 0);         /* EARTH */
            int x0 = g_pw, x1 = -1, y0 = g_ph, y1 = -1;
            for (int y = 0; y < g_ph; y++)
                for (int x = 0; x < g_pw; x++)
                    if (g_id[y * g_pw + x] == 0) {
                        x0 = x < x0 ? x : x0;
                        x1 = x > x1 ? x : x1;
                        y0 = y < y0 ? y : y0;
                        y1 = y > y1 ? y : y1;
                    }
            float w = (float)(x1 - x0 + 1), h = (float)(y1 - y0 + 1) / (ascii ? 1 : 2);   /* in cells */
            CHECK(x1 >= 0 && fabsf(w / (h * ratios[k]) - 1) < 0.05f);
        }
}

/* stdout made non-blocking (a parent shares it) with a slow reader: every byte still arrives */
static void test_nonblocking_write(void)
{
    static char big[1 << 20];
    int p[2], saved = dup(STDOUT_FILENO), status = -1;
    if (pipe(p) < 0 || saved < 0) {
        CHECK(!"pipe");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(p[1]);
        char b[4096];
        long got = 0;
        ssize_t r;
        while ((r = read(p[0], b, sizeof b)) > 0) {
            got += r;
            usleep(50);
        }
        _exit(got == (long)sizeof big ? 0 : 1);
    }
    close(p[0]);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    dup2(p[1], STDOUT_FILENO);
    close(p[1]);
    CHECK(write_all(big, sizeof big) == 0);
    dup2(saved, STDOUT_FILENO);         /* closes the pipe: the reader sees the end */
    close(saved);
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
    test_cell_ratio();
    test_colors();
    test_locale();
    test_hud();
    test_round();
    test_nonblocking_write();
    printf("term test: %d errors\n", errors);
    return errors != 0;
}
