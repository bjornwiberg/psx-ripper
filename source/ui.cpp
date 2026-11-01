#include <string.h>
#include "rsxutil.h"
#include "ui.h"

struct Glyph { uint32_t cp; int16_t w, h, xoff, yoff, adv; uint32_t off; };
struct Font { const Glyph *glyphs; int count; const uint8_t *data; int height; };
#include "font.inc"

static const Font *fonts[5];
static uint32_t *row_bg;   // one precomputed background colour per scanline
static uint32_t *canvas;   // the UI is drawn here (cached RAM), never read back from VRAM

void ui_init()
{
	bool hd = display_height >= 1000;
	fonts[UI_SMALL] = hd ? &f1080_small : &f720_small;
	fonts[UI_BODY]  = hd ? &f1080_body  : &f720_body;
	fonts[UI_TITLE] = hd ? &f1080_title : &f720_title;
	fonts[UI_SYM]   = hd ? &f1080_sym   : &f720_sym;
	fonts[UI_BTN]   = hd ? &f1080_btn   : &f720_btn;
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

// Anti-aliased disc with a 2 px ring (button icons).
static void disc(float cx, float cy, float r, uint32_t fill, uint32_t ring)
{
	int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1), y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
	for (int y = y0; y <= y1; y++) {
		if (y < 0 || y >= (int)display_height) continue;
		uint32_t *p = fb_row(y);
		for (int x = x0; x <= x1; x++) {
			if (x < 0 || x >= (int)display_width) continue;
			float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			float d = __builtin_sqrtf(dx * dx + dy * dy);
			float cov = r + 0.5f - d;                    // coverage of the outer edge
			if (cov <= 0) continue;
			if (cov > 1) cov = 1;
			float inner = (r - 2.0f) + 0.5f - d;         // inside the ring
			uint32_t col = inner >= 1 ? fill : ring;
			if (inner > 0 && inner < 1) {                // blend ring -> fill
				uint32_t a = (uint32_t)(inner * 255);
				col = ((((fill >> 16) & 255) * a + ((ring >> 16) & 255) * (255 - a)) / 255 << 16) |
				      ((((fill >> 8) & 255) * a + ((ring >> 8) & 255) * (255 - a)) / 255 << 8) |
				      (((fill & 255) * a + (ring & 255) * (255 - a)) / 255);
			}
			uint32_t a = (uint32_t)(cov * 255), dpx = p[x];
			uint32_t r8 = (((col >> 16) & 255) * a + ((dpx >> 16) & 255) * (255 - a)) / 255;
			uint32_t g8 = (((col >> 8) & 255) * a + ((dpx >> 8) & 255) * (255 - a)) / 255;
			uint32_t b8 = ((col & 255) * a + (dpx & 255) * (255 - a)) / 255;
			p[x] = 0xFF000000 | (r8 << 16) | (g8 << 8) | b8;
		}
	}
}

// D-pad icon: a dark cross with a light border; the two highlighted arms get a small triangle
// pointing outwards. Coverage is supersampled 4x4 per pixel for smooth edges.
static uint32_t blend(uint32_t dst, uint32_t col, float a)
{
	uint32_t al = (uint32_t)(a * 255);
	uint32_t r = (((col >> 16) & 255) * al + ((dst >> 16) & 255) * (255 - al)) / 255;
	uint32_t g = (((col >> 8) & 255) * al + ((dst >> 8) & 255) * (255 - al)) / 255;
	uint32_t b = ((col & 255) * al + (dst & 255) * (255 - al)) / 255;
	return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static bool in_cross(float x, float y, float h, float arm)   // centred at 0,0, half size h
{
	return (x >= -arm && x <= arm && y >= -h && y <= h) || (y >= -arm && y <= arm && x >= -h && x <= h);
}

static bool in_tri(float x, float y, float ax, float ay, float bx, float by, float cx, float cy)
{
	float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
	float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
	float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
	bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
	return !(neg && pos);
}

static void dpad(float cx, float cy, float size, bool vertical, uint32_t fill, uint32_t ring, uint32_t arrow)
{
	float h = size / 2, arm = size * 0.19f, bw = 2.0f;
	float t = size * 0.13f;                      // arrow half width
	for (int y = (int)(cy - h - 1); y <= (int)(cy + h + 1); y++) {
		if (y < 0 || y >= (int)display_height) continue;
		uint32_t *p = fb_row(y);
		for (int x = (int)(cx - h - 1); x <= (int)(cx + h + 1); x++) {
			if (x < 0 || x >= (int)display_width) continue;
			int outer = 0, inner = 0, arr = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					float fx = x + (sx + 0.5f) / 4 - cx, fy = y + (sy + 0.5f) / 4 - cy;
					if (!in_cross(fx, fy, h, arm)) continue;
					outer++;
					if (in_cross(fx, fy, h - bw, arm - bw)) inner++;
					float e = h * 0.80f, b0 = h * 0.42f;   // arrow tip and base distance from centre
					bool tri = vertical
						? in_tri(fx, fy, 0, -e, -t, -b0, t, -b0) || in_tri(fx, fy, 0, e, -t, b0, t, b0)
						: in_tri(fx, fy, -e, 0, -b0, -t, -b0, t) || in_tri(fx, fy, e, 0, b0, -t, b0, t);
					if (tri) arr++;
				}
			if (!outer) continue;
			uint32_t px = blend(p[x], ring, outer / 16.0f);
			if (inner) px = blend(px, fill, inner / 16.0f);
			if (arr) px = blend(px, arrow, arr / 16.0f);
			p[x] = px;
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

static bool is_face_button(const char *button)
{
	return strcmp(button, "updown") && strcmp(button, "leftright");
}

// Diameter of a button disc: 1.5x the cap height.
static int disc_size()
{
	bool hd = display_height >= 1000;
	int cap = hd ? f1080_sym_cap_bottom - f1080_sym_cap_top + 1 : f720_sym_cap_bottom - f720_sym_cap_top + 1;
	return cap * 3 / 2;
}

int ui_hint_width(const char *button, const char *label)
{
	uint32_t col;
	int sw = disc_size();   // face buttons and the d-pad share the icon size
	(void)col;
	return sw + ui_text_width(UI_BODY, " ") + ui_text_width(UI_BODY, label);
}

int ui_guides = 0;   // set when /dev_hdd0/tmp/psxrip.guides exists: draw cap-height/baseline lines

int ui_hint(int x, int y, const char *button, const char *label)
{
	uint32_t col;
	const char *sym = hint_symbol(button, &col);
	bool hd = display_height >= 1000;
	int top = hd ? f1080_sym_cap_top : f720_sym_cap_top, bot = hd ? f1080_sym_cap_bottom : f720_sym_cap_bottom;
	if (ui_guides) {
		ui_rect(x - 6, y + top, ui_hint_width(button, label) + 12, 1, 0xFFFFFF40);
		ui_rect(x - 6, y + bot, ui_hint_width(button, label) + 12, 1, 0xFF40FFFF);
	}
	// icon box: disc_size() square, centred on the cap height, on whole pixels so the
	// pre-centred button glyph (same size as the disc) lines up exactly with the disc
	int d = disc_size();
	int oy = (int)(y + (top + bot + 1) / 2.0f - d / 2.0f + 0.5f);
	float cx = x + d / 2.0f, cy = oy + d / 2.0f;
	if (is_face_button(button)) {
		disc(cx, cy, d / 2.0f, 0x1E2234, 0x5A6280);
		ui_text(UI_BTN, x, oy, col, sym);
	} else {
		dpad(cx, cy, d, !strcmp(button, "updown"), 0x1E2234, 0x5A6280, 0xE8E8F0);
	}
	int w = d;
	w += ui_text_width(UI_BODY, " ");
	w += ui_text(UI_BODY, x + w, y, 0xE8E8F0, label);
	return w;
}

