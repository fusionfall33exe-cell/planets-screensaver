/*
 * Render test: every scene at many terminal sizes and moments in time, in
 * both pixel modes. Fails on NaN, infinity, negative colors or pixel ids
 * that point at nothing. "make test" builds it with sanitizers.
 * THREADS=n picks the number of render threads (default: the maximum).
 */
#define main planets_main
#include "../planets.c"
#undef main

static void setup(int cols, int rows, int ascii)
{
    g_ascii = ascii;
    g_cols = cols;
    g_rows = rows;
    g_pw = cols;
    g_ph = ascii ? rows : rows * 2;
    g_yasp = ascii ? 0.5f : 1.0f;
    g_focal = fminf((float)g_pw, g_ph / g_yasp);
    size_t np = (size_t)g_pw * g_ph;
    g_color = xrealloc(g_color, np * sizeof *g_color);
    g_color2 = xrealloc(g_color2, np * sizeof *g_color2);
    g_id = xrealloc(g_id, np * sizeof *g_id);
}

int main(void)
{
    static const int sizes[][2] = { { 1, 1 }, { 2, 1 }, { 7, 3 }, { 30, 10 }, { 80, 24 }, { 211, 57 }, { 320, 100 } };
    static const float times[] = { 0, 0.001f, 1, 3.3f, 7.7f, 12.5f, 24.99f, 25, 100, 1000, 1e4f, 1e5f };
    long frames = 0, bad = 0, pixels = 0;

    init_tables();
    const char *th = getenv("THREADS");
    g_nthreads = th ? atoi(th) : MAXTHREADS;
    if (g_nthreads < 1 || g_nthreads > MAXTHREADS)
        g_nthreads = MAXTHREADS;

    for (size_t z = 0; z < sizeof sizes / sizeof sizes[0]; z++)
        for (int ascii = 0; ascii <= 1; ascii++) {
            setup(sizes[z][0], sizes[z][1], ascii);
            for (int s = 0; s < NSCENES; s++)
                for (size_t k = 0; k < sizeof times / sizeof times[0]; k++) {
                    render_scene(s, times[k]);
                    frames++;
                    for (int i = 0; i < g_pw * g_ph; i++, pixels++) {
                        V3 c = g_color[i];
                        int id = g_id[i];
                        int ok = isfinite(c.x) && isfinite(c.y) && isfinite(c.z)
                              && c.x >= 0 && c.y >= 0 && c.z >= 0
                              && (id == -1 || (id >= 0 && id < g_nbody) || (id >= 100 && id < 100 + g_nring));
                        if (!ok && bad++ < 5)
                            printf("BAD: %dx%d ascii=%d %s t=%g pixel %d color (%g, %g, %g) id %d\n",
                                   sizes[z][0], sizes[z][1], ascii, g_scenes[s].name, times[k], i,
                                   c.x, c.y, c.z, id);
                    }
                }
        }
    printf("render test: %ld frames, %ld pixels, %ld bad (%d threads)\n", frames, pixels, bad, g_nthreads);
    free(g_color);
    free(g_color2);
    free(g_id);
    return bad != 0;
}
