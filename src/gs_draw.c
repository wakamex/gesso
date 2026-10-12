#include "gs_draw.h"

#include <math.h>

#include "stb_image_write.h"

#define ARC 12                  // the most segments in a quarter circle
#define POINTS (4 * (ARC + 1))  // the most points in an outline

SDL_FColor gs_rgba(unsigned rgb, float alpha) {
    return (SDL_FColor){ (float)(rgb >> 16 & 0xff) / 255, (float)(rgb >> 8 & 0xff) / 255, (float)(rgb & 0xff) / 255, alpha };
}

float gs_ease(float t) {
    t = 1 - SDL_clamp(t, 0, 1);
    return 1 - t * t * t;
}

static int arcs(float r) { return r < 0.5f ? 0 : SDL_clamp((int)(r / 1.5f) + 2, 3, ARC); }  // (segments about two pixels long)

// The outline of a rounded rectangle as 4 * (arc + 1) points, clockwise, `arc` segments to a corner.
static int ring(SDL_FRect f, float r, int arc, SDL_FPoint *p) {
    r = SDL_max(0, SDL_min(r, SDL_min(f.w, f.h) / 2));
    const float cx[4] = { f.x + f.w - r, f.x + f.w - r, f.x + r, f.x + r }, cy[4] = { f.y + r, f.y + f.h - r, f.y + f.h - r, f.y + r };
    int n = 0;
    for (int corner = 0; corner < 4; corner++)
        for (int i = 0; i <= arc; i++) {
            float a = ((float)corner - 1 + (arc ? (float)i / (float)arc : 0)) * 1.5707963f;
            p[n++] = (SDL_FPoint){ cx[corner] + cosf(a) * r, cy[corner] + sinf(a) * r };
        }
    return n;
}

// The same without points that repeat, as a convex shape's corners. Returns how many.
static int outline(SDL_FRect f, float r, SDL_FPoint *p) {
    SDL_FPoint all[POINTS];
    int count = ring(f, r, arcs(SDL_min(r, SDL_min(f.w, f.h) / 2)), all), n = 0;
    for (int i = 0; i < count; i++)
        if (!n || fabsf(all[i].x - p[n - 1].x) + fabsf(all[i].y - p[n - 1].y) >= 0.01f) p[n++] = all[i];
    if (n > 1 && fabsf(p[0].x - p[n - 1].x) + fabsf(p[0].y - p[n - 1].y) < 0.01f) n--;
    return n;
}

// The points of a convex outline moved `d` pixels outward (inward when negative) along the corner's
// bisector, so that each edge moves by d.
static void offset(const SDL_FPoint *p, int n, float d, SDL_FPoint *out) {
    for (int i = 0; i < n; i++) {
        SDL_FPoint a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
        float ax = b.x - a.x, ay = b.y - a.y, bx = c.x - b.x, by = c.y - b.y;
        float al = SDL_max(1e-6f, sqrtf(ax * ax + ay * ay)), bl = SDL_max(1e-6f, sqrtf(bx * bx + by * by));
        // The outward normals of the two edges at this point (clockwise with y down: turn left).
        float nx = ay / al + by / bl, ny = -ax / al - bx / bl, nl = SDL_max(1e-6f, sqrtf(nx * nx + ny * ny));
        nx /= nl, ny /= nl;
        float along = SDL_max(0.5f, nx * (ay / al) + ny * (-ax / al));  // the cosine of half the turn; a sharp corner's reach is held to twice d
        out[i] = (SDL_FPoint){ b.x + nx * d / along, b.y + ny * d / along };
    }
}

static void geometry(SDL_Renderer *r, const SDL_Vertex *v, int nv, const int *idx, int ni) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(r, NULL, v, nv, idx, ni);
}

// A band between two outlines of n points each, as triangles.
static int band(int *idx, int k, int a, int b, int n) {
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        idx[k++] = a + i, idx[k++] = b + i, idx[k++] = b + j;
        idx[k++] = a + i, idx[k++] = b + j, idx[k++] = a + j;
    }
    return k;
}

// A convex shape filled, with a one-pixel rim that fades out.
static void fill_convex(SDL_Renderer *r, const SDL_FPoint *p, int n, SDL_FColor c) {
    if (n < 3 || c.a <= 0) return;
    SDL_Vertex v[2 * POINTS + 1];
    SDL_FPoint in[POINTS], out[POINTS];
    int idx[9 * POINTS], k = 0;
    float mx = 0, my = 0;
    for (int i = 0; i < n; i++) mx += p[i].x, my += p[i].y;
    offset(p, n, -0.5f, in), offset(p, n, 0.5f, out);
    SDL_FColor clear = c;
    clear.a = 0;
    v[0] = (SDL_Vertex){ { mx / (float)n, my / (float)n }, c, { 0, 0 } };
    for (int i = 0; i < n; i++) v[1 + i] = (SDL_Vertex){ in[i], c, { 0, 0 } }, v[1 + n + i] = (SDL_Vertex){ out[i], clear, { 0, 0 } };
    for (int i = 0; i < n; i++) idx[k++] = 0, idx[k++] = 1 + i, idx[k++] = 1 + (i + 1) % n;
    k = band(idx, k, 1, 1 + n, n);
    geometry(r, v, 1 + 2 * n, idx, k);
}

void gs_draw_round(SDL_Renderer *r, SDL_FRect rect, float radius, SDL_FColor c) {
    if (rect.w <= 0 || rect.h <= 0) return;
    SDL_FPoint p[POINTS];
    fill_convex(r, p, outline(rect, radius, p), c);
}

void gs_draw_circle(SDL_Renderer *r, float cx, float cy, float radius, SDL_FColor c) {
    gs_draw_round(r, (SDL_FRect){ cx - radius, cy - radius, radius * 2, radius * 2 }, radius, c);
}

void gs_draw_outline(SDL_Renderer *r, SDL_FRect rect, float radius, float width, SDL_FColor c) {
    if (rect.w <= 0 || rect.h <= 0 || width <= 0 || c.a <= 0) return;
    if (width * 2 >= SDL_min(rect.w, rect.h)) { gs_draw_round(r, rect, radius, c); return; }
    // Four rings: clear outside the edge, the colour across the stroke, clear inside it. A ring `d`
    // inside the edge is the rectangle and its radius both smaller by d, so a stroke wider than the
    // radius has square inner corners.
    SDL_FPoint p[POINTS];
    SDL_Vertex v[4 * POINTS];
    int idx[18 * POINTS], k = 0, arc = arcs(SDL_min(radius, SDL_min(rect.w, rect.h) / 2)), n = 4 * (arc + 1);
    const float in[4] = { -0.5f, 0.5f, width - 0.5f, width + 0.5f };
    SDL_FColor clear = c;
    clear.a = 0;
    for (int i = 0; i < 4; i++) {
        ring((SDL_FRect){ rect.x + in[i], rect.y + in[i], rect.w - 2 * in[i], rect.h - 2 * in[i] }, radius - in[i], arc, p);
        for (int j = 0; j < n; j++) v[i * n + j] = (SDL_Vertex){ p[j], i == 0 || i == 3 ? clear : c, { 0, 0 } };
    }
    for (int i = 0; i < 3; i++) k = band(idx, k, (i + 1) * n, i * n, n);
    geometry(r, v, 4 * n, idx, k);
}

void gs_draw_line(SDL_Renderer *r, float x0, float y0, float x1, float y1, float width, SDL_FColor c) {
    // A stadium: the segment swept by a disc. Built as a rounded rectangle's outline, turned.
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy), h = width / 2;
    if (len < 0.001f) { gs_draw_circle(r, x0, y0, h, c); return; }
    float ux = dx / len, uy = dy / len;
    SDL_FPoint flat[POINTS], p[POINTS];
    int n = outline((SDL_FRect){ -h, -h, len + width, width }, h, flat);
    for (int i = 0; i < n; i++) p[i] = (SDL_FPoint){ x0 + flat[i].x * ux - flat[i].y * uy, y0 + flat[i].x * uy + flat[i].y * ux };
    fill_convex(r, p, n, c);
}

void gs_draw_triangle(SDL_Renderer *r, float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c) {
    // (clockwise, whichever way it was given)
    bool clockwise = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0) > 0;
    const SDL_FPoint p[3] = { { x0, y0 }, clockwise ? (SDL_FPoint){ x1, y1 } : (SDL_FPoint){ x2, y2 }, clockwise ? (SDL_FPoint){ x2, y2 } : (SDL_FPoint){ x1, y1 } };
    fill_convex(r, p, 3, c);
}

bool gs_draw_save_png(SDL_Renderer *r, const char *path) {
    SDL_Surface *s = SDL_RenderReadPixels(r, NULL), *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
    bool ok = c && stbi_write_png(path, c->w, c->h, 4, c->pixels, c->pitch);
    if (c && !ok) SDL_SetError("could not write %s", path);
    SDL_DestroySurface(c), SDL_DestroySurface(s);
    return ok;
}
