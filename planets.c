/*
 * planets - the solar system in your terminal
 *
 * Every frame is ray traced per pixel: perfectly round planets, procedural
 * surfaces (continents, cloud bands, storms, craters), glowing atmospheres,
 * see-through rings, moons that cast shadows and anti-aliased edges. Output
 * is 24-bit ANSI color; each character cell shows two pixels stacked on top
 * of each other (the upper half block glyph), so pixels come out square.
 *
 * The tour goes outwards from the Sun and loops forever:
 *   1 MERCURY  2 VENUS  3 EARTH  4 MARS  5 JUPITER  6 SATURN  7 URANUS
 *   8 NEPTUNE  9 SOLAR SYSTEM
 *
 * Build: make    Install: make install    Run: Planets    Help: Planets -h
 */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PI       3.14159265f
#define NSTARS   2000
#define NCRATER  64
#define MAXBODY  12
#define MAXRING  2
#define MAXTHREADS 8
#define SKY_W    512                /* sky texture size (equirectangular) */
#define SKY_H    256
#define XFADE    1.5f               /* seconds of crossfade between scenes */
#define FG_ANY   0xFFFFFFFFu        /* cell does not care about its foreground */

/* ------------------------------------------------------------------ */
/* vector math                                                         */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, z; } V3;
typedef struct { float m[3][3]; } M3;

static V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 scale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static V3 mul(V3 a, V3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static V3 cross(V3 a, V3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

static V3 norm(V3 a)
{
    float l = sqrtf(dot(a, a));
    return l > 1e-12f ? scale(a, 1.0f / l) : a;
}

static V3 mix(V3 a, V3 b, float t) { return add(a, scale(sub(b, a), t)); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* smoothstep; also works "backwards" when e0 > e1 */
static float smooth(float e0, float e1, float x)
{
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static M3 m3(float a, float b, float c, float d, float e, float f, float g, float h, float i)
{
    M3 r = { { { a, b, c }, { d, e, f }, { g, h, i } } };
    return r;
}

static M3 rot_x(float a) { float c = cosf(a), s = sinf(a); return m3(1, 0, 0, 0, c, -s, 0, s, c); }
static M3 rot_y(float a) { float c = cosf(a), s = sinf(a); return m3(c, 0, s, 0, 1, 0, -s, 0, c); }
static M3 rot_z(float a) { float c = cosf(a), s = sinf(a); return m3(c, -s, 0, s, c, 0, 0, 0, 1); }

static M3 m3_mul(M3 a, M3 b)
{
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

static M3 m3_t(M3 a)
{
    return m3(a.m[0][0], a.m[1][0], a.m[2][0],
              a.m[0][1], a.m[1][1], a.m[2][1],
              a.m[0][2], a.m[1][2], a.m[2][2]);
}

static V3 m3_apply(M3 m, V3 v)
{
    return v3(m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
              m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
              m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z);
}

/* world -> view rotation for a camera looking from eye at target */
static M3 look_at(V3 eye, V3 target)
{
    V3 f = norm(sub(target, eye));
    V3 r = norm(cross(v3(0, 1, 0), f));
    V3 u = cross(f, r);
    return m3(r.x, r.y, r.z, u.x, u.y, u.z, f.x, f.y, f.z);
}

/* ------------------------------------------------------------------ */
/* noise                                                               */
/* ------------------------------------------------------------------ */

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float lattice(int x, int y, int z)
{
    uint32_t h = hash32((uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu);
    return (float)h * (2.0f / 4294967296.0f) - 1.0f;
}

/* smooth 3D value noise in [-1, 1] */
static float noise(V3 p)
{
    float fx = floorf(p.x), fy = floorf(p.y), fz = floorf(p.z);
    int x = (int)fx, y = (int)fy, z = (int)fz;
    float u = p.x - fx, v = p.y - fy, w = p.z - fz;
    u = u * u * u * (u * (u * 6 - 15) + 10);
    v = v * v * v * (v * (v * 6 - 15) + 10);
    w = w * w * w * (w * (w * 6 - 15) + 10);

    float a = lattice(x, y, z),         b = lattice(x + 1, y, z);
    float c = lattice(x, y + 1, z),     d = lattice(x + 1, y + 1, z);
    float e = lattice(x, y, z + 1),     f = lattice(x + 1, y, z + 1);
    float g = lattice(x, y + 1, z + 1), h = lattice(x + 1, y + 1, z + 1);
    float k0 = a + (b - a) * u, k1 = c + (d - c) * u;
    float k2 = e + (f - e) * u, k3 = g + (h - g) * u;
    float l0 = k0 + (k1 - k0) * v, l1 = k2 + (k3 - k2) * v;
    return l0 + (l1 - l0) * w;
}

/* fractal noise: octaves of detail on top of each other, roughly [-1, 1] */
static float fbm(V3 p, int octaves)
{
    float sum = 0, amp = 0.5f;
    for (int i = 0; i < octaves; i++) {
        sum += amp * noise(p);
        p = v3(p.x * 2.02f + 1.7f, p.y * 2.02f - 3.1f, p.z * 2.02f + 5.3f);
        amp *= 0.5f;
    }
    return sum;
}

/* fixed-seed generator: the sky and the craters look the same every run */
static uint32_t g_rng = 0x2545f491u;

static float rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (float)(g_rng >> 8) / 16777216.0f;
}

static float gauss(void)
{
    float u1 = fmaxf(rnd(), 1e-7f), u2 = rnd();
    return sqrtf(-2.0f * logf(u1)) * cosf(2 * PI * u2);
}

static V3 rnd_dir(void)
{
    float z = rnd() * 2 - 1, a = rnd() * 2 * PI, r = sqrtf(1 - z * z);
    return v3(r * cosf(a), z, r * sinf(a));
}

/* ------------------------------------------------------------------ */
/* planet surfaces                                                     */
/* ------------------------------------------------------------------ */

enum {
    T_SUN, T_MERCURY, T_VENUS, T_EARTH, T_MOON, T_MARS, T_ROCK, T_JUPITER, T_IO,
    T_EUROPA, T_GANYMEDE, T_CALLISTO, T_SATURN, T_TITAN, T_URANUS, T_NEPTUNE,
    T_TRITON, T_ICE
};

typedef struct {
    V3 albedo;                      /* surface color */
    V3 emit;                        /* light of its own (the Sun) */
    V3 bump;                        /* tilt of the surface normal, body space */
    float spec;                     /* shininess (oceans) */
    float night;                    /* city lights on the night side */
} Surf;

static float g_time;                /* scene time, animates storms and clouds */
static V3    g_crater_c[NCRATER];
static float g_crater_r[NCRATER];

/*
 * Bowl-shaped craters with raised rims. Returns a brightness change and
 * tilts *bump along the crater slopes, so low sunlight casts shading in them.
 */
static float craters(V3 d, int first, int count, float depth, V3 *bump)
{
    float shade = 0;
    for (int i = 0; i < count; i++) {
        int k = (first + i) % NCRATER;
        float r = g_crater_r[k];
        V3 dd = sub(d, g_crater_c[k]);
        float d2 = dot(dd, dd);
        if (d2 > r * r * 2.25f)
            continue;
        float x = sqrtf(d2) / r;
        float rim = expf(-(x - 1) * (x - 1) * 25.0f);
        float slope = x * (1 - smooth(0.9f, 1.05f, x)) - 20.0f * rim * (x - 1);
        V3 tdir = sub(dd, scale(d, dot(dd, d)));    /* along the ground, away from the center */
        float tl = sqrtf(dot(tdir, tdir));
        if (tl > 1e-6f)
            *bump = sub(*bump, scale(tdir, slope * depth / tl));
        shade += -0.06f * (1 - smooth(0.8f, 0.95f, x)) + 0.10f * rim;
    }
    return shade;
}

static Surf surface(int tex, V3 d, float extra)
{
    Surf s = { { 0.5f, 0.5f, 0.5f }, { 0, 0, 0 }, { 0, 0, 0 }, 0, 0 };
    float t = g_time;

    switch (tex) {
    case T_SUN: {
        float g = fbm(add(scale(d, 6.0f), v3(0, t * 0.2f, 0)), 4);
        s.emit = mix(v3(1.0f, 0.42f, 0.06f), v3(1.0f, 0.9f, 0.55f), clampf(0.55f + g * 1.4f, 0, 1));
        break;
    }
    case T_MERCURY: {
        float g = 0.48f + 0.2f * fbm(scale(d, 3.0f), 4) + craters(d, 0, NCRATER, 0.22f, &s.bump);
        s.albedo = v3(g, g * 0.94f, g * 0.88f);
        break;
    }
    case T_VENUS: {
        /* thick sulfuric clouds, smeared into soft swirls */
        V3 q = add(d, scale(v3(fbm(scale(d, 2.0f), 3), 0, fbm(add(scale(d, 2.0f), v3(4, 1, 7)), 3)), 0.35f));
        float band = sinf(q.y * 7.0f + 2.0f * fbm(scale(q, 3.0f), 4));
        s.albedo = mix(v3(0.80f, 0.66f, 0.42f), v3(0.98f, 0.92f, 0.74f), 0.5f + 0.4f * band);
        break;
    }
    case T_EARTH: {
        const float sea = 0.06f;
        float e = fbm(scale(d, 1.6f), 6) + 0.25f * noise(scale(d, 0.8f));
        float lat = fabsf(d.y);
        if (e < sea) {              /* ocean: darker when deep, shiny */
            s.albedo = mix(v3(0.01f, 0.05f, 0.20f), v3(0.04f, 0.22f, 0.42f), smooth(sea - 0.25f, sea, e));
            s.spec = 0.6f;
        } else {
            float wet = fbm(add(scale(d, 3.0f), v3(9, 9, 9)), 4);
            V3 c = mix(v3(0.13f, 0.34f, 0.10f), v3(0.33f, 0.50f, 0.18f), smooth(-0.2f, 0.2f, wet));
            c = mix(c, v3(0.80f, 0.66f, 0.42f), smooth(-0.05f, -0.25f, wet) * (1 - smooth(0.35f, 0.55f, lat)));
            c = mix(c, v3(0.42f, 0.38f, 0.32f), smooth(0.25f, 0.45f, e - sea));   /* mountains */
            c = mix(c, v3(0.45f, 0.45f, 0.40f), smooth(0.55f, 0.7f, lat));        /* tundra */
            s.albedo = c;
            s.night = smooth(0.45f, 0.75f, noise(scale(d, 28.0f))) * (1 - smooth(0.5f, 0.65f, lat));
        }
        float ice = smooth(0.80f, 0.86f, lat + 0.06f * fbm(scale(d, 6.0f), 3));
        s.albedo = mix(s.albedo, v3(0.92f, 0.95f, 1.0f), ice);
        s.spec *= 1 - ice;
        s.night *= 1 - ice;

        /* clouds drift a little faster than the ground */
        V3 cd = m3_apply(rot_y(extra), d);
        float cover = smooth(0.02f, 0.3f, fbm(add(scale(cd, 2.2f), v3(3, 7, 1)), 6) + 0.1f * (lat - 0.3f));
        s.albedo = mix(s.albedo, v3(0.95f, 0.96f, 1.0f), cover * 0.95f);
        s.spec *= 1 - cover;
        s.night *= 1 - cover * 0.8f;
        break;
    }
    case T_MOON: {
        float g = 0.66f - 0.28f * smooth(0.0f, 0.18f, fbm(scale(d, 2.0f), 4))    /* dark maria */
                + 0.08f * fbm(scale(d, 9.0f), 2) + craters(d, 20, 30, 0.18f, &s.bump);
        s.albedo = v3(g, g, g * 0.97f);
        break;
    }
    case T_MARS: {
        float f = fbm(scale(d, 2.4f), 5);
        V3 c = mix(v3(0.78f, 0.40f, 0.20f), v3(0.46f, 0.22f, 0.12f), smooth(0.02f, 0.3f, f));
        c = mix(c, v3(0.88f, 0.62f, 0.42f), smooth(-0.1f, -0.35f, f));
        float g = craters(d, 40, 16, 0.12f, &s.bump);
        c = add(c, v3(g, g * 0.6f, g * 0.4f));
        float cap = fmaxf(smooth(0.86f, 0.9f, d.y + 0.04f * f), smooth(0.9f, 0.94f, 0.04f * f - d.y));
        s.albedo = mix(c, v3(0.95f, 0.93f, 0.9f), cap);
        break;
    }
    case T_ROCK: {
        float g = 0.30f + 0.08f * fbm(scale(d, 4.0f), 3) + craters(d, 7, 20, 0.25f, &s.bump);
        s.albedo = v3(g, g * 0.9f, g * 0.82f);
        break;
    }
    case T_JUPITER: {
        /* bands flow at different speeds; turbulence is stretched along them */
        float lat = d.y;
        V3 q = m3_apply(rot_y(t * 0.04f * sinf(lat * 23.0f)), d);
        float turb = fbm(v3(q.x * 3.0f, q.y * 14.0f, q.z * 3.0f), 5);
        float band = sinf(lat * 22.0f + turb * 1.8f);
        V3 c = mix(v3(0.66f, 0.43f, 0.28f), v3(0.93f, 0.88f, 0.78f), 0.5f + 0.5f * band);
        c = mix(c, v3(0.88f, 0.62f, 0.38f), smooth(0.15f, 0.0f, fabsf(lat)) * 0.4f);   /* orange equator */
        c = mix(c, v3(0.55f, 0.52f, 0.50f), smooth(0.75f, 0.95f, fabsf(lat)));         /* gray poles */

        /* the Great Red Spot, slowly swirling */
        float dl = remainderf(atan2f(q.z, q.x) - 0.8f, 2 * PI), dy = lat + 0.36f;
        float e = dl * dl / 0.06f + dy * dy / 0.004f;
        if (e < 1.5f) {
            float swirl = sinf(atan2f(dy * 4.0f, dl) * 2.0f + sqrtf(e) * 6.0f - t * 0.8f);
            V3 spot = mix(v3(0.78f, 0.33f, 0.20f), v3(0.90f, 0.55f, 0.40f), 0.5f + 0.3f * swirl);
            c = mix(spot, c, smooth(0.6f, 1.4f, e));
        }
        s.albedo = c;
        break;
    }
    case T_IO: {
        V3 c = mix(v3(0.95f, 0.86f, 0.45f), v3(0.85f, 0.52f, 0.20f), smooth(0.05f, 0.3f, fbm(scale(d, 3.5f), 4)));
        s.albedo = mix(c, v3(0.25f, 0.14f, 0.08f), smooth(0.55f, 0.75f, noise(scale(d, 7.0f))));   /* volcanoes */
        break;
    }
    case T_EUROPA: {
        /* cracked ice: lines where the noise crosses zero */
        float l1 = 1 - smooth(0.0f, 0.07f, fabsf(noise(scale(d, 4.5f))));
        float l2 = 1 - smooth(0.0f, 0.05f, fabsf(noise(add(scale(d, 7.0f), v3(3, 1, 2)))));
        s.albedo = mix(v3(0.92f, 0.88f, 0.80f), v3(0.62f, 0.40f, 0.26f), fmaxf(l1, l2 * 0.7f) * 0.75f);
        break;
    }
    case T_GANYMEDE: {
        V3 c = mix(v3(0.36f, 0.32f, 0.28f), v3(0.68f, 0.64f, 0.60f), smooth(-0.1f, 0.2f, fbm(scale(d, 3.0f), 4)));
        float g = 1.5f * craters(d, 30, 16, 0.1f, &s.bump);
        s.albedo = add(c, v3(g, g, g));
        break;
    }
    case T_CALLISTO: {
        float g = 0.28f + 0.06f * fbm(scale(d, 5.0f), 3) + 1.8f * fmaxf(0.0f, craters(d, 10, 40, 0.1f, &s.bump));
        s.albedo = v3(g, g * 0.92f, g * 0.84f);
        break;
    }
    case T_SATURN: {
        float lat = d.y;
        float turb = fbm(v3(d.x * 2.0f, d.y * 10.0f, d.z * 2.0f), 4);
        V3 c = mix(v3(0.80f, 0.68f, 0.46f), v3(0.94f, 0.86f, 0.66f), 0.5f + 0.35f * sinf(lat * 16.0f + turb * 0.8f));
        c = mix(c, v3(0.60f, 0.62f, 0.62f), smooth(0.80f, 0.95f, lat));   /* bluish north pole */
        if (lat > 0.85f) {          /* the hexagon-shaped storm around the north pole */
            float rho = sqrtf(d.x * d.x + d.z * d.z);
            float seg = fmodf(atan2f(d.z, d.x) + 2 * PI, PI / 3) - PI / 6;
            float hex = 0.30f * cosf(PI / 6) / cosf(seg);
            c = mix(c, v3(0.45f, 0.48f, 0.5f), 0.6f * (1 - smooth(0.0f, 0.03f, fabsf(rho - hex))));
        }
        s.albedo = c;
        break;
    }
    case T_TITAN: {
        float b = 0.5f + 0.5f * sinf(d.y * 5.0f + fbm(scale(d, 2.0f), 3));
        s.albedo = mix(v3(0.78f, 0.50f, 0.20f), v3(0.90f, 0.65f, 0.30f), b);
        break;
    }
    case T_URANUS: {
        float band = 0.5f + 0.5f * sinf(d.y * 9.0f + 0.5f * fbm(v3(d.x * 2.0f, d.y * 8.0f, d.z * 2.0f), 3));
        V3 c = mix(v3(0.55f, 0.80f, 0.84f), v3(0.66f, 0.88f, 0.90f), band * 0.6f);
        s.albedo = mix(c, v3(0.75f, 0.92f, 0.93f), smooth(0.6f, 0.9f, d.y));
        break;
    }
    case T_NEPTUNE: {
        float turb = fbm(v3(d.x * 2.5f, d.y * 10.0f, d.z * 2.5f), 4);
        V3 c = mix(v3(0.15f, 0.30f, 0.80f), v3(0.28f, 0.48f, 0.95f), 0.5f + 0.4f * sinf(d.y * 12.0f + turb * 1.5f));
        float dl = remainderf(atan2f(d.z, d.x) - 1.0f, 2 * PI), dy = d.y + 0.35f;
        c = mix(v3(0.06f, 0.12f, 0.40f), c, smooth(0.5f, 1.2f, dl * dl / 0.05f + dy * dy / 0.006f));   /* dark spot */
        float streak = smooth(0.35f, 0.55f, fbm(v3(d.x * 3.0f, d.y * 25.0f, d.z * 3.0f), 3));
        s.albedo = mix(c, v3(0.9f, 0.95f, 1.0f), streak * 0.8f);                                      /* white clouds */
        break;
    }
    case T_TRITON: {
        V3 c = mix(v3(0.78f, 0.74f, 0.72f), v3(0.90f, 0.72f, 0.68f), smooth(-0.1f, 0.25f, fbm(scale(d, 3.0f), 4)));
        s.albedo = mix(c, v3(0.95f, 0.85f, 0.82f), smooth(0.2f, 0.5f, -d.y));   /* pink polar cap */
        break;
    }
    default: {                      /* T_ICE: small icy moons */
        float g = 0.52f + 0.1f * fbm(scale(d, 3.5f), 3) + craters(d, 50, 14, 0.12f, &s.bump);
        s.albedo = v3(g, g, g * 1.04f);
        break;
    }
    }
    return s;
}

/* Rings, by distance from the planet center in planet radii. Returns opacity. */
enum { RING_SATURN, RING_URANUS };

static float ring_tex(int tex, float x, V3 *col)
{
    if (tex == RING_URANUS) {       /* a set of narrow dark rings */
        static const float rr[8] = { 1.64f, 1.68f, 1.73f, 1.80f, 1.84f, 1.87f, 1.96f, 2.00f };
        float a = 0;
        for (int i = 0; i < 8; i++) {
            float k = (x - rr[i]) / (i == 7 ? 0.04f : 0.022f);
            a = fmaxf(a, expf(-k * k));
        }
        *col = v3(0.42f, 0.42f, 0.45f);
        return a * 0.45f;
    }

    /* Saturn: C ring, bright B ring, Cassini division, A ring, with soft edges */
    float wc = smooth(1.22f, 1.26f, x) * (1 - smooth(1.51f, 1.55f, x));
    float wb = smooth(1.51f, 1.55f, x) * (1 - smooth(1.93f, 1.97f, x));
    float wd = smooth(1.93f, 1.97f, x) * (1 - smooth(2.01f, 2.05f, x));
    float wa = smooth(2.01f, 2.05f, x) * (1 - smooth(2.25f, 2.29f, x));
    float w = wc + wb + wd + wa;
    if (w < 1e-4f)
        return 0;
    V3 c = add(add(scale(v3(0.45f, 0.42f, 0.38f), wc), scale(v3(0.86f, 0.78f, 0.62f), wb)),
               add(scale(v3(0.50f, 0.45f, 0.40f), wd), scale(v3(0.76f, 0.70f, 0.60f), wa)));
    float fine = 0.82f + 0.18f * sinf(x * 45.0f) * sinf(x * 13.0f + 1.0f);   /* ringlets */
    *col = scale(c, fine / w);
    return (0.18f * wc + 0.92f * wb + 0.08f * wd + 0.65f * wa) * (0.85f + 0.15f * sinf(x * 31.0f));
}

/* ------------------------------------------------------------------ */
/* the scene: bodies, rings, camera and light                          */
/* ------------------------------------------------------------------ */

typedef struct {
    V3 c;                           /* center, view space */
    float r;
    M3 rot;                         /* view space -> body space, for the surface */
    M3 irot;                        /* body space -> view space */
    int tex;
    float atmo;                     /* atmosphere height in radii, 0 = none */
    V3 atmo_col;
    float extra;                    /* surface parameter (Earth: cloud drift) */
} Body;

typedef struct { V3 c, n; float rin, rout, pr; int tex; } Ring;

static Body  g_body[MAXBODY];
static int   g_nbody;
static Ring  g_ring[MAXRING];
static int   g_nring;
static M3    g_view, g_view_t;      /* world -> view rotation and its inverse */
static V3    g_eye;
static V3    g_light;               /* towards the Sun, view space */
static int   g_point_light;         /* solar system view: light comes from g_sun_pos */
static V3    g_sun_pos;

static void camera(float dist, float az, float el)
{
    g_eye = v3(dist * sinf(az) * cosf(el), dist * sinf(el), -dist * cosf(az) * cosf(el));
    g_view = look_at(g_eye, v3(0, 0, 0));
    g_view_t = m3_t(g_view);
}

static void sun_from(V3 dir)
{
    g_light = norm(m3_apply(g_view, norm(dir)));
}

/* o turns body space into world space (tilt and spin) */
static void add_body(V3 c, float r, M3 o, int tex, float atmo, V3 atmo_col, float extra)
{
    if (g_nbody >= MAXBODY)
        return;
    Body *b = &g_body[g_nbody++];
    b->c = m3_apply(g_view, sub(c, g_eye));
    b->r = r;
    b->irot = m3_mul(g_view, o);
    b->rot = m3_t(b->irot);
    b->tex = tex;
    b->atmo = atmo;
    b->atmo_col = atmo_col;
    b->extra = extra;
}

static void add_ring(V3 c, V3 n, float pr, int tex)
{
    if (g_nring >= MAXRING)
        return;
    Ring *rg = &g_ring[g_nring++];
    rg->c = m3_apply(g_view, sub(c, g_eye));
    rg->n = norm(m3_apply(g_view, n));
    rg->rin = pr * 1.2f;
    rg->rout = pr * 2.3f;
    rg->pr = pr;
    rg->tex = tex;
}

/* a moon on a circular orbit in the x-z plane of 'o', around the origin */
static void add_moon(M3 o, float orbit, float angle, float r, int tex, float atmo, V3 atmo_col)
{
    V3 c = m3_apply(o, v3(orbit * cosf(angle), 0, orbit * sinf(angle)));
    add_body(c, r, m3_mul(o, rot_y(-angle)), tex, atmo, atmo_col, 0);   /* same face to the planet */
}

/* ------------------------------------------------------------------ */
/* ray tracer                                                          */
/* ------------------------------------------------------------------ */

static int    g_cols, g_rows;       /* terminal size in cells */
static int    g_pw, g_ph;           /* pixel buffer size */
static float  g_focal, g_yasp;      /* projection scale, pixel aspect */
static V3    *g_color, *g_color2;   /* frame, and a second one for crossfades */
static int   *g_id;                 /* what each pixel shows: body index, 100+ring, -1 sky */
static V3     g_sky[SKY_H][SKY_W];
static V3     g_mw_n, g_mw_u, g_mw_v; /* Milky Way plane */

static int    g_ascii;              /* 0 = half-block pixels, 1 = ASCII shading */
static int    g_256;                /* 256-color palette instead of 24-bit */

static const V3 ZERO = { 0, 0, 0 };

static float hit_sphere(V3 d, const Body *b)
{
    float tca = dot(d, b->c);
    if (tca <= 0)
        return -1;
    float d2 = dot(b->c, b->c) - tca * tca, r2 = b->r * b->r;
    if (d2 > r2)
        return -1;
    return tca - sqrtf(r2 - d2);
}

/* ray o + t*d against a ring; *x is the hit radius in planet radii */
static float hit_ring(V3 o, V3 d, const Ring *rg, float *x)
{
    float den = dot(d, rg->n);
    if (fabsf(den) < 1e-6f)
        return -1;
    float t = dot(sub(rg->c, o), rg->n) / den;
    if (t <= 1e-4f)
        return -1;
    V3 q = sub(add(o, scale(d, t)), rg->c);
    float r = sqrtf(dot(q, q));
    if (r < rg->rin || r > rg->rout)
        return -1;
    *x = r / rg->pr;
    return t;
}

static V3 light_dir(V3 p)
{
    return g_point_light ? norm(sub(g_sun_pos, p)) : g_light;
}

/* How much sunlight reaches p: other bodies (soft-edged) and rings block it. */
static float sunlight(V3 p, V3 l, int self, int rings)
{
    float light = 1.0f;
    for (int j = 0; j < g_nbody; j++) {
        const Body *b = &g_body[j];
        if (j == self || b->tex == T_SUN)
            continue;
        V3 oc = sub(b->c, p);
        float tca = dot(oc, l);
        if (tca <= 0)
            continue;
        float dist = sqrtf(fmaxf(0.0f, dot(oc, oc) - tca * tca));
        float pen = b->r * 0.06f + tca * 0.01f;
        light *= smooth(b->r - pen, b->r + pen, dist);
    }
    for (int k = 0; rings && k < g_nring; k++) {
        float x;
        V3 col;
        if (hit_ring(p, l, &g_ring[k], &x) > 0)
            light *= 1.0f - 0.9f * ring_tex(g_ring[k].tex, x, &col);
    }
    return light;
}

static V3 shade_body(int i, V3 p, V3 d)
{
    const Body *b = &g_body[i];
    V3 n = scale(sub(p, b->c), 1.0f / b->r);
    V3 v = scale(d, -1.0f);
    float ndv = fmaxf(0.0f, dot(n, v));
    Surf s = surface(b->tex, m3_apply(b->rot, n), b->extra);
    if (b->tex == T_SUN)
        return scale(s.emit, 0.55f + 0.45f * ndv);      /* limb darkening */

    V3 ns = norm(add(n, m3_apply(b->irot, s.bump)));
    V3 l = light_dir(p);
    float ndl = dot(n, l);
    float sun = sunlight(p, l, i, 1);
    float diff = fmaxf(0.0f, dot(ns, l)) * smooth(-0.05f, 0.08f, ndl) * sun;   /* soft terminator */
    V3 col = mul(s.albedo, v3(diff * 1.04f + 0.012f, diff + 0.014f, diff * 0.95f + 0.02f));

    if (s.spec > 0 && diff > 0) {   /* sun glint on water */
        float sp = powf(fmaxf(0.0f, dot(ns, norm(add(l, v)))), 50.0f) * s.spec * sun;
        col = add(col, v3(sp, sp * 0.95f, sp * 0.85f));
    }
    if (s.night > 0)
        col = add(col, scale(v3(1.0f, 0.72f, 0.35f), s.night * 0.8f * (1 - smooth(-0.2f, 0.05f, ndl))));
    if (b->atmo > 0) {              /* atmosphere: bright rim on the day side */
        float rim = powf(1.0f - ndv, 3.0f), lit = smooth(-0.3f, 0.5f, ndl);
        col = add(col, scale(b->atmo_col, (rim * 0.9f + 0.05f) * lit));
    }
    return col;
}

/* glow of atmospheres (and the Sun's corona) just outside the planet edge */
static V3 halos(V3 d, float tmax, V3 col)
{
    for (int i = 0; i < g_nbody; i++) {
        const Body *b = &g_body[i];
        if (b->atmo <= 0)
            continue;
        float tca = dot(d, b->c);
        if (tca <= 0 || tca > tmax)
            continue;
        float dist = sqrtf(fmaxf(0.0f, dot(b->c, b->c) - tca * tca));
        float h = b->r * b->atmo;
        if (dist <= b->r || dist >= b->r + h)
            continue;
        float k = 1.0f - (dist - b->r) / h;
        k *= k;
        if (b->tex == T_SUN) {
            col = add(col, scale(b->atmo_col, k * 0.9f));
            continue;
        }
        V3 q = scale(d, tca), m = scale(sub(q, b->c), 1.0f / dist);
        V3 l = light_dir(q);
        float lit = smooth(-0.35f, 0.45f, dot(m, l));
        float fwd = powf(fmaxf(0.0f, dot(d, l)), 6.0f);    /* backlit planets glow */
        col = add(col, scale(b->atmo_col, k * (lit * 0.8f + fwd * 1.2f)));
    }
    return col;
}

static V3 sky_color(V3 d)
{
    V3 w = m3_apply(g_view_t, d);
    float u = (atan2f(w.z, w.x) / (2 * PI) + 0.5f) * SKY_W - 0.5f;
    float v = (asinf(clampf(w.y, -1, 1)) / PI + 0.5f) * SKY_H - 0.5f;
    int u0 = (int)floorf(u), v0 = (int)floorf(v);
    float fu = u - u0, fv = v - v0;
    int u1 = (u0 + 1) % SKY_W;
    u0 = (u0 + SKY_W) % SKY_W;
    int va = v0 < 0 ? 0 : v0, vb = v0 + 1 >= SKY_H ? SKY_H - 1 : v0 + 1;
    V3 a = mix(g_sky[va][u0], g_sky[va][u1], fu), b = mix(g_sky[vb][u0], g_sky[vb][u1], fu);
    return mix(a, b, fv);
}

static V3 trace(V3 d, int *id)
{
    float tbest = 1e30f;
    int hit = -1;
    for (int i = 0; i < g_nbody; i++) {
        float t = hit_sphere(d, &g_body[i]);
        if (t > 0 && t < tbest) {
            tbest = t;
            hit = i;
        }
    }
    V3 col = hit >= 0 ? shade_body(hit, scale(d, tbest), d) : sky_color(d);
    col = halos(d, tbest, col);
    *id = hit;

    /* see-through rings in front of whatever the ray hit */
    for (int k = 0; k < g_nring; k++) {
        const Ring *rg = &g_ring[k];
        float x, t = hit_ring(ZERO, d, rg, &x);
        if (t <= 0 || t >= tbest)
            continue;
        V3 rc;
        float a = ring_tex(rg->tex, x, &rc);
        if (a < 0.001f)
            continue;
        V3 p = scale(d, t), l = light_dir(p);
        float lit = (0.3f + 0.7f * fabsf(dot(rg->n, l))) * sunlight(p, l, -1, 0);
        if (dot(rg->n, l) * dot(rg->n, d) > 0)      /* looking at the unlit side */
            lit *= 0.45f;
        col = mix(col, scale(rc, lit + 0.02f), a);
        if (a > 0.35f)
            *id = 100 + k;
    }
    return col;
}

static V3 pixel_ray(float x, float y)
{
    return norm(v3((x - g_pw * 0.5f) / g_focal, (g_ph * 0.5f - y) / (g_focal * g_yasp), 1.0f));
}

/* ------------------------------------------------------------------ */
/* stars, sky, orbits                                                  */
/* ------------------------------------------------------------------ */

typedef struct { V3 dir, col; } Star;
static Star g_stars[NSTARS];

/* Add light to one sky pixel. Planets and rings hide it. */
static void splat(int x, int y, V3 c)
{
    if (x < 0 || y < 0 || x >= g_pw || y >= g_ph)
        return;
    int i = y * g_pw + x;
    if (g_id[i] == -1)
        g_color[i] = add(g_color[i], c);
}

/* A point spread over the 4 nearest pixels, so it glides instead of jumping. */
static void splat_point(V3 view, V3 c)
{
    if (view.z < 0.05f)
        return;
    float sx = g_pw * 0.5f + view.x / view.z * g_focal - 0.5f;
    float sy = g_ph * 0.5f - view.y / view.z * g_focal * g_yasp - 0.5f;
    if (sx < -2 || sy < -2 || sx > g_pw + 1 || sy > g_ph + 1)
        return;
    int x = (int)floorf(sx), y = (int)floorf(sy);
    float fx = sx - x, fy = sy - y;
    splat(x, y, scale(c, (1 - fx) * (1 - fy)));
    splat(x + 1, y, scale(c, fx * (1 - fy)));
    splat(x, y + 1, scale(c, (1 - fx) * fy));
    splat(x + 1, y + 1, scale(c, fx * fy));
}

static void draw_stars(void)
{
    for (int i = 0; i < NSTARS; i++)
        splat_point(m3_apply(g_view, g_stars[i].dir), g_stars[i].col);
}

static V3 sky_fn(V3 w)
{
    float band = dot(w, g_mw_n);
    float mw = expf(-band * band * 28.0f);
    V3 c = scale(v3(0.62f, 0.60f, 0.72f), mw * clampf(0.3f + 1.4f * fbm(scale(w, 3.0f), 4), 0, 1) * 0.13f);
    c = scale(c, 1.0f - 0.6f * mw * smooth(0.1f, 0.35f, fbm(add(scale(w, 6.0f), v3(2, 2, 2)), 3)));  /* dust */
    float n1 = fbm(add(scale(w, 1.4f), v3(7, 3, 1)), 4);
    float n2 = fbm(add(scale(w, 1.8f), v3(-4, 8, 2)), 4);
    c = add(c, scale(v3(0.38f, 0.10f, 0.42f), smooth(0.12f, 0.45f, n1) * 0.09f));   /* nebulae */
    return add(c, scale(v3(0.08f, 0.22f, 0.42f), smooth(0.15f, 0.5f, n2) * 0.08f));
}

static void init_tables(void)
{
    g_mw_n = norm(v3(0.25f, 0.9f, -0.35f));
    g_mw_u = norm(cross(g_mw_n, v3(1, 0, 0)));
    g_mw_v = cross(g_mw_n, g_mw_u);

    for (int y = 0; y < SKY_H; y++) {
        float lat = (y + 0.5f) / SKY_H * PI - PI / 2;
        for (int x = 0; x < SKY_W; x++) {
            float lon = (x + 0.5f) / SKY_W * 2 * PI - PI;
            g_sky[y][x] = sky_fn(v3(cosf(lat) * cosf(lon), sinf(lat), cosf(lat) * sinf(lon)));
        }
    }

    for (int i = 0; i < NSTARS; i++) {
        V3 d;
        if (rnd() < 0.45f) {        /* crowd along the Milky Way */
            float a = rnd() * 2 * PI;
            d = norm(add(add(scale(g_mw_u, cosf(a)), scale(g_mw_v, sinf(a))), scale(g_mw_n, gauss() * 0.12f)));
        } else {
            d = rnd_dir();
        }
        float m = rnd(), k = rnd();
        V3 col = k < 0.15f ? v3(0.65f, 0.75f, 1.0f)     /* hot blue */
               : k < 0.60f ? v3(1.0f, 1.0f, 1.0f)
               : k < 0.85f ? v3(1.0f, 0.92f, 0.75f)     /* sun-like */
                           : v3(1.0f, 0.7f, 0.5f);      /* cool red */
        g_stars[i].dir = d;
        g_stars[i].col = scale(col, 0.12f + 0.9f * m * m * m * m);
    }

    for (int i = 0; i < NCRATER; i++) {
        float u = rnd();
        g_crater_c[i] = rnd_dir();
        g_crater_r[i] = 0.03f + 0.2f * u * u * u;
    }
}

/* ------------------------------------------------------------------ */
/* scenes                                                              */
/* ------------------------------------------------------------------ */

static void scene_mercury(float t)
{
    camera(3.5f, -0.3f + t * 0.03f, 0.10f);
    sun_from(v3(-1.0f, 0.2f, -0.5f));
    add_body(ZERO, 1.0f, rot_y(t * 0.08f), T_MERCURY, 0, ZERO, 0);
}

static void scene_venus(float t)
{
    camera(3.5f, 0.2f + t * 0.03f, 0.15f);
    sun_from(v3(-1.0f, 0.3f, -0.4f));
    add_body(ZERO, 1.0f, rot_y(-t * 0.05f), T_VENUS, 0.07f, v3(1.0f, 0.85f, 0.55f), 0);
}

static void scene_earth(float t)
{
    camera(3.8f, -0.2f + t * 0.035f, 0.12f);
    sun_from(v3(-1.0f, 0.25f, -0.45f));
    M3 tilt = rot_z(0.41f);
    add_body(ZERO, 1.0f, m3_mul(tilt, rot_y(t * 0.12f)), T_EARTH, 0.045f, v3(0.35f, 0.6f, 1.0f), t * 0.03f);
    add_moon(m3_mul(tilt, rot_x(0.09f)), 2.4f, 2.2f + t * 0.12f, 0.27f, T_MOON, 0, ZERO);
}

static void scene_mars(float t)
{
    camera(3.5f, 0.1f + t * 0.03f, 0.10f);
    sun_from(v3(-1.0f, 0.2f, -0.5f));
    M3 tilt = rot_z(0.44f);
    add_body(ZERO, 1.0f, m3_mul(tilt, rot_y(t * 0.12f)), T_MARS, 0.03f, v3(0.95f, 0.6f, 0.45f), 0);
    add_moon(tilt, 1.9f, t * 0.6f, 0.09f, T_ROCK, 0, ZERO);            /* Phobos */
    add_moon(tilt, 2.5f, 1.0f + t * 0.3f, 0.065f, T_ROCK, 0, ZERO);    /* Deimos */
}

static void scene_jupiter(float t)
{
    camera(5.0f, -0.25f + t * 0.03f, 0.10f);
    sun_from(v3(-1.0f, 0.1f, -0.5f));
    M3 tilt = rot_z(0.05f);
    add_body(ZERO, 1.0f, m3_mul(tilt, rot_y(t * 0.15f)), T_JUPITER, 0.02f, v3(0.95f, 0.85f, 0.7f), 0);
    add_moon(tilt, 1.7f, 0.5f + t * 0.40f, 0.09f, T_IO, 0, ZERO);
    add_moon(tilt, 2.2f, 2.0f + t * 0.28f, 0.08f, T_EUROPA, 0, ZERO);
    add_moon(tilt, 2.8f, 3.6f + t * 0.19f, 0.13f, T_GANYMEDE, 0, ZERO);
    add_moon(tilt, 3.4f, 5.0f + t * 0.13f, 0.12f, T_CALLISTO, 0, ZERO);
}

static void scene_saturn(float t)
{
    camera(5.2f, -0.3f + t * 0.025f, 0.18f);
    sun_from(v3(-1.0f, 0.45f, -0.5f));
    M3 tilt = m3_mul(rot_z(0.2f), rot_x(-0.35f));
    add_body(ZERO, 1.0f, m3_mul(tilt, rot_y(t * 0.14f)), T_SATURN, 0.02f, v3(0.95f, 0.85f, 0.6f), 0);
    add_ring(ZERO, m3_apply(tilt, v3(0, 1, 0)), 1.0f, RING_SATURN);
    add_moon(m3_mul(tilt, rot_x(0.05f)), 3.4f, 4.0f + t * 0.1f, 0.09f, T_TITAN, 0.15f, v3(0.95f, 0.65f, 0.3f));
}

static void scene_uranus(float t)
{
    camera(4.0f, 0.2f + t * 0.03f, 0.10f);
    sun_from(v3(-1.0f, 0.2f, -0.6f));
    M3 tilt = m3_mul(rot_y(-0.6f), rot_z(1.71f));      /* spins on its side */
    add_body(ZERO, 1.0f, m3_mul(tilt, rot_y(t * 0.1f)), T_URANUS, 0.04f, v3(0.6f, 0.9f, 0.95f), 0);
    add_ring(ZERO, m3_apply(tilt, v3(0, 1, 0)), 1.0f, RING_URANUS);
    add_moon(tilt, 2.5f, 1.0f + t * 0.2f, 0.07f, T_ICE, 0, ZERO);      /* Titania */
    add_moon(tilt, 3.0f, 3.5f + t * 0.15f, 0.065f, T_ICE, 0, ZERO);    /* Oberon */
}

static void scene_neptune(float t)
{
    camera(3.6f, -0.1f + t * 0.03f, 0.12f);
    sun_from(v3(-1.0f, 0.2f, -0.45f));
    M3 tilt = rot_z(0.49f);
    add_body(ZERO, 1.0f, m3_mul(tilt, rot_y(t * 0.13f)), T_NEPTUNE, 0.04f, v3(0.35f, 0.55f, 1.0f), 0);
    add_moon(m3_mul(tilt, rot_x(2.7f)), 2.4f, t * 0.2f, 0.11f, T_TRITON, 0, ZERO);  /* orbits backwards */
}

static const struct {
    float orbit, r, phase, tilt, atmo;
    int tex;
    V3 atmo_col;
} g_sys[8] = {
    { 1.7f, 0.15f, 0.3f, 0.00f, 0.00f, T_MERCURY, { 0, 0, 0 } },
    { 2.4f, 0.24f, 2.1f, 0.00f, 0.08f, T_VENUS,   { 1.0f, 0.85f, 0.55f } },
    { 3.2f, 0.25f, 4.0f, 0.41f, 0.06f, T_EARTH,   { 0.35f, 0.6f, 1.0f } },
    { 4.0f, 0.19f, 5.5f, 0.44f, 0.03f, T_MARS,    { 0.95f, 0.6f, 0.45f } },
    { 5.3f, 0.60f, 1.2f, 0.05f, 0.02f, T_JUPITER, { 0.95f, 0.85f, 0.7f } },
    { 6.8f, 0.50f, 3.3f, 0.47f, 0.02f, T_SATURN,  { 0.95f, 0.85f, 0.6f } },
    { 8.0f, 0.36f, 0.8f, 1.71f, 0.04f, T_URANUS,  { 0.6f, 0.9f, 0.95f } },
    { 9.0f, 0.35f, 5.0f, 0.49f, 0.04f, T_NEPTUNE, { 0.35f, 0.55f, 1.0f } },
};

static float sys_angle(int i, float t)
{
    return g_sys[i].phase + t * 0.6f * powf(1.7f / g_sys[i].orbit, 1.5f);   /* Kepler */
}

static void scene_system(float t)
{
    camera(13.5f, 0.3f + t * 0.03f, 0.5f);
    g_point_light = 1;
    g_sun_pos = m3_apply(g_view, sub(ZERO, g_eye));
    add_body(ZERO, 1.1f, rot_y(t * 0.05f), T_SUN, 1.6f, v3(1.0f, 0.55f, 0.18f), 0);
    for (int i = 0; i < 8; i++) {
        float a = sys_angle(i, t);
        V3 pos = v3(g_sys[i].orbit * cosf(a), 0, g_sys[i].orbit * sinf(a));
        M3 tilt = rot_z(g_sys[i].tilt);
        add_body(pos, g_sys[i].r, m3_mul(tilt, rot_y(t * 0.3f)), g_sys[i].tex,
                 g_sys[i].atmo, g_sys[i].atmo_col, 0);
        if (g_sys[i].tex == T_SATURN)
            add_ring(pos, m3_apply(tilt, v3(0, 1, 0)), g_sys[i].r, RING_SATURN);
    }
}

/* faint orbit lines, drawn on the sky after tracing */
static void overlay_system(float t)
{
    (void)t;
    for (int i = 0; i < 8; i++) {
        float r = g_sys[i].orbit;
        int n = (int)(2 * PI * r * g_focal / 13.5f * 3.0f) + 16;
        for (int k = 0; k < n; k++) {
            float a = k * 2 * PI / n;
            splat_point(m3_apply(g_view, sub(v3(r * cosf(a), 0, r * sinf(a)), g_eye)),
                        v3(0.035f, 0.04f, 0.055f));
        }
    }
}

typedef struct {
    const char *name, *fact;
    void (*setup)(float t);
    void (*overlay)(float t);
} Scene;

static const Scene g_scenes[] = {
    { "MERCURY", "closest to the Sun - a year there lasts only 88 days", scene_mercury, NULL },
    { "VENUS", "hottest planet - 465 C under clouds of sulfuric acid", scene_venus, NULL },
    { "EARTH", "our home - 71% of the surface is ocean", scene_earth, NULL },
    { "MARS", "the red planet - home of Olympus Mons, the tallest volcano", scene_mars, NULL },
    { "JUPITER", "largest planet - the Great Red Spot is wider than Earth", scene_jupiter, NULL },
    { "SATURN", "rings reach 282,000 km out, but are only about 10 m thick", scene_saturn, NULL },
    { "URANUS", "tipped on its side - it spins with a 98 degree tilt", scene_uranus, NULL },
    { "NEPTUNE", "windiest planet - storms faster than 2,000 km/h", scene_neptune, NULL },
    { "SOLAR SYSTEM", "8 planets, 4.6 billion years old", scene_system, overlay_system },
};
#define NSCENES ((int)(sizeof g_scenes / sizeof g_scenes[0]))

/* Ray trace one scene at time t into g_color / g_id. */
static void trace_row(int y)
{
    for (int x = 0; x < g_pw; x++) {
        int i = y * g_pw + x;
        g_color[i] = trace(pixel_ray(x + 0.5f, y + 0.5f), &g_id[i]);
    }
}

/* anti-aliasing: four more rays where different objects meet */
static void smooth_row(int y)
{
    static const float aa[4][2] = { { 0.375f, 0.125f }, { 0.875f, 0.375f },
                                    { 0.625f, 0.875f }, { 0.125f, 0.625f } };
    for (int x = 0; x < g_pw; x++) {
        int i = y * g_pw + x, id = g_id[i], dummy;
        if (!((x > 0 && g_id[i - 1] != id) || (x + 1 < g_pw && g_id[i + 1] != id) ||
              (y > 0 && g_id[i - g_pw] != id) || (y + 1 < g_ph && g_id[i + g_pw] != id)))
            continue;
        V3 sum = g_color[i];
        for (int k = 0; k < 4; k++)
            sum = add(sum, trace(pixel_ray(x + aa[k][0], y + aa[k][1]), &dummy));
        g_color[i] = scale(sum, 0.2f);
    }
}

/*
 * Rows are split between threads: thread k does rows k, k + n, k + 2n, ...
 * so the expensive rows through a planet are shared evenly. Every pixel only
 * reads the scene and writes itself, so the threads never collide.
 */
static int g_nthreads = 1;

typedef struct { int first, pass; } Job;

static void *worker(void *arg)
{
    const Job *job = arg;
    for (int y = job->first; y < g_ph; y += g_nthreads) {
        if (job->pass == 0)
            trace_row(y);
        else
            smooth_row(y);
    }
    return NULL;
}

static void run_pass(int pass)
{
    pthread_t th[MAXTHREADS];
    Job job[MAXTHREADS];
    int started[MAXTHREADS] = { 0 };
    for (int k = 0; k < g_nthreads; k++)
        job[k] = (Job){ k, pass };
    for (int k = 1; k < g_nthreads; k++)
        started[k] = pthread_create(&th[k], NULL, worker, &job[k]) == 0;
    worker(&job[0]);                /* the main thread takes the first share */
    for (int k = 1; k < g_nthreads; k++) {
        if (started[k])
            pthread_join(th[k], NULL);
        else
            worker(&job[k]);        /* no thread: do that share here */
    }
}

static void render_scene(int s, float t)
{
    g_time = t;
    g_nbody = g_nring = 0;
    g_point_light = 0;
    g_scenes[s].setup(t);

    run_pass(0);                    /* all edges must be known before smoothing them */
    run_pass(1);

    draw_stars();
    if (g_scenes[s].overlay)
        g_scenes[s].overlay(t);
}

/* ------------------------------------------------------------------ */
/* terminal output                                                     */
/* ------------------------------------------------------------------ */

typedef struct { int x, y; char text[64]; V3 col; float alpha; } Span;
static Span g_spans[4];
static int  g_nspans;

/* Text overlay that fades with alpha; negative x/y count from the right/bottom. */
static void hud(int x, int y, V3 col, float alpha, const char *text)
{
    if (g_nspans >= (int)(sizeof g_spans / sizeof g_spans[0]))
        return;
    Span *s = &g_spans[g_nspans++];
    s->x = x < 0 ? g_cols + x : x;
    s->y = y < 0 ? g_rows + y : y;
    s->col = col;
    s->alpha = alpha;
    snprintf(s->text, sizeof s->text, "%s", text);
}

typedef struct { uint32_t fg, bg, ch; } Cell;

static Cell  *g_cells, *g_prev;
static int    g_full_redraw;
static char  *g_out;
static size_t g_out_len, g_out_cap;

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);    /* realloc(p, 0) may free p: never ask for 0 */
    if (!q) {
        fputs("Planets: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static void out(const char *s, size_t n)
{
    if (g_out_len + n > g_out_cap) {
        size_t cap = g_out_cap ? g_out_cap : 1 << 16;
        while (cap < g_out_len + n)
            cap *= 2;
        g_out = xrealloc(g_out, cap);
        g_out_cap = cap;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
}

static void outs(const char *s) { out(s, strlen(s)); }

static void outf(const char *fmt, ...)
{
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0)
        out(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static void flush_out(void)
{
    size_t off = 0;
    while (off < g_out_len) {
        ssize_t w = write(STDOUT_FILENO, g_out + off, g_out_len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)w;
    }
    g_out_len = 0;
}

/* nearest entry in the xterm 6x6x6 color cube or the 24-step gray ramp */
static uint32_t rgb256(int r, int g, int b)
{
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    int ir = r < 48 ? 0 : r < 115 ? 1 : (r - 35) / 40;
    int ig = g < 48 ? 0 : g < 115 ? 1 : (g - 35) / 40;
    int ib = b < 48 ? 0 : b < 115 ? 1 : (b - 35) / 40;
    int avg = (r + g + b) / 3;
    int gi = avg > 238 ? 23 : avg < 3 ? 0 : (avg - 3) / 10;
    int gray = 8 + 10 * gi;
    int dc = (lv[ir] - r) * (lv[ir] - r) + (lv[ig] - g) * (lv[ig] - g) + (lv[ib] - b) * (lv[ib] - b);
    int dg = (gray - r) * (gray - r) + (gray - g) * (gray - g) + (gray - b) * (gray - b);
    return dg < dc ? (uint32_t)(232 + gi) : (uint32_t)(16 + 36 * ir + 6 * ig + ib);
}

static uint32_t pack(V3 c, float k)
{
    int r = (int)(clampf(c.x * k, 0, 1) * 255 + 0.5f);
    int g = (int)(clampf(c.y * k, 0, 1) * 255 + 0.5f);
    int b = (int)(clampf(c.z * k, 0, 1) * 255 + 0.5f);
    return g_256 ? rgb256(r, g, b) : (uint32_t)(r << 16 | g << 8 | b);
}

static void color_arg(char *buf, size_t n, int bg, uint32_t c)
{
    if (g_256)
        snprintf(buf, n, "%d;5;%u", bg ? 48 : 38, c);
    else
        snprintf(buf, n, "%d;2;%u;%u;%u", bg ? 48 : 38, c >> 16, (c >> 8) & 255, c & 255);
}

static void emit_char(uint32_t cp)
{
    char b[3];
    if (cp < 0x80) {
        b[0] = (char)cp;
        out(b, 1);
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | cp >> 6);
        b[1] = (char)(0x80 | (cp & 0x3F));
        out(b, 2);
    } else {
        b[0] = (char)(0xE0 | cp >> 12);
        b[1] = (char)(0x80 | (cp >> 6 & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        out(b, 3);
    }
}

/* Turn the pixel buffer into cells and send only the cells that changed. */
static void present(float fade)
{
    static const char ramp[] = " .,-~:;=!*#$@";
    const int nr = (int)sizeof ramp - 2;
    uint32_t black = pack(ZERO, 1);

    for (int cy = 0; cy < g_rows; cy++)
        for (int cx = 0; cx < g_cols; cx++) {
            Cell *cell = &g_cells[cy * g_cols + cx];
            if (!g_ascii) {
                uint32_t top = pack(g_color[(2 * cy) * g_pw + cx], fade);
                uint32_t bot = pack(g_color[(2 * cy + 1) * g_pw + cx], fade);
                *cell = top == bot ? (Cell){ FG_ANY, bot, ' ' } : (Cell){ top, bot, 0x2580 };
                continue;
            }
            int i = cy * g_pw + cx;
            V3 c = g_color[i];
            float m = fmaxf(c.x, fmaxf(c.y, c.z));
            if (g_id[i] == -1 && m < 0.15f) {         /* empty sky: just its color */
                *cell = (Cell){ FG_ANY, pack(c, fade), ' ' };
                continue;
            }
            /* the character carries the brightness, so keep the color itself bright */
            if (m > 1e-3f)
                c = scale(c, fminf(1.0f, 0.35f + m) / m);
            int k = 1 + (int)(clampf(m, 0, 1) * (nr - 1) + 0.5f);
            *cell = (Cell){ pack(c, fade), black, (uint32_t)ramp[k] };
        }

    for (int s = 0; s < g_nspans; s++) {
        const Span *sp = &g_spans[s];
        if (sp->y < 0 || sp->y >= g_rows)
            continue;
        for (int j = 0; sp->text[j]; j++) {
            int x = sp->x + j;
            if (x < 0 || x >= g_cols)
                continue;
            V3 under = g_ascii ? g_color[sp->y * g_pw + x]
                               : scale(add(g_color[2 * sp->y * g_pw + x], g_color[(2 * sp->y + 1) * g_pw + x]), 0.5f);
            under = scale(under, fade * (1 - 0.5f * sp->alpha));    /* darken behind the text */
            g_cells[sp->y * g_cols + x] =
                (Cell){ pack(mix(under, sp->col, sp->alpha), 1), pack(under, 1), (unsigned char)sp->text[j] };
        }
    }

    uint32_t cur_fg = FG_ANY, cur_bg = FG_ANY;
    int cur_x = -1, cur_y = -1;
    outs("\x1b[?2026h");            /* synchronized update: no tearing */
    if (g_full_redraw)
        outs("\x1b[0m\x1b[2J");
    for (int cy = 0; cy < g_rows; cy++)
        for (int cx = 0; cx < g_cols; cx++) {
            int i = cy * g_cols + cx;
            const Cell *c = &g_cells[i];
            if (!g_full_redraw && memcmp(c, &g_prev[i], sizeof *c) == 0)
                continue;
            if (cy == cur_y && cx > cur_x && cx - cur_x <= 4)
                outf("\x1b[%dC", cx - cur_x);       /* short hop to the right */
            else if (cx != cur_x || cy != cur_y)
                outf("\x1b[%d;%dH", cy + 1, cx + 1);

            /* one escape sequence for both colors when both change */
            char a[32] = "", b[32] = "";
            if (c->fg != FG_ANY && c->fg != cur_fg) {
                color_arg(a, sizeof a, 0, c->fg);
                cur_fg = c->fg;
            }
            if (c->bg != cur_bg) {
                color_arg(b, sizeof b, 1, c->bg);
                cur_bg = c->bg;
            }
            if (a[0] || b[0])
                outf("\x1b[%s%s%sm", a, a[0] && b[0] ? ";" : "", b);
            emit_char(c->ch);
            cur_x = cx + 1;
            cur_y = cy;
        }
    outs("\x1b[?2026l");
    flush_out();

    memcpy(g_prev, g_cells, (size_t)g_cols * g_rows * sizeof *g_cells);
    g_full_redraw = 0;
}

/* ------------------------------------------------------------------ */
/* terminal setup, input, main loop                                    */
/* ------------------------------------------------------------------ */

static struct termios g_orig_tio;
static int g_have_tty_in;
static int g_anykey;
static volatile sig_atomic_t g_quit, g_resized;

static void term_restore(void)
{
    /* reset colors, end sync update, re-enable wrap and cursor, leave alt screen */
    const char *s = "\x1b[0m\x1b[?2026l\x1b[?7h\x1b[?25h\x1b[?1049l";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) {
        /* nothing sensible left to do */
    }
    if (g_have_tty_in)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_tio);
}

static void on_signal(int sig)
{
    if (sig == SIGWINCH)
        g_resized = 1;
    else
        g_quit = 1;
}

static void term_setup(void)
{
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_orig_tio) == 0) {
        struct termios raw = g_orig_tio;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);  /* keep ISIG: Ctrl-C still works */
        raw.c_cc[VMIN] = 0;                         /* read() never blocks */
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);     /* keep keys typed during startup */
        g_have_tty_in = 1;
    }
    atexit(term_restore);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);   /* the terminal window was closed */
    sigaction(SIGWINCH, &sa, NULL);

    /* alt screen, hide cursor, no auto-wrap */
    outs("\x1b[?1049h\x1b[?25l\x1b[?7l");
    flush_out();
}

static void alloc_buffers(void)
{
    struct winsize ws;
    int cols = 80, rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    g_cols = cols;
    g_rows = rows;
    g_pw = cols;
    g_ph = g_ascii ? rows : rows * 2;
    g_yasp = g_ascii ? 0.5f : 1.0f;     /* a character cell is about twice as tall as wide */
    g_focal = fminf((float)g_pw, g_ph / g_yasp);

    size_t np = (size_t)g_pw * g_ph, nc = (size_t)cols * rows;
    g_color = xrealloc(g_color, np * sizeof *g_color);
    g_color2 = xrealloc(g_color2, np * sizeof *g_color2);
    g_id = xrealloc(g_id, np * sizeof *g_id);
    g_cells = xrealloc(g_cells, nc * sizeof *g_cells);
    g_prev = xrealloc(g_prev, nc * sizeof *g_prev);
    g_full_redraw = 1;
}

/* Returns how many scenes to move: +1 per next key, -1 per previous key. */
static int poll_keys(int *paused, float *speed)
{
    unsigned char buf[64];
    if (!g_have_tty_in)
        return 0;
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if (n <= 0)
        return 0;
    if (g_anykey) {
        g_quit = 1;
        return 0;
    }

    int step = 0;
    for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == 27) {
            if (i + 2 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
                if (buf[i + 2] == 'C')
                    step++;
                else if (buf[i + 2] == 'D')
                    step--;
                i += 2;
            } else if (i + 1 == n) {
                g_quit = 1;         /* a lone Esc */
            }
            continue;
        }
        switch (c) {
        case 'q': case 'Q': g_quit = 1; break;
        case 'n': step++; break;
        case 'b': step--; break;
        case ' ': *paused = !*paused; break;
        case 'm': g_ascii = !g_ascii; alloc_buffers(); break;
        case 'c': g_256 = !g_256; g_full_redraw = 1; break;
        case '+': case '=': *speed = fminf(*speed * 1.25f, 8.0f); break;
        case '-': case '_': *speed = fmaxf(*speed / 1.25f, 0.125f); break;
        }
    }
    return step;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: Planets [options]\n"
        "  -t SEC   seconds per planet (default 25, 0 = stay on one)\n"
        "  -s N     start with scene N:\n"
        "             1 MERCURY  2 VENUS   3 EARTH    4 MARS   5 JUPITER\n"
        "             6 SATURN   7 URANUS  8 NEPTUNE  9 SOLAR SYSTEM\n"
        "  -r       random order instead of the tour from the Sun outwards\n"
        "  -f FPS   frames per second (default 60; try 30 if it stutters)\n"
        "  -a       ASCII shading instead of half-block pixels\n"
        "  -2       256 colors, for terminals without 24-bit color\n"
        "  -x       screensaver mode: any key quits\n"
        "  -h       show this help\n"
        "\n"
        "Runs forever until you quit or close the terminal.\n"
        "keys: q/Esc/Ctrl-C quit   n/Right next   b/Left previous   space pause\n"
        "      m ASCII/blocks   c 256/24-bit colors   +/- speed\n");
}

int main(int argc, char **argv)
{
    float scene_len = 25, fps = 60;
    int scene = 0, shuffle = 0, opt;

    while ((opt = getopt(argc, argv, "t:s:rf:a2xh")) != -1) {
        switch (opt) {
        case 't': scene_len = strtof(optarg, NULL); break;
        case 's':
            scene = atoi(optarg) - 1;
            if (scene < 0 || scene >= NSCENES) {
                fprintf(stderr, "Planets: scene must be 1-%d\n", NSCENES);
                return 1;
            }
            break;
        case 'r': shuffle = 1; break;
        case 'f': fps = strtof(optarg, NULL); break;
        case 'a': g_ascii = 1; break;
        case '2': g_256 = 1; break;
        case 'x': g_anykey = 1; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 1;
        }
    }
    fps = clampf(fps, 1, 120);
    if (!isatty(STDOUT_FILENO)) {
        fputs("Planets: stdout is not a terminal\n", stderr);
        return 1;
    }

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    g_nthreads = cores < 1 ? 1 : cores > MAXTHREADS ? MAXTHREADS : (int)cores;

    srand((unsigned)time(NULL));
    term_setup();                   /* first, so the screen goes dark right away */
    init_tables();
    alloc_buffers();

    float st = 0, pt = 0, speed = 1, age = 0;
    int prev = -1, paused = 0;
    double last = now(), next_frame = last;
    while (!g_quit) {
        double frame_start = now();
        float dt = fminf((float)(frame_start - last), 0.1f);
        last = frame_start;
        age += dt;

        int step = poll_keys(&paused, &speed);
        if (g_resized) {
            g_resized = 0;
            alloc_buffers();
        }

        float sdt = paused ? 0 : dt * speed;
        st += sdt;
        pt += sdt;
        int next = -1;
        if (step) {
            next = ((scene + step) % NSCENES + NSCENES) % NSCENES;
        } else if (scene_len > 0 && st > scene_len) {
            next = (scene + 1) % NSCENES;
            while (shuffle && (next = rand() % NSCENES) == scene)
                ;
        }
        if (next >= 0 && next != scene) {
            prev = scene;
            pt = st;
            scene = next;
            st = 0;
        }

        /* crossfade: the old scene keeps moving while the new one appears */
        if (prev >= 0 && st < XFADE) {
            render_scene(prev, pt);
            memcpy(g_color2, g_color, (size_t)g_pw * g_ph * sizeof *g_color);
            render_scene(scene, st);
            float a = smooth(0, XFADE, st);
            for (int i = 0; i < g_pw * g_ph; i++)
                g_color[i] = mix(g_color2[i], g_color[i], a);
        } else {
            prev = -1;
            render_scene(scene, st);
        }

        g_nspans = 0;
        float alpha = paused ? 1.0f : smooth(0.3f, 1.3f, st) * (1 - smooth(7.0f, 8.5f, st));
        if (alpha > 0.01f) {
            char label[64];
            snprintf(label, sizeof label, " %d/%d  %s%s ", scene + 1, NSCENES,
                     g_scenes[scene].name, paused ? "  (paused)" : "");
            hud(1, 1, v3(1, 1, 1), alpha, label);
            snprintf(label, sizeof label, " %s ", g_scenes[scene].fact);
            hud(1, -2, v3(0.7f, 0.8f, 1.0f), alpha, label);
        }
        present(smooth(0.0f, 1.2f, age));   /* fade in from black at startup */

        /* steady frame pacing: aim at fixed ticks instead of sleeping a fixed time */
        next_frame += 1.0 / fps;
        double rest = next_frame - now();
        if (rest > 0) {
            struct timespec ts = { (time_t)rest, (long)((rest - (double)(time_t)rest) * 1e9) };
            nanosleep(&ts, NULL);
        } else if (rest < -0.25) {
            next_frame = now();     /* fell far behind: do not try to catch up */
        }
    }
    return 0;
}
