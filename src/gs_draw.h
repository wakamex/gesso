// Smooth shapes on an SDL renderer: rounded rectangles filled and outlined, discs, strokes with
// round ends and triangles, each with a one-pixel rim that fades out so its edge is not stepped.
// Coordinates are pixels and colours blend over what is there, so a colour's alpha is its opacity.
// gs_ui draws its controls with these and takes points; use them directly for a window drawn
// without gs_ui (an overlay, a game's interface).
#pragma once
#include <SDL3/SDL.h>

void gs_draw_round(SDL_Renderer *r, SDL_FRect rect, float radius, SDL_FColor c);
// The outline of a rounded rectangle, `width` thick, inside its edge.
void gs_draw_outline(SDL_Renderer *r, SDL_FRect rect, float radius, float width, SDL_FColor c);
void gs_draw_circle(SDL_Renderer *r, float cx, float cy, float radius, SDL_FColor c);
// A straight stroke from one point to another, `width` thick, with round ends.
void gs_draw_line(SDL_Renderer *r, float x0, float y0, float x1, float y1, float width, SDL_FColor c);
void gs_draw_triangle(SDL_Renderer *r, float x0, float y0, float x1, float y1, float x2, float y2, SDL_FColor c);

SDL_FColor gs_rgba(unsigned rgb, float alpha);  // 0xRRGGBB and an opacity
// Progress t in [0, 1] eased to start fast and settle (a cubic ease-out), for a value moving from
// one place to another over a fixed time: from + (to - from) * gs_ease(elapsed / duration).
float gs_ease(float t);
