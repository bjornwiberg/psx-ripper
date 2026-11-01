// Minimal software-rendered UI: draws straight into the RSX colour buffer before each flip.
#ifndef PSXRIP_UI_H
#define PSXRIP_UI_H

#include <stdint.h>

enum UiFont { UI_SMALL, UI_BODY, UI_TITLE };
enum UiAlign { UI_LEFT, UI_CENTER, UI_RIGHT };

void ui_init();
void ui_blit();                              // copy the finished canvas to the RSX back buffer
const uint32_t *ui_canvas();
int ui_w();
int ui_h();
int ui_line(UiFont f);                       // line height in pixels
void ui_background();                        // full-screen gradient
void ui_rect(int x, int y, int w, int h, uint32_t argb);
int ui_text_width(UiFont f, const char *utf8);
int ui_text(UiFont f, int x, int y, uint32_t rgb, const char *utf8, UiAlign align = UI_LEFT);  // y = top
// Draws a PS button symbol ("cross", "circle", "triangle", "square") followed by a label; returns width.
int ui_hint(int x, int y, const char *button, const char *label);
int ui_hint_width(const char *button, const char *label);

#endif
