/*
 * boing.c - the Amiga Boing Ball, as native m68k AmigaOS code.
 *
 * No Amiga repo is complete without it. Opens a custom screen, draws the
 * classic purple grid, a red-and-white checkered sphere with a curved
 * (spherical) checker, and a shadow. Renders one composed frame and holds
 * it so it can be seen / screenshotted.
 *
 * Pure graphics.library + intuition.library, C89, builds with the same
 * bebbo m68k-amigaos cross toolchain as the rest of this repo (-m68020).
 */

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>

struct IntuitionBase *IntuitionBase = 0;
struct GfxBase *GfxBase = 0;

/* integer sqrt */
static long isqrt(long n)
{
    long x, r = 0, b;
    if (n <= 0) return 0;
    b = 1;
    while (b * 4 <= n) b *= 4;
    while (b) {
        x = r + b;
        r >>= 1;
        if (n >= x) { n -= x; r += b; }
        b >>= 2;
    }
    return r;
}

/* filled horizontal-scan ellipse (shadow), centre cx,cy, radii a (x) b (y) */
static void fill_ellipse(struct RastPort *rp, int cx, int cy, int a, int b)
{
    int dy;
    for (dy = -b; dy <= b; dy++) {
        /* half-width w where (dy/b)^2 + (w/a)^2 = 1 */
        long inside = (long)a * a * (b * b - dy * dy);
        int w;
        if (inside < 0) continue;
        w = (int)(isqrt(inside) / b);
        Move(rp, cx - w, cy + dy);
        Draw(rp, cx + w, cy + dy);
    }
}

int main(void)
{
    struct Screen *scr;
    struct RastPort *rp;
    struct ViewPort *vp;
    int x, y, dx, dy, cx, cy, R, spin;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 0);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 0);
    if (!IntuitionBase || !GfxBase) return 20;

    scr = OpenScreenTags(0,
        SA_Width,  320,
        SA_Height, 256,
        SA_Depth,  4,
        SA_Type,   (ULONG)CUSTOMSCREEN,
        SA_Title,  (ULONG)"Boing - RustChain Amiga Edition",
        TAG_END);
    if (!scr) {
        if (GfxBase) CloseLibrary((struct Library *)GfxBase);
        if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
        return 20;
    }
    rp = &scr->RastPort;
    vp = &scr->ViewPort;

    /* palette: 0 grey bg, 1 white, 2 red, 3 purple grid, 4 shadow */
    SetRGB4(vp, 0, 10, 10, 10);
    SetRGB4(vp, 1, 15, 15, 15);
    SetRGB4(vp, 2, 15,  1,  1);
    SetRGB4(vp, 3, 11,  0, 11);
    SetRGB4(vp, 4,  6,  6,  6);

    /* grey field */
    SetAPen(rp, 0);
    RectFill(rp, 0, 0, 319, 255);

    /* the classic purple grid */
    SetAPen(rp, 3);
    for (x = 0; x <= 320; x += 20) { Move(rp, x, 0);  Draw(rp, x, 255); }
    for (y = 0; y <= 256; y += 20) { Move(rp, 0, y);  Draw(rp, 319, y); }

    /* ball placement (mid-bounce) */
    cx = 178; cy = 96; R = 66; spin = 40;

    /* shadow on the floor, offset to the right, squashed */
    SetAPen(rp, 4);
    fill_ellipse(rp, cx + 40, 214, R - 6, 16);

    /* the checkered sphere: curved (spherical) checker via z from isqrt */
    for (dy = -R; dy <= R; dy++) {
        for (dx = -R; dx <= R; dx++) {
            long r2 = (long)dx * dx + (long)dy * dy;
            long z, lon, lat;
            int cell;
            if (r2 > (long)R * R) continue;
            z = isqrt((long)R * R - r2);
            lon = ((long)dx * 140) / (z + 10);   /* curves toward the edges */
            lat = ((long)dy * 96) / R;
            cell = (int)((((lon + spin) / 16) + (lat / 16)) & 1);
            SetAPen(rp, cell ? 2 : 1);
            WritePixel(rp, cx + dx, cy + dy);
        }
    }

    /* hold the frame so it can be seen / screenshotted (~14s) */
    Delay(700);

    CloseScreen(scr);
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
