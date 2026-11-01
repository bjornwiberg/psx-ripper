#include <string.h>
#include "rsxutil.h"
#include "ui.h"

struct Glyph { uint32_t cp; int16_t w, h, xoff, yoff, adv; uint32_t off; };
struct Font { const Glyph *glyphs; int count; const uint8_t *data; int height; };
#include "font.inc"

static const Font *fonts[4];
static uint32_t *row_bg;   // one precomputed background colour per scanline
static uint32_t *canvas;   // the UI is drawn here (cached RAM), never read back from VRAM

void ui_init()
{
	bool hd = display_height >= 1000;
	fonts[UI_SMALL] = hd ? &f1080_small : &f720_small;
	fonts[UI_BODY]  = hd ? &f1080_body  : &f720_body;
	fonts[UI_TITLE] = hd ? &f1080_title : &f720_title;
	fonts[UI_SYM]   = hd ? &f1080_sym   : &f720_sym;
	row_bg = new uint32_t[display_height];
	canvas = new uint32_t[display_width * display_height];
	for (u32 y = 0; y < display_height; y++) {
		float t = (float)y / display_height;   // deep navy to near black
		uint32_t r = 28 - (uint32_t)(14 * t), g = 34 - (uint32_t)(18 * t), b = 72 - (uint32_t)(40 * t);
		row_bg[y] = 0xFF000000 | (r << 16) | (g << 8) | b;
	}
}

int ui_w() { return display_width; }
int ui_h() { return display_height; }
int ui_line(UiFont f) { return fonts[f]->height; }

static inline uint32_t *fb_row(int y) { return canvas + y * display_width; }

void ui_blit()
{
	if (color_pitch == display_width * 4) {
		memcpy(color_buffer[curr_fb], canvas, display_width * display_height * 4);
	} else {
		for (u32 y = 0; y < display_height; y++)
			memcpy(color_buffer[curr_fb] + y * (color_pitch / 4), canvas + y * display_width, display_width * 4);
	}
}

const uint32_t *ui_canvas() { return canvas; }

void ui_background()
{
	for (u32 y = 0; y < display_height; y++) {
		uint32_t *p = fb_row(y), c = row_bg[y];
		for (u32 x = 0; x < display_width; x++) p[x] = c;
	}
}

void ui_rect(int x, int y, int w, int h, uint32_t argb)
{
	uint32_t a = argb >> 24;
	for (int j = y < 0 ? 0 : y; j < y + h && j < (int)display_height; j++) {
		uint32_t *p = fb_row(j);
		for (int i = x < 0 ? 0 : x; i < x + w && i < (int)display_width; i++) {
			if (a == 255) { p[i] = argb; continue; }
			uint32_t d = p[i];
			uint32_t r = (((argb >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
			uint32_t g = (((argb >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
			uint32_t b = ((argb & 255) * a + (d & 255) * (255 - a)) / 255;
			p[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
		}
	}
}

static uint32_t next_cp(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t c = *p++;
	if (c >= 0xF0)      { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
	else if (c >= 0xE0) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
	else if (c >= 0xC0) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
	*s = (const char *)p;
	return c;
}

static const Glyph *find_glyph(const Font *f, uint32_t cp)
{
	for (int i = 0; i < f->count; i++) if (f->glyphs[i].cp == cp) return &f->glyphs[i];
	for (int i = 0; i < f->count; i++) if (f->glyphs[i].cp == '?') return &f->glyphs[i];
	return 0;
}

int ui_text_width(UiFont fi, const char *s)
{
	int w = 0;
	while (*s) { const Glyph *g = find_glyph(fonts[fi], next_cp(&s)); if (g) w += g->adv; }
	return w;
}

static void draw_glyph(const Font *f, const Glyph *g, int x, int y, uint32_t rgb)
{
	const uint8_t *a = f->data + g->off;
	int stride = (g->w + 1) / 2;   // 4-bit alpha, two pixels per byte
	for (int j = 0; j < g->h; j++) {
		int py = y + g->yoff + j;
		if (py < 0 || py >= (int)display_height) continue;
		uint32_t *p = fb_row(py);
		for (int i = 0; i < g->w; i++) {
			int px = x + g->xoff + i;
			uint8_t nib = a[j * stride + i / 2];
			uint32_t al = ((i & 1) ? (nib & 15) : (nib >> 4)) * 17;
			if (!al || px < 0 || px >= (int)display_width) continue;
			uint32_t d = p[px];
			uint32_t r = (((rgb >> 16) & 255) * al + ((d >> 16) & 255) * (255 - al)) / 255;
			uint32_t gg = (((rgb >> 8) & 255) * al + ((d >> 8) & 255) * (255 - al)) / 255;
			uint32_t b = ((rgb & 255) * al + (d & 255) * (255 - al)) / 255;
			p[px] = 0xFF000000 | (r << 16) | (gg << 8) | b;
		}
	}
}

int ui_text(UiFont fi, int x, int y, uint32_t rgb, const char *s, UiAlign align)
{
	int w = ui_text_width(fi, s);
	if (align == UI_CENTER) x -= w / 2; else if (align == UI_RIGHT) x -= w;
	const Font *f = fonts[fi];
	while (*s) {
		uint32_t cp = next_cp(&s);
		const Glyph *g = find_glyph(f, cp);
		if (!g) continue;
		if (g->w) draw_glyph(f, g, x, y, rgb);
		x += g->adv;
	}
	return w;
}

static const char *hint_symbol(const char *button, uint32_t *col)
{
	*col = 0x7DA7D9;   // PlayStation button colours
	if (!strcmp(button, "circle"))   { *col = 0xFF6B6B; return "○"; }
	if (!strcmp(button, "triangle")) { *col = 0x40E0A0; return "△"; }
	if (!strcmp(button, "square"))   { *col = 0xE08CE0; return "□"; }
	if (!strcmp(button, "updown"))   { *col = 0xDDDDDD; return "↑↓"; }
	if (!strcmp(button, "leftright")){ *col = 0xDDDDDD; return "←→"; }
	return "✕";
}

int ui_hint_width(const char *button, const char *label)
{
	uint32_t col;
	return ui_text_width(UI_SYM, hint_symbol(button, &col)) + ui_text_width(UI_BODY, " ") + ui_text_width(UI_BODY, label);
}

int ui_hint(int x, int y, const char *button, const char *label)
{
	uint32_t col;
	const char *sym = hint_symbol(button, &col);
	// symbols are rasterised at cap height with their top at the cap line: same y as the label
	int w = ui_text(UI_SYM, x, y, col, sym);
	w += ui_text_width(UI_BODY, " ");
	w += ui_text(UI_BODY, x + w, y, 0xE8E8F0, label);
	return w;
}

