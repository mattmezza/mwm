#include "shell.h"

#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <fontconfig/fontconfig.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cui/cui_icon.h"
#include "cui/cui_text.h"
#include "font_assets.h"

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static const cui_color transparent = {0};
static const cui_text_style regular = {0};
static const cui_text_style medium = {.weight = CUI_TEXT_WEIGHT_MEDIUM};
static const cui_text_style mono = {.monospace = true};

typedef struct {
	cui_context *ui;
	const ShellFont *font;
	const Config *config;
	ShellHit *hits;
	int nhits, capacity;
	int hover_x, hover_y;
	int composite;
	float hover, size;
} Frame;

unsigned long
shell_rgb(cui_color c)
{
	return (unsigned long)c.r << 16 | (unsigned long)c.g << 8 | c.b;
}

double
shell_seconds(void)
{
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0)
		return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static FcPattern *
match_font(const char *name, int weight)
{
	FcPattern *request = FcNameParse((const FcChar8 *)name);
	if (!request)
		return NULL;
	if (weight >= 0) {
		FcPatternDel(request, FC_WEIGHT);
		FcPatternAddInteger(request, FC_WEIGHT, weight);
	}
	FcConfigSubstitute(NULL, request, FcMatchPattern);
	FcDefaultSubstitute(request);
	FcResult result;
	FcPattern *match = FcFontMatch(NULL, request, &result);
	FcPatternDestroy(request);
	return match;
}

static cui_font *
open_face(const char *name, int weight, double *size)
{
	FcPattern *request = FcNameParse((const FcChar8 *)name);
	FcChar8 *family = NULL, *matched = NULL, *file;
	if (request)
		FcPatternGetString(request, FC_FAMILY, 0, &family);
	int geist = family && !FcStrCmpIgnoreCase(family, (const FcChar8 *)"Geist");
	int geist_mono = family && !FcStrCmpIgnoreCase(family, (const FcChar8 *)"Geist Mono");
	FcPattern *pattern = match_font(name, weight);
	cui_font *font = NULL;
	if (pattern) {
		if (size)
			FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, size);
		FcPatternGetString(pattern, FC_FAMILY, 0, &matched);
		if (((!geist && !geist_mono) || (matched && !FcStrCmpIgnoreCase(family, matched)))
		    && FcPatternGetString(pattern, FC_FILE, 0, &file) == FcResultMatch)
			font = cui_font_open((char *)file);
		FcPatternDestroy(pattern);
	}
	if (request) {
		if (!font && size)
			FcPatternGetDouble(request, FC_PIXEL_SIZE, 0, size);
		FcPatternDestroy(request);
	}
	if (!font && geist_mono)
		font = cui_font_open_memory(GeistMono_Regular_ttf, sizeof GeistMono_Regular_ttf);
	if (!font && geist)
		font = weight == FC_WEIGHT_MEDIUM
		       ? cui_font_open_memory(Geist_Medium_ttf, sizeof Geist_Medium_ttf)
		       : cui_font_open_memory(Geist_Regular_ttf, sizeof Geist_Regular_ttf);
	return font;
}

void
shell_font_free(ShellFont *font)
{
	cui_font_close(font->font);
	cui_font_close(font->medium);
	cui_font_close(font->mono);
	*font = (ShellFont){0};
}

void
shell_font_license(void)
{
	fwrite(OFL_txt, 1, sizeof OFL_txt, stdout);
}

int
shell_font(ShellFont *out, const Config *config, char error[CONFIG_ERROR])
{
	double size = 18;
	cui_font *font = open_face(config->font, -1, &size);
	if (!font) {
		snprintf(error, CONFIG_ERROR, "cannot load font %s", config->font);
		return 0;
	}
	*out = (ShellFont){.font = font, .size = round(fmin(fmax(size, 9), 28)),
	                   .medium = open_face(config->font, FC_WEIGHT_MEDIUM, NULL),
	                   .mono = open_face(config->monofont, -1, NULL)};
	out->bar_size = config->barfontsize ? config->barfontsize : out->size;
	cui_font_set_style(font, medium, out->medium);
	cui_font_set_style(font, mono, out->mono);
	const char *faces[] = {"sans-serif:lang=ja", "sans-serif:lang=ar", "sans-serif:lang=hi",
	                       "sans-serif:lang=el", "sans-serif:lang=ru"};
	char *fallbacks[COUNT(faces)] = {0};
	int nfallbacks = 0;
	for (int i = 0; i < COUNT(faces); ++i) {
		FcPattern *pattern = match_font(faces[i], -1);
		FcChar8 *file;
		if (pattern && FcPatternGetString(pattern, FC_FILE, 0, &file) == FcResultMatch) {
			int j;
			for (j = 0; j < nfallbacks; ++j)
				if (!strcmp(fallbacks[j], (char *)file))
					break;
			if (j == nfallbacks && (fallbacks[j] = strdup((char *)file)))
				++nfallbacks;
		}
		if (pattern)
			FcPatternDestroy(pattern);
	}
	cui_font_register_fallbacks(font, (const char *const *)fallbacks, nfallbacks);
	for (int i = 0; i < nfallbacks; ++i)
		free(fallbacks[i]);
	return 1;
}

void
shell_surface_free(const ShellHost *host, ShellSurface *surface)
{
	if (surface->gc)
		XFreeGC(host->dpy, surface->gc);
	if (surface->image)
		XDestroyImage(surface->image);
	cui_software_destroy(surface->renderer);
	if (surface->ui)
		cui_destroy(surface->ui);
	*surface = (ShellSurface){0};
}

static int
begin(const ShellHost *host, const ShellFont *font, unsigned revision, ShellSurface *surface,
      Window win, int w, int h, double elapsed)
{
	if (surface->revision != revision)
		shell_surface_free(host, surface);
	surface->revision = revision;
	if (!surface->ui)
		surface->ui = cui_create((cui_config){
			.width = w, .height = h, .max_elements = 820, .max_commands = 2200,
			.measure = cui_font_measure, .measure_styled = cui_font_measure_styled,
			.font_metrics = cui_font_get_metrics, .measure_user = font->font,
			.text_capabilities = cui_font_text_capabilities(),
		});
	if (!surface->renderer)
		surface->renderer = cui_software_create(w, h, font->font);
	if (!surface->ui || !surface->renderer)
		return 0;
	if (!surface->image || surface->image->width != w || surface->image->height != h) {
		if (!cui_software_resize(surface->renderer, w, h))
			return 0;
		XImage *image = XCreateImage(host->dpy, host->visual, host->depth, ZPixmap,
		                             0, NULL, w, h, 32, 0);
		if (!image)
			return 0;
		image->data = calloc(h, image->bytes_per_line);
		if (!image->data) {
			XDestroyImage(image);
			return 0;
		}
		if (surface->image)
			XDestroyImage(surface->image);
		surface->image = image;
		surface->shape_count = -1;
	}
	if (!surface->gc)
		surface->gc = XCreateGC(host->dpy, win, 0, NULL);
	cui_clear_error(surface->ui);
	if (!cui_begin_frame(surface->ui, w, h, elapsed))
		return 0;
	cui_begin_box(surface->ui, 10000, (cui_box){.width = w, .height = h});
	return 1;
}

static unsigned long
channel(unsigned char value, unsigned long mask)
{
	if (!mask)
		return 0;
	unsigned long bottom = mask & (~mask + 1);
	return ((value * (mask / bottom) + 127) / 255 * bottom) & mask;
}

static int
render(const ShellHost *host, ShellSurface *surface)
{
	cui_end_box(surface->ui);
	cui_draw_list list = cui_end_frame(surface->ui);
	if (cui_last_error(surface->ui) != CUI_OK) {
		fprintf(stderr, "mwm: CUI frame failed (%d)\n", cui_last_error(surface->ui));
		return 0;
	}
	if (!cui_software_render(surface->renderer, &list, transparent)) {
		fputs("mwm: CUI software rendering failed\n", stderr);
		return 0;
	}
	unsigned w, h;
	const unsigned char *rgba = cui_software_pixels(surface->renderer, &w, &h);
	XImage *image = surface->image;
	int argb = host->depth == 32;
	int fast = image->bits_per_pixel == 32 && image->byte_order == LSBFirst &&
	           image->red_mask == 0xff0000 && image->green_mask == 0x00ff00 && image->blue_mask == 0x0000ff;
	for (unsigned y = 0; y < h; ++y) {
		unsigned char *row = (unsigned char *)image->data + y * image->bytes_per_line;
		for (unsigned x = 0; x < w; ++x, rgba += 4) {
			unsigned a = argb && host->composited ? rgba[3] : 255;
			unsigned char r = (rgba[0] * a + 127) / 255;
			unsigned char g = (rgba[1] * a + 127) / 255;
			unsigned char b = (rgba[2] * a + 127) / 255;
			if (fast) {
				row[4*x] = b;
				row[4*x+1] = g;
				row[4*x+2] = r;
				row[4*x+3] = argb ? a : 0;
			} else {
				unsigned long pixel = channel(r, image->red_mask) | channel(g, image->green_mask) |
				                      channel(b, image->blue_mask) | (argb ? (unsigned long)a << 24 : 0);
				XPutPixel(image, x, y, pixel);
			}
		}
	}
	return 1;
}

static void
blit(const ShellHost *host, const ShellSurface *surface, Window window)
{
	XPutImage(host->dpy, window, surface->gc, surface->image, 0, 0, 0, 0,
	          surface->image->width, surface->image->height);
}

int
shell_composited(const ShellHost *host)
{
	char name[40];
	snprintf(name, sizeof name, "_NET_WM_CM_S%d", DefaultScreen(host->dpy));
	return host->depth == 32 && XGetSelectionOwner(host->dpy, XInternAtom(host->dpy, name, False)) != None;
}

static void
rounded(Display *dpy, Pixmap mask, GC gc, cui_rect box, int radius)
{
	int x = box.x, y = box.y, w = box.w, h = box.h;
	if (w <= 0 || h <= 0)
		return;
	int d = MIN(2 * radius, MIN(w, h));
	if (d < 2) {
		XFillRectangle(dpy, mask, gc, x, y, w, h);
		return;
	}
	int r = d / 2;
	XFillRectangle(dpy, mask, gc, x + r, y, w - 2*r, h);
	XFillRectangle(dpy, mask, gc, x, y + r, w, h - 2*r);
	XFillArc(dpy, mask, gc, x, y, d, d, 0, 360*64);
	XFillArc(dpy, mask, gc, x+w-d, y, d, d, 0, 360*64);
	XFillArc(dpy, mask, gc, x, y+h-d, d, d, 0, 360*64);
	XFillArc(dpy, mask, gc, x+w-d, y+h-d, d, d, 0, 360*64);
}

static void
shape(const ShellHost *host, ShellSurface *surface, Window win, int w, int h,
      const cui_rect *boxes, int n, int radius, int outset)
{
	if (!host->shape || (surface->shape_count == n && surface->shape_radius == radius
	    && surface->shape_outset == outset && !memcmp(surface->shapes, boxes, n * sizeof *boxes)))
		return;
	memcpy(surface->shapes, boxes, n * sizeof *boxes);
	surface->shape_count = n;
	surface->shape_radius = radius;
	surface->shape_outset = outset;
	Display *dpy = host->dpy;
	Pixmap mask = XCreatePixmap(dpy, win, w, h, 1);
	GC gc = XCreateGC(dpy, mask, 0, NULL);
	XSetForeground(dpy, gc, 0);
	XFillRectangle(dpy, mask, gc, 0, 0, w, h);
	XSetForeground(dpy, gc, 1);
	for (int i = 0; i < n; ++i)
		rounded(dpy, mask, gc, boxes[i], radius);
	XShapeCombineMask(dpy, win, ShapeInput, 0, 0, mask, ShapeSet);
	if (outset) {
		for (int i = 0; i < n; ++i) {
			cui_rect bounds = {boxes[i].x - outset, boxes[i].y - outset,
			                   boxes[i].w + 2*outset, boxes[i].h + 2*outset};
			rounded(dpy, mask, gc, bounds, radius + outset);
		}
	}
	XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);
	XFreeGC(dpy, gc);
	XFreePixmap(dpy, mask);
}

int
shell_bar_inset(const Config *config)
{
	return config->shadows && config->shadow_opacity ? 3 * config->bar_shadow_blur + 1 : 0;
}

static cui_color
alpha(cui_color color, int a)
{
	color.a = a;
	return color;
}

static cui_color
blend(cui_color a, cui_color b, float t)
{
	return cui_color_mix_oklab(a, b, t);
}

static void
box(Frame *f, int id, cui_rect r, float radius, cui_color bg, cui_color border,
    float stroke, int depth)
{
	if (r.w <= 0 || r.h <= 0)
		return;
	cui_begin_absolute_box(f->ui, (cui_id)id, (cui_box){
		.width = r.w, .height = r.h, .radius = radius, .background = bg,
		.border = border, .border_width = stroke,
	}, r);
	if (f->composite && f->config->shadows && f->config->shadow_opacity && depth && bg.a) {
		float blur = depth == 2 ? f->config->panel_shadow_blur : f->config->bar_shadow_blur;
		int opacity = f->config->shadow_opacity;
		cui_box_shadow shadows[] = {
			{.layer = {.offset_y = blur * 0.7f, .sigma = blur,
			           .color = {0, 0, 0, opacity * 3 / 4}}},
			{.layer = {.offset_y = 1, .sigma = blur * 0.4f,
			           .color = {0, 0, 0, opacity / 2}}},
		};
		cui_corner_radii corners = {radius, radius, radius, radius};
		cui_set_box_shadows(f->ui, shadows, COUNT(shadows), corners);
	}
	cui_end_box(f->ui);
}

static float
textwidth(const Frame *f, const char *s, float size, cui_text_style style)
{
	return ceilf(cui_font_measure_styled(f->font->font, cui_string(s), size, style).width);
}

static void
ellipsize(const Frame *f, char *out, size_t capacity, const char *s,
          float width, float size, cui_text_style style)
{
	if (width <= 0) {
		*out = 0;
		return;
	}
	size_t n = MIN(strlen(s), capacity - 4);
	while (((unsigned char)s[n] & 0xc0) == 0x80)
		--n;
	snprintf(out, capacity, "%.*s", (int)n, s);
	float measured = textwidth(f, out, size, style);
	if (!s[n] && (measured > 0 || n == 0) && measured <= width)
		return;
	if (textwidth(f, "…", size, style) > width) {
		*out = 0;
		return;
	}
	do {
		memcpy(out + n, "…", sizeof "…");
		measured = textwidth(f, out, size, style);
		if (!n || (measured > 0 && measured <= width))
			return;
		do {
			--n;
		} while (((unsigned char)s[n] & 0xc0) == 0x80);
	} while (1);
}

static void
label(Frame *f, int id, cui_rect r, const char *s, cui_color color, float size,
      cui_text_style style, cui_align align)
{
	if (r.w <= 0 || r.h <= 0 || !s[0])
		return;
	char text[640];
	ellipsize(f, text, sizeof text, s, r.w, size, style);
	if (!*text)
		return;
	cui_begin_absolute_box(f->ui, (cui_id)id, (cui_box){
		.width = r.w, .height = r.h, .align_x = align, .align_y = CUI_ALIGN_CENTER,
	}, r);
	cui_text_line(f->ui, cui_string(text), size, color, ceilf(size + 4), style);
	cui_end_box(f->ui);
}

static const char *icons[ActChoose + 1] = {
	/* Outline glyphs drawn on the same 24px grid as Heroicons. */
	[ActCalendar] = "<rect x='3' y='5' width='18' height='16' rx='3'/><path d='M7 3v4m10-4v4M3 10h18m-14 5h2m3 0h2m3 0h1m-11 3h2m3 0h2'/>",
	[ActClock] = "<circle cx='12' cy='12' r='9'/><path d='M12 7v5l3 2'/>",
	[ActLayout] = "<rect x='3' y='4' width='18' height='16' rx='2'/><path d='M10 4v16m0-8h11'/>",
	[ActPalette] = "<circle cx='10.5' cy='10.5' r='5.5'/><path d='m15 15 5 5'/>",
	[ActPast] = "<path d='m14.5 6-6 6 6 6'/>",
	[ActFuture] = "<path d='m9.5 6 6 6-6 6'/>",
};

static void
icon(Frame *f, int id, cui_rect rect, enum ShellAction kind, cui_color fg, cui_icon_state *state)
{
	char svg[512];
	snprintf(svg, sizeof svg,
	         "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
	         "<g fill='none' stroke='#ffffff' stroke-width='1.7' stroke-linecap='round' "
	         "stroke-linejoin='round'>%s</g></svg>", icons[kind]);
	if (!state->internal) {
		cui_icon_state_init(state);
		cui_icon_set_data(state, cui_string(svg));
	}
	cui_begin_absolute_box(f->ui, id, (cui_box){.width = rect.w, .height = rect.h}, rect);
	cui_theme theme = cui_theme_dark();
	cui_icon(f->ui, (cui_id)id + 2000, &theme, state,
	         (cui_icon_options){.width = rect.w, .height = rect.h, .custom_width = true, .custom_height = true, .color = fg,
	                            .custom_color = true, .decorative = true});
	cui_end_box(f->ui);
}

static cui_icon_state icon_states[COUNT(icons)];

void
shell_icons_free(void)
{
	for (int i = 0; i < COUNT(icon_states); ++i)
		cui_icon_state_destroy(&icon_states[i]);
}

static void
glyph(Frame *f, int id, float x, float y, int size, enum ShellAction kind, cui_color color)
{
	icon(f, id, (cui_rect){x, y, size, size}, kind, color, &icon_states[kind]);
}

static int
inside(cui_rect rect, int x, int y)
{
	return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

ShellHit
shell_hit(const ShellHit *hits, int count, int x, int y)
{
	for (int i = count - 1; i >= 0; --i)
		if (inside(hits[i].rect, x, y))
			return hits[i];
	return (ShellHit){0};
}

static void
hit(Frame *f, cui_rect r, enum ShellAction action, time_t value)
{
	if (r.w > 0 && r.h > 0 && f->nhits < f->capacity)
		f->hits[f->nhits++] = (ShellHit){r, action, value};
}

static void
button(Frame *f, int id, cui_rect r, const char *text, enum ShellAction action,
       cui_color bg, cui_color fg, int icon_size)
{
	if (r.w <= 0 || r.h <= 0)
		return;
	if (inside(r, f->hover_x, f->hover_y))
		bg = blend(bg, alpha(fg, MAX(bg.a, 90)), 0.14f * f->hover);
	int radius = action == ActLayout ? f->config->segradius : f->config->panelradius;
	box(f, id, r, MIN(radius, 8), bg, transparent, 0, 0);
	if (icon_size) {
		enum ShellAction glyph_kind = action == ActZonePrevious ? ActPast
		                              : action == ActZoneNext ? ActFuture : action;
		glyph(f, id + 1, r.x + (r.w - icon_size) / 2, r.y + (r.h - icon_size) / 2,
		      icon_size, glyph_kind, fg);
	}
	label(f, id + 2, (cui_rect){r.x + 2, r.y, r.w - 4, r.h}, text, fg,
	      f->size, action == ActLayout ? mono : regular, CUI_ALIGN_CENTER);
	hit(f, r, action, 0);
}

static int
status_parse(const char *input, char text[512], cui_text_color_span colors[64], cui_color fg)
{
	size_t n = 0, start = 0;
	int count = 0;
	cui_color current = fg;
	while (*input && n < 511) {
		int reset = !strncmp(input, "^d^", 3);
		int color = !strncmp(input, "^c#", 3) && strlen(input) >= 10 && input[9] == '^';
		for (int i = 3; color && i < 9; ++i)
			color = isxdigit((unsigned char)input[i]);
		if (!reset && !color) {
			text[n++] = *input++;
			continue;
		}
		if (n > start && count < 64)
			colors[count++] = (cui_text_color_span){start, n, current};
		start = n;
		current = fg;
		if (color) {
			char hex[7];
			memcpy(hex, input + 3, 6);
			hex[6] = 0;
			unsigned long rgb = strtoul(hex, NULL, 16);
			current = (cui_color){rgb >> 16, rgb >> 8, rgb, 255};
		}
		input += reset ? 3 : 10;
	}
	text[n] = 0;
	if (n > start && count < 64)
		colors[count++] = (cui_text_color_span){start, n, current};
	return count;
}

static void
status_label(Frame *f, cui_rect r, const char *status, int id, cui_color fg)
{
	char text[512], clipped[520];
	cui_text_color_span colors[64];
	int n = status_parse(status, text, colors, fg);
	ellipsize(f, clipped, sizeof clipped, text, r.w, f->size, mono);
	size_t len = strlen(clipped);
	if (!len || r.h <= 0)
		return;
	if (strcmp(clipped, text))
		len -= sizeof "…" - 1;
	while (n && colors[n-1].byte_start >= len)
		--n;
	if (n && colors[n-1].byte_end > len)
		colors[n-1].byte_end = len;
	cui_begin_absolute_box(f->ui, id, (cui_box){
		.width = r.w, .height = r.h, .align_y = CUI_ALIGN_CENTER,
	}, r);
	cui_text_paragraph_colored(f->ui, cui_string(clipped), f->size, fg,
	                          ceilf(f->size + 4), mono, false, colors, n);
	cui_end_box(f->ui);
}

static cui_color
surface_color(const Frame *f, int scheme)
{
	cui_color c = f->config->palette[scheme][ColBg];
	if (!f->composite && c.a)
		c.a = 255;
	return c;
}

static void
bar_clock(Frame *f, cui_rect r, const char *text, enum ShellAction action, int id,
          cui_color bg, cui_color fg, cui_color accent, int with_icon)
{
	if (r.w < 12)
		return;
	if (inside(r, f->hover_x, f->hover_y))
		box(f, id, r, MIN(f->config->segradius, 7), blend(bg, fg, 0.12f * f->hover), transparent, 0, 0);
	if (with_icon)
		glyph(f, id + 1, r.x + 7, r.y + (r.h - 18) / 2, 18, action, accent);
	int pad = with_icon ? 31 : 3;
	label(f, id + 2, (cui_rect){r.x + pad, r.y, r.w - pad - 3, r.h}, text,
	      fg, f->size, mono, CUI_ALIGN_CENTER);
	hit(f, r, action, 0);
}

static float
reveal(ShellBar *bar, ShellReveal *state, int shown, double tick, int duration)
{
	if (!duration || !bar->last_frame) {
		*state = (ShellReveal){.level = shown, .shown = shown};
		return shown;
	}
	if (state->shown != shown) {
		state->from = state->level;
		state->shown = shown;
		state->started = tick;
	}
	if (state->started) {
		float t = fmin(1, (tick - state->started) * 1000 / duration);
		float eased = t*t*(3 - 2*t);
		state->level = state->from + (shown - state->from) * eased;
		if (t == 1)
			state->started = 0;
	}
	bar->animating |= state->started != 0;
	return state->level;
}

static void
clip(Frame *f, int id, cui_rect r)
{
	cui_begin_absolute_box(f->ui, id, (cui_box){.width = r.w, .height = r.h, .clip = true}, r);
}

static int
tag_width(const Frame *f, const ShellTag *tag)
{
	return MIN(140, MAX(32, textwidth(f, tag->label, f->size, mono) + 2*f->config->barpadx));
}

enum { LayoutButtonWidth = 34 };

static int
tags_width(const Frame *f, const ShellBarModel *m)
{
	int width = m->layout >= 0 ? LayoutButtonWidth + 8 : m->ntags ? 8 : 0;
	for (int i = 0; i < m->ntags; ++i)
		width += tag_width(f, &m->tags[i]);
	return width;
}

static void
bar_tags(Frame *f, ShellBar *bar, cui_rect area, const ShellBarModel *m,
         double tick, int duration, cui_color outline, cui_color accent)
{
	const Config *c = f->config;
	int width = area.w, y = area.y, height = area.h;
	float radius = c->segradius, size = f->size;
	cui_color norm = c->palette[SchemeNorm][ColFg];
	cui_color norm_bg = surface_color(f, SchemeNorm);
	/* Fit the active tag first, then lay the visible tags out in tag order. */
	int layoutw = m->layout >= 0 && width >= LayoutButtonWidth + 8 + (m->ntags ? 32 : 0)
	              ? LayoutButtonWidth : 0;
	int leftw = layoutw + 8;
	int tagw[MAX_TAGS] = {0};
	for (int pass = 0; pass < 2; ++pass)
		for (int i = 0; i < m->ntags; ++i) {
			const ShellTag *tag = &m->tags[i];
			if ((tag->id == m->tag) != (pass == 0))
				continue;
			int allocated = MIN(tag_width(f, tag), width - leftw);
			if (allocated >= 21) {
				tagw[i] = allocated;
				leftw += allocated;
			}
		}
	if (leftw > 8) {
		cui_rect left = {area.x, y, width, height};
		box(f, 20, left, radius, norm_bg, outline, norm_bg.a != 0, 1);
		int x = area.x + 4;
		cui_rect selected = {0};
		for (int i = 0; i < m->ntags; ++i) {
			if (!tagw[i])
				continue;
			if (m->tags[i].id == m->tag)
				selected = (cui_rect){x - left.x, y+4, tagw[i], height-8};
			x += tagw[i];
		}
		if (selected.w > 0) {
			if (!bar->selected_w || !duration) {
				bar->selected_x = bar->from_x = selected.x;
				bar->selected_w = bar->from_w = selected.w;
				bar->transition = 0;
			} else if (bar->tag != m->tag) {
				bar->from_x = bar->selected_x;
				bar->from_w = bar->selected_w;
				bar->transition = tick;
			}
			float t = bar->transition ? fmin(1, (tick - bar->transition) * 1000 / duration) : 1;
			float eased = 1 - powf(1 - t, 3);
			bar->selected_x = bar->from_x + (selected.x - bar->from_x) * eased;
			bar->selected_w = bar->from_w + (selected.w - bar->from_w) * eased;
			bar->animating |= t < 1;
			bar->tag = m->tag;
			float sx = left.x + fmaxf(bar->selected_x, 3);
			float sw = fminf(bar->selected_w, left.x + left.w - 3 - sx);
			box(f, 21, (cui_rect){sx, selected.y, sw, selected.h}, c->tagradius,
			    surface_color(f, SchemeSel), transparent, 0, 0);
		} else {
			bar->selected_w = 0;
		}
		x = area.x + 4;
		for (int i = 0; i < m->ntags; ++i) {
			const ShellTag *tag = &m->tags[i];
			if (!tagw[i])
				continue;
			cui_rect r = {x, y+4, tagw[i], height-8};
			int current = tag->id == m->tag, id = 200 + tag->id * 3;
			cui_color color = current ? c->palette[SchemeSel][ColFg] : norm;
			if (!current && tag->urgent) {
				box(f, id, r, c->tagradius, surface_color(f, SchemeUrg), transparent, 0, 0);
				color = c->palette[SchemeUrg][ColFg];
			} else if (!current && inside(r, f->hover_x, f->hover_y)) {
				box(f, id, r, c->tagradius, blend(norm_bg, norm, 0.12f * f->hover), transparent, 0, 0);
			}
			label(f, id+1, (cui_rect){r.x+3, r.y, r.w-6, r.h}, tag->label, color,
			      size, mono, CUI_ALIGN_CENTER);
			hit(f, r, ActTag, tag->id);
			x += tagw[i];
		}
		if (layoutw) {
			cui_rect r = {x, y+4, layoutw, height-8};
			if (x > area.x + 4)
				box(f, 24, (cui_rect){x, y+9, 1, height-18}, 0, outline, transparent, 0, 0);
			const char *symbols[] = {"|||", "||=", "><>"};
			button(f, 25, r, symbols[m->layout], ActLayout, transparent, accent, 0);
		}
	} else {
		bar->selected_w = 0;
	}
}

static void
bar_datetime(Frame *f, cui_rect r, const char *date, const char *clock,
             const ShellTimer *timer, time_t now, double tick, cui_color outline, cui_color accent)
{
	const Config *c = f->config;
	int y = r.y, height = r.h;
	float size = f->size;
	cui_color fg = c->palette[SchemeStatus][ColFg], status_bg = surface_color(f, SchemeStatus);
	int cw = textwidth(f, clock, size, mono) + 39;
	int dw = textwidth(f, date, size, mono) + 39;
	int clockw = cw + dw + 8;
	int show_icons = r.w >= clockw;
	if (!show_icons)
		cw -= 26;
	int clockx = r.x, right = r.x + r.w;
	cw = MIN(cw, MAX(0, (int)r.w - 8));
	dw = MAX(0, (int)r.w - cw - 8);
	if (dw < 34) {
		dw = 0;
		cw = MAX(0, (int)r.w - 8);
	}
	box(f, 10, r, c->segradius, status_bg, outline, status_bg.a != 0, 1);
	bar_clock(f, (cui_rect){clockx+4, y+2, dw, height-4}, date, ActCalendar,
	          11, status_bg, fg, accent, show_icons);
	cui_rect hour = {right-cw-4, y+2, cw, height-4};
	bar_clock(f, hour, clock, ActClock, 15, status_bg, fg, accent, show_icons);
	if (dw > 20)
		box(f, 19, (cui_rect){clockx+dw+4, y+8, 1, height-16},
		    0, outline, transparent, 0, 0);
	if (timer->active && c->timerprogressheight && hour.w > 20) {
		int thickness = c->timerprogressheight;
		cui_rect rail = {hour.x+5, y+height-1-thickness, hour.w-10, thickness};
		double remaining = shell_timer_remaining(timer, now, tick);
		float elapsed = 1 - fmin(fmax(remaining / timer->duration, 0), 1);
		box(f, 30, rail, thickness/2.f, blend(status_bg, accent, 0.35f), transparent, 0, 0);
		rail.w = MAX(thickness, roundf(rail.w * elapsed));
		box(f, 31, rail, thickness/2.f, accent, transparent, 0, 0);
	}
}

enum { SystemPadding = 10, SystemGap = 8, CpuLabelSize = 11 };

static int
system_width(const Frame *f)
{
	return 2*SystemPadding + SystemGap + textwidth(f, "CPU", CpuLabelSize, medium)
	       + textwidth(f, "100%", f->size, mono);
}

static void
bar_system(Frame *f, ShellBar *bar, cui_rect r, const ShellSystem *system,
           cui_color outline, cui_color accent)
{
	const Config *c = f->config;
	cui_color bg = surface_color(f, SchemeStatus), fg = c->palette[SchemeStatus][ColFg];
	box(f, 50, r, c->segradius, bg, outline, bg.a != 0, 1);
	int x = r.x + SystemPadding;
	int key = textwidth(f, "CPU", CpuLabelSize, medium);
	label(f, 51, (cui_rect){x, r.y, key, r.h}, "CPU",
	      blend(bg, fg, 0.76f), CpuLabelSize, medium, CUI_ALIGN_START);
	int number = textwidth(f, "100%", f->size, mono);
	char value[8] = "–";
	if (system->count)
		snprintf(value, sizeof value, "%d%%", (int)round(system->cpu[system->count-1].value));
	label(f, 52, (cui_rect){x+key+SystemGap, r.y, number, r.h}, value,
	      fg, f->size, mono, CUI_ALIGN_END);

	int width = MIN(c->cpu_width, (int)r.w - system_width(f) - SystemGap);
	if (width < 16)
		return;
	int height = MIN(CPU_CHART_HEIGHT, (int)r.h - 8);
	int left = r.x + r.w - SystemPadding - width;
	int top = r.y + (int)(r.h - height)/2;
	box(f, 53, (cui_rect){left, top + height/2, width, 1}, 0,
	    alpha(fg, 32), transparent, 0, 0);

	int count = MIN(system->count, c->cpu_history);
	if (count < 2)
		return;
	int span = MAX(2, (int)round((width - 1.0) * (count - 1) / (c->cpu_history - 1)) + 1);
	bar->cpu_chart.pixels = bar->cpu_pixels;
	bar->cpu_chart.byte_capacity = sizeof bar->cpu_pixels;
	cui_rect graph = {left + width - span, top, span, height};
	cui_begin_absolute_box(f->ui, 54, (cui_box){.width = span, .height = height}, graph);
	cui_chart(f->ui, 55, &bar->cpu_chart, &(cui_chart_options){
		.kind = CUI_CHART_AREA, .curve = CUI_CHART_CURVE_LINEAR,
		.data = system->cpu + system->count - count, .count = count, .data_revision = system->revision,
		.width = span, .height = height, .has_domain = true, .domain_min = 0, .domain_max = 100,
		.hide_axes = true, .hide_x_axis = true, .hide_grid = true, .hide_legend = true,
		.noninteractive = true, .disable_appear = true,
		.custom_y_padding = true, .y_padding_top = 1, .y_padding_bottom = 1,
		.series = {accent}, .primary_fill = alpha(accent, 55), .line_width = 1.5f,
	}, cui_string("CPU utilization"));
	cui_end_box(f->ui);
}

typedef struct {
	int wanted, minimum, width, gap, x;
	float level;
	enum BarAlign align;
} BarSlot;

static void
place_segments(const Config *c, BarSlot slots[BarSegmentCount], int w, int margin)
{
	/* Keep navigation and the clock legible as the available space shrinks. */
	const enum BarSegment priority[] = {BarTags, BarDatetime, BarSystem, BarTray, BarStatus, BarTitle};
	int free = w - 2*margin;
	for (int i = 0; i < COUNT(priority); ++i) {
		BarSlot *s = &slots[priority[i]];
		int width = roundf(s->minimum * s->level);
		int gap = roundf(c->barseggap * s->level);
		if (width && width + gap <= free) {
			s->width = width;
			s->gap = gap;
			free -= width + gap;
		}
	}
	for (int i = 0; i < COUNT(priority); ++i) {
		BarSlot *s = &slots[priority[i]];
		if (s->width) {
			int extra = MIN(free, (int)roundf(s->wanted * s->level) - s->width);
			s->width += extra;
			free -= extra;
		}
	}

	int widths[BarAlignCount] = {0}, trailing[BarAlignCount] = {0};
	for (int i = 0; i < c->nsegments; ++i) {
		const BarSlot *s = &slots[c->segments[i].segment];
		if (s->width) {
			widths[s->align] += s->width + s->gap;
			trailing[s->align] = s->gap;
		}
	}
	int right = w - margin - widths[BarRight] + trailing[BarRight];
	int center_width = widths[BarCenter] - trailing[BarCenter];
	int center = MAX(margin + widths[BarLeft],
	                 MIN((w - center_width)/2, right - center_width -
	                     (widths[BarRight] ? trailing[BarCenter] : 0)));
	int x[BarAlignCount] = {margin, center, right};
	for (int i = 0; i < c->nsegments; ++i) {
		BarSlot *s = &slots[c->segments[i].segment];
		if (s->width) {
			s->x = x[s->align];
			x[s->align] += s->width + s->gap;
		}
	}
}

void
shell_bar_draw(const ShellHost *host, const ShellFont *font, unsigned revision,
               const Config *c, ShellBar *bar, Window window, const ShellBarModel *m,
               const ShellTimer *timer, time_t now, double tick)
{
	int w = m->w, h = m->h, y = m->pad;
	int height = h - 2*y;
	double dt = bar->last_frame ? fmin(tick - bar->last_frame, 0.1) : 0;
	if (w < 1 || height < 1
	    || !begin(host, font, revision, &bar->surface, window, w, h, dt))
		return;

	int duration = c->reduced_motion ? 0 : c->duration;
	bar->animating = 0;
	int tray_duration = host->shape && m->tray_width ? duration : 0;
	float tray = reveal(bar, &bar->tray_reveal, m->tray_shown, tick, tray_duration);
	float status = reveal(bar, &bar->status_reveal, !!m->status[0], tick, duration);
	float title = reveal(bar, &bar->title_reveal, !!m->title[0], tick, duration);
	if (m->status[0])
		snprintf(bar->status, sizeof bar->status, "%s", m->status);
	if (m->title[0])
		snprintf(bar->title, sizeof bar->title, "%s", m->title);
	bar->last_frame = tick;

	ShellHit hover = shell_hit(bar->hits, bar->nhits, bar->hover_x, bar->hover_y);
	int hovered = hover.action * (MAX_TAGS + 1) + hover.value;
	if (bar->hovered != hovered) {
		bar->hovered = hovered;
		bar->hover_since = tick;
	}
	float fade = duration ? fminf(1, (tick - bar->hover_since) * 1000 / duration) : 1;
	Frame f = {bar->surface.ui, font, c, bar->hits, 0, COUNT(bar->hits),
	           bar->hover_x, bar->hover_y, host->composited, fade, font->bar_size};
	cui_set_reduced_motion(f.ui, c->reduced_motion != 0);
	cui_rect shapes[BarSegmentCount];
	int n = 0;
	float radius = c->segradius, size = f.size;
	cui_color fg = c->palette[SchemeStatus][ColFg];
	cui_color accent = alpha(c->palette[SchemeSel][ColBg], 255);
	cui_color outline = alpha(c->palette[SchemeNorm][ColBorder], 170);
	cui_color status_bg = surface_color(&f, SchemeStatus), bar_bg = surface_color(&f, SchemeBar);

	int margin = MIN(c->barmargin, w / 8);
	cui_rect strip = {0, y, w, height};
	if (bar_bg.a)
		box(&f, 1, strip, 0, bar_bg, transparent, 0, 0);
	struct tm local;
	char date[80] = "", clock[40] = "";
	if (localtime_r(&now, &local)) {
		strftime(date, sizeof date, "%a %d %b", &local);
		strftime(clock, sizeof clock, "%H:%M", &local);
	}

	int tags = tags_width(&f, m);
	int titlew = title > 0 ? textwidth(&f, bar->title, size, medium) + 2*c->barpadx + 22 : 0;
	int statusw = 0;
	if (status > 0) {
		char text[512];
		cui_text_color_span spans[64];
		status_parse(bar->status, text, spans, fg);
		statusw = textwidth(&f, text, size, mono) + 2*c->barpadx;
		if (title > 0)
			statusw = MIN(statusw, w / 3);
	}
	int datetime = textwidth(&f, clock, size, mono) + textwidth(&f, date, size, mono) + 86;
	int system = system_width(&f);
	int tray_min = host->shape ? MIN(m->tray_width, 2*MAX(5, c->barpadx) + 16) : m->tray_width;
	BarSlot slots[BarSegmentCount] = {
		[BarTags] = {tags, MIN(tags, 40), .level = 1},
		[BarTitle] = {titlew, titlew >= 54 ? 54 : 0, .level = title},
		[BarStatus] = {statusw, MIN(statusw, 40), .level = status},
		[BarSystem] = {system + SystemGap + c->cpu_width, system, .level = 1},
		[BarDatetime] = {datetime, textwidth(&f, clock, size, mono) + 14, .level = 1},
		[BarTray] = {m->tray_width, tray_min, .level = tray},
	};
	unsigned placed = 0;
	for (int i = 0; i < c->nsegments; ++i) {
		BarPlacement p = c->segments[i];
		slots[p.segment].align = !c->custom_segments && p.segment == BarTitle
		                         ? (enum BarAlign)c->titlealign : p.align;
		placed |= 1u << p.segment;
	}
	for (int i = 0; i < BarSegmentCount; ++i)
		if (!(placed & (1u << i)))
			slots[i].level = 0;
	place_segments(c, slots, w, margin);

	if (!slots[BarTags].width)
		bar->selected_w = 0;
	for (int i = 0; i < c->nsegments; ++i) {
		enum BarSegment kind = c->segments[i].segment;
		const BarSlot *s = &slots[kind];
		if (!s->width)
			continue;
		cui_rect r = {s->x, y, s->width, height};
		shapes[n++] = r;
		switch (kind) {
		case BarTags:
			bar_tags(&f, bar, r, m, tick, duration, outline, accent);
			break;
		case BarTitle: {
			cui_color ink = c->palette[SchemeTitle][ColFg], bg = surface_color(&f, SchemeTitle);
			if (inside(r, f.hover_x, f.hover_y))
				bg = blend(bg, ink, 0.1f * f.hover);
			box(&f, 40, r, radius, bg, outline, bg.a != 0, 1);
			int full = roundf(r.w / s->level);
			clip(&f, 43, r);
			glyph(&f, 41, 9, (height-17)/2, 17, ActPalette, accent);
			label(&f, 42, (cui_rect){31, 0, full-31-c->barpadx, height}, bar->title,
			      ink, size, medium, CUI_ALIGN_START);
			cui_end_box(f.ui);
			if (m->title[0])
				hit(&f, r, ActPalette, 0);
			break;
		}
		case BarStatus: {
			box(&f, 35, r, radius, status_bg, outline, status_bg.a != 0, 1);
			int full = roundf(r.w / s->level);
			int shift = s->align == BarRight ? r.w - full : 0;
			clip(&f, 37, r);
			status_label(&f, (cui_rect){shift+c->barpadx, 0, full-2*c->barpadx, height},
			             bar->status, 36, fg);
			cui_end_box(f.ui);
			break;
		}
		case BarDatetime:
			bar_datetime(&f, r, date, clock, timer, now, tick, outline, accent);
			break;
		case BarTray:
			box(&f, 2, r, radius, alpha(status_bg, 255), outline, 1, 1);
			break;
		case BarSystem:
			bar_system(&f, bar, r, m->system, outline, accent);
			break;
		case BarSegmentCount:
			break;
		}
	}
	bar->animating |= hovered && fade < 1;
	if (render(host, &bar->surface)) {
		bar->tray_x = slots[BarTray].x;
		bar->tray_width = slots[BarTray].width;
		bar->nhits = f.nhits;
		if (bar_bg.a)
			shape(host, &bar->surface, window, w, h, &strip, 1, 0, 0);
		else
			shape(host, &bar->surface, window, w, h, shapes, n, radius, host->composited ? y : 0);
		/* Expand the drawable region before presenting newly visible pixels. */
		blit(host, &bar->surface, window);
	}
}

void
shell_calendar_shift(ShellPanel *panel, int months)
{
	int month = panel->year*12 + panel->month + months;
	if (month < 1900*12 || month >= 10000*12)
		return;
	panel->year = month / 12;
	panel->month = month % 12;
}

int
shell_panel_open(const ShellHost *host, const Config *config, ShellPanel *panel,
                 enum ShellAction kind, int monitor, int x, int y, int w, int h,
                 int by, int bh, float anchor, Time event_time)
{
	if (w < 210 || h < 160)
		return 0;
	shell_panel_close(host, panel);
	int pad = host->composited && config->shadows && config->shadow_opacity
	          ? 4 * config->panel_shadow_blur : 0;
	int width = MIN(kind == ActPalette ? config->palette_width
	                : kind == ActClock ? config->clock_width : config->calendar_width, w - 12 - 2*pad);
	const int separation = 7;
	int bar_inset = shell_bar_inset(config);
	int py = kind == ActPalette ? y + (h - MIN(420, h-16))/4
	         : config->topbar ? by + bh - bar_inset + separation - pad
	                          : by + bar_inset - separation + pad;
	int available = kind == ActPalette ? y + h - py - 8
	                : config->topbar ? y + h - py - 6 : py - y - 6;
	int timer_presets = (config->nin_presets || config->at_presets) && available - 2*pad >= 326;
	int clock_base = timer_presets ? 286 : 246;
	int zone_rows = MAX(1, MIN(config->nzones + 1, (available - clock_base - 2*pad)/40));
	int height = kind == ActCalendar ? 364 : kind == ActPalette ? 420 : clock_base + zone_rows*40;
	height = MIN(height, available - 2*pad);
	if (height < (kind == ActClock ? 280 : kind == ActCalendar ? 240 : 160))
		return 0;
	int px = kind == ActPalette ? x + (w - width)/2 - pad : anchor - width/2 - pad;
	int margin = MIN(config->barmargin, (w - width)/2);
	int edge = MAX(0, margin - pad);
	px = MAX(x + edge, MIN(px, x + w - width - 2*pad - edge));
	if (kind != ActPalette && !config->topbar)
		py -= height + 2*pad;
	struct tm local = {0};
	time_t now = time(NULL);
	localtime_r(&now, &local);
	*panel = (ShellPanel){.kind = kind, .monitor = monitor, .width = width,
	                      .height = height, .pad = pad, .year = local.tm_year+1900, .month = local.tm_mon,
	                      .zone_rows = zone_rows, .timer_presets = timer_presets,
	                      .hover_x = -1, .hover_y = -1,
	                      .opened = shell_seconds()};
	XSetWindowAttributes attrs = {
		.override_redirect = True, .background_pixel = 0, .border_pixel = 0, .colormap = host->cmap,
		.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | KeyPressMask,
	};
	Display *dpy = host->dpy;
	panel->win = XCreateWindow(dpy, host->root, px, py, width+2*pad, height+2*pad,
	                           0, host->depth, InputOutput, host->visual,
	                           CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
	                           &attrs);
	XClassHint klass = {"mwm-panel", "mwm"};
	XSetClassHint(dpy, panel->win, &klass);
	XStoreName(dpy, panel->win, kind == ActCalendar ? "mwm calendar"
	           : kind == ActClock ? "mwm clock" : "mwm command palette");
	XMapRaised(dpy, panel->win);
	int pointer = XGrabPointer(dpy, panel->win, False, ButtonPressMask | PointerMotionMask,
	                           GrabModeAsync, GrabModeAsync, None, None, event_time);
	int keyboard = XGrabKeyboard(dpy, panel->win, False, GrabModeAsync, GrabModeAsync, event_time);
	if (pointer != GrabSuccess || keyboard != GrabSuccess) {
		shell_panel_close(host, panel);
		return 0;
	}
	return 1;
}

void
shell_panel_close(const ShellHost *host, ShellPanel *panel)
{
	if (!panel->win)
		return;
	XDestroyWindow(host->dpy, panel->win);
	shell_surface_free(host, &panel->surface);
	*panel = (ShellPanel){0};
}

static void
calendar(Frame *f, const ShellPanel *p, const struct tm *local, cui_color fg,
         cui_color muted, cui_color accent, cui_color ink, cui_color card)
{
	struct tm first = {.tm_year = p->year-1900, .tm_mon = p->month, .tm_mday = 1, .tm_hour = 12, .tm_isdst = -1};
	mktime(&first);
	char heading[96];
	strftime(heading, sizeof heading, "%B %Y", &first);
	float x = p->pad, y = p->pad, w = p->width, h = p->height;
	label(f, 110, (cui_rect){x+18, y+12, w-120, 36}, heading, fg,
	      f->size + 5, medium, CUI_ALIGN_START);
	button(f, 120, (cui_rect){x+w-100, y+12, 38, 36}, "", ActPast, card, fg, 18);
	button(f, 130, (cui_rect){x+w-55, y+12, 38, 36}, "", ActFuture, card, fg, 18);
	box(f, 140, (cui_rect){x+17, y+56, w-34, 1}, 0, card, transparent, 0, 0);
	int cell = (w - 34)/7;
	int row = MAX(15, MIN(37, ((int)h - 138)/6));
	float gridx = x + (w - 7*cell)/2;
	for (int i = 0; i < 7; ++i) {
		struct tm day = {.tm_wday = (i + 1) % 7};
		char caption[32];
		strftime(caption, sizeof caption, "%a", &day);
		label(f, 150+i, (cui_rect){gridx+i*cell, y+62, cell, 24},
		      caption, muted, MAX(9, f->size-2), medium, CUI_ALIGN_CENTER);
	}
	struct tm last = {.tm_year = p->year-1900, .tm_mon = p->month+1, .tm_mday = 0,
	                  .tm_hour = 12, .tm_isdst = -1};
	mktime(&last);
	int start = (first.tm_wday + 6) % 7;
	for (int i = 0; i < 42; ++i) {
		int day = i - start + 1;
		if (day < 1 || day > last.tm_mday)
			continue;
		int today = day == local->tm_mday && p->month == local->tm_mon && p->year == local->tm_year + 1900;
		cui_rect r = {gridx+(i%7)*cell+2, y+89+(i/7)*row, cell-4, row-2};
		if (today)
			box(f, 200+i, r, f->config->panelradius, accent, transparent, 0, 0);
		char text[8];
		snprintf(text, sizeof text, "%d", day);
		label(f, 250+i, r, text, today ? ink : i%7 > 4 ? muted : fg,
		      f->size, mono, CUI_ALIGN_CENTER);
	}
	button(f, 310, (cui_rect){x+17, y+h-44, 80, 32}, "Today", ActToday, card, fg, 0);
	char today[80];
	strftime(today, sizeof today, "%A, %d %B", local);
	label(f, 320, (cui_rect){x+108, y+h-44, w-125, 32}, today, muted,
	      MAX(9, f->size-1), regular, CUI_ALIGN_END);
}

static void
timer_presets(Frame *f, int at, cui_rect row, time_t now, const struct tm *local,
              cui_color card, cui_color fg, cui_color outline)
{
	int count = at ? f->config->at_presets : f->config->nin_presets;
	if (!count)
		return;
	const int gap = 6;
	float size = MAX(9, f->size-1);
	float minimum = textwidth(f, at ? "00:00" : "00h00m", size, mono) + 8;
	count = MIN(count, (row.w + gap) / (minimum + gap));
	if (!count)
		return;
	time_t candidate = now - local->tm_sec + 60;
	int last_hour = -1;
	for (int i = 0; i < count; ++i) {
		time_t value;
		char caption[16];
		if (at) {
			struct tm hour;
			do {
				if (difftime(candidate, now) > 12*3600 || !localtime_r(&candidate, &hour))
					return;
				value = candidate;
				candidate += 60;
			} while (hour.tm_min || hour.tm_hour == last_hour);
			last_hour = hour.tm_hour;
			strftime(caption, sizeof caption, "%H:%M", &hour);
		} else {
			int minutes = f->config->in_presets[i];
			value = minutes * 60;
			if (minutes < 60)
				snprintf(caption, sizeof caption, "%dm", minutes);
			else if (minutes % 60)
				snprintf(caption, sizeof caption, "%dh%02dm", minutes/60, minutes%60);
			else
				snprintf(caption, sizeof caption, "%dh", minutes/60);
		}
		float start = roundf(i * (row.w + gap) / count);
		float end = roundf((i+1) * (row.w + gap) / count);
		cui_rect r = {row.x+start, row.y, end-start-gap, row.h};
		int id = 380 + i*2;
		cui_color bg = inside(r, f->hover_x, f->hover_y) ? blend(card, fg, 0.13f) : card;
		box(f, id, r, f->config->panelradius, bg, outline, 1, 0);
		label(f, id+1, (cui_rect){r.x+4, r.y, r.w-8, r.h}, caption, fg, size, mono, CUI_ALIGN_CENTER);
		hit(f, r, at ? ActTimerAtPreset : ActTimerInPreset, value);
	}
}

static void
world_clock(Frame *f, const ShellPanel *p, const ShellTimer *timer, const struct tm *local,
            time_t now, double tick, cui_color fg, cui_color muted, cui_color accent,
            cui_color ink, cui_color card, cui_color outline)
{
	float x = p->pad, y = p->pad, w = p->width, h = p->height;
	float small = MAX(9, f->size - 1);
	float radius = f->config->panelradius;
	float time_width = textwidth(f, "00:00", f->size, mono) + 2;
	glyph(f, 110, x+18, y+20, 20, ActClock, accent);
	label(f, 111, (cui_rect){x+47, y+10, w-156, 39}, "World clock", fg,
	      f->size+5, medium, CUI_ALIGN_START);
	if (f->config->nzones+1 > p->zone_rows) {
		button(f, 120, (cui_rect){x+w-96, y+12, 36, 36}, "", ActZonePrevious, card, fg, 18);
		button(f, 130, (cui_rect){x+w-53, y+12, 36, 36}, "", ActZoneNext, card, fg, 18);
	}
	int first = MIN(p->zone_start, MAX(0, f->config->nzones+1-p->zone_rows));
	for (int row = 0; row < p->zone_rows && first+row <= f->config->nzones; ++row) {
		int i = first+row;
		float yy = y+55+row*40;
		struct tm zoned = *local;
		const char *name = "Local time";
		if (i) {
			const TimeZone *zone = &f->config->zones[i-1];
			name = zone->label;
			if (!config_zone_time(zone->name, now, &zoned))
				continue;
		}
		char time[32], weekday[24];
		strftime(time, sizeof time, "%H:%M", &zoned);
		strftime(weekday, sizeof weekday, "%a", &zoned);
		box(f, 150+row*4, (cui_rect){x+17, yy, w-34, 36}, radius, card, transparent, 0, 0);
		label(f, 151+row*4, (cui_rect){x+27, yy, w-116-time_width, 36}, name, fg,
		      f->size, i ? regular : medium, CUI_ALIGN_START);
		label(f, 152+row*4, (cui_rect){x+w-83-time_width, yy, 46, 36}, weekday, muted,
		      small, regular, CUI_ALIGN_END);
		label(f, 153+row*4, (cui_rect){x+w-27-time_width, yy, time_width, 36}, time, i ? fg : accent,
		      f->size, mono, CUI_ALIGN_END);
	}
	float title = h - (p->timer_presets ? 217 : 177);
	box(f, 300, (cui_rect){x+17, y+title-8, w-34, 1}, 0, outline, transparent, 0, 0);
	label(f, 301, (cui_rect){x+17, y+title, w-34, 28}, "Timer", fg, f->size+3, medium, CUI_ALIGN_START);
	if (timer->active) {
		int seconds = MAX(0, ceil(shell_timer_remaining(timer, now, tick)));
		char remaining[48];
		snprintf(remaining, sizeof remaining, "%02d:%02d:%02d remaining",
		         seconds/3600, seconds/60%60, seconds%60);
		int countdown_height = p->timer_presets ? 80 : 51;
		box(f, 310, (cui_rect){x+17, y+title+41, w-34, countdown_height}, radius, card, outline, 1, 0);
		label(f, 311, (cui_rect){x+27, y+title+41, w-54, countdown_height}, remaining, accent,
		      f->size+3, mono, CUI_ALIGN_CENTER);
		button(f, 320, (cui_rect){x+w-109, y+h-52, 92, 35}, "Cancel", ActTimerCancel, card, fg, 0);
		return;
	}
	float bw = (w-40)/2;
	button(f, 330, (cui_rect){x+17, y+title+32, bw, 36}, "Alert me in", ActIn,
	       p->at ? card : accent, p->at ? fg : ink, 0);
	button(f, 340, (cui_rect){x+23+bw, y+title+32, bw, 36}, "Alert me at", ActAt,
	       p->at ? accent : card, p->at ? ink : fg, 0);
	cui_rect input = {x+17, y+title+79, w-132, 42};
	box(f, 350, input, radius, card, accent, 1, 0);
	label(f, 351, (cui_rect){input.x+14, input.y, input.w-25, input.h},
	      p->input[0] ? p->input : p->at ? "HH:MM" : "Minutes / HH:MM:SS",
	      p->input[0] ? fg : muted, small, mono, CUI_ALIGN_START);
	char prefix[sizeof p->input];
	snprintf(prefix, sizeof prefix, "%.*s", p->cursor, p->input);
	float caret = prefix[0] ? textwidth(f, prefix, small, mono) : 0;
	if (caret < input.w-24)
		box(f, 352, (cui_rect){input.x+12+caret, input.y+11, 1, 20},
		    0, accent, transparent, 0, 0);
	hit(f, input, ActTimerInput, 0);
	button(f, 360, (cui_rect){x+w-105, input.y, 88, input.h}, "Set", ActTimerSet, accent, ink, 0);
	if (p->timer_presets)
		timer_presets(f, p->at, (cui_rect){x+17, y+title+130, w-34, 34},
		              now, local, card, fg, outline);
	label(f, 370, (cui_rect){x+17, y+h-47, w-34, 30},
	      p->message[0] ? p->message : p->at ? "24-hour local time" : "Press Enter to start",
	      p->message[0] ? f->config->palette[SchemeUrg][ColBg] : muted, small,
	      regular, CUI_ALIGN_START);
}

static void
palette(Frame *f, const ShellPanel *p, const ShellEntry *entries, int count,
        cui_color fg, cui_color muted, cui_color accent, cui_color ink, cui_color card, cui_color outline)
{
	float x = p->pad, y = p->pad, w = p->width, h = p->height;
	box(f, 110, (cui_rect){x+15, y+15, w-30, 44}, f->config->panelradius, card, alpha(accent, 170), 1, 0);
	glyph(f, 111, x+28, y+29, 16, ActPalette, accent);
	label(f, 112, (cui_rect){x+53, y+15, w-82, 44},
	      p->input[0] ? p->input : "Find a window, tag, or layout…",
	      p->input[0] ? fg : muted, f->size+1, medium, CUI_ALIGN_START);
	if (p->input[0]) {
		float caret = textwidth(f, p->input, f->size+1, medium);
		if (caret < w-84)
			box(f, 114, (cui_rect){x+54+caret, y+27, 1, 20}, 0, accent, transparent, 0, 0);
	}
	int rows = MAX(1, MIN(8, ((int)h-107)/38));
	int first = MIN(MAX(0, p->selected-rows+1), MAX(0, count-rows));
	if (!count)
		label(f, 113, (cui_rect){x+18, y+74, w-36, 60}, "No matching commands",
		      muted, f->size, regular, CUI_ALIGN_CENTER);
	for (int row = 0; row < rows && first+row < count; ++row) {
		int i = first+row;
		float yy = y+69+row*38;
		cui_rect r = {x+15, yy, w-30, 36};
		int active = i == p->selected;
		if (active || inside(r, f->hover_x, f->hover_y))
			box(f, 150+row*4, r, f->config->panelradius, active ? accent : card, transparent, 0, 0);
		float detail = MIN(w/3, textwidth(f, entries[i].detail, f->size-1, regular) + 12);
		label(f, 151+row*4, (cui_rect){x+26, yy, w-53-detail, 36}, entries[i].label,
		      active ? ink : fg, f->size, medium, CUI_ALIGN_START);
		label(f, 152+row*4, (cui_rect){x+w-26-detail, yy, detail, 36}, entries[i].detail,
		      active ? ink : muted, f->size-1, regular, CUI_ALIGN_END);
		hit(f, r, ActChoose, i);
	}
	box(f, 200, (cui_rect){x+15, y+h-35, w-30, 1}, 0, outline, transparent, 0, 0);
	label(f, 201, (cui_rect){x+17, y+h-33, w-34, 29}, "↑ ↓  Navigate     ↵  Select     Esc  Close",
	      muted, f->size-2, mono, CUI_ALIGN_START);
}

int
shell_panel_draw(const ShellHost *host, const ShellFont *font, unsigned revision,
                 const Config *c, ShellPanel *p, const ShellTimer *timer,
                 const ShellEntry *entries, int count, time_t now, double tick)
{
	int w = p->width+2*p->pad, h = p->height+2*p->pad;
	if (!p->win || !begin(host, font, revision, &p->surface, p->win, w, h, 0.016))
		return 0;
	Frame f = {p->surface.ui, font, c, p->hits, 0, COUNT(p->hits),
	           p->hover_x, p->hover_y, host->composited, 1, font->size};
	cui_set_reduced_motion(f.ui, c->reduced_motion != 0);
	cui_color fg = c->palette[SchemeStatus][ColFg], bg = alpha(c->palette[SchemeStatus][ColBg], 255);
	cui_color accent = alpha(c->palette[SchemeSel][ColBg], 255), ink = c->palette[SchemeSel][ColFg];
	cui_color card = blend(bg, fg, 0.10f), muted = blend(bg, fg, 0.64f);
	cui_color outline = blend(bg, fg, 0.19f);
	float radius = c->panelradius;
	cui_rect body = {p->pad, p->pad, p->width, p->height};
	/* The primary surface settles in place while its input region stays stable. */
	float t = !c->reduced_motion && c->duration && host->composited
	          ? fmin(1, (tick-p->opened)*1000/c->duration) : 1;
	cui_rect animated = body;
	if (p->pad)
		animated.y += (1-t) * 4 * (c->topbar ? -1 : 1);
	int opacity = host->composited ? c->panel_opacity : 255;
	box(&f, 100, animated, radius, alpha(bg, opacity * (0.92f+0.08f*t)), outline, 1, 2);
	struct tm local = {0};
	localtime_r(&now, &local);
	if (p->kind == ActCalendar)
		calendar(&f, p, &local, fg, muted, accent, ink, card);
	else if (p->kind == ActClock)
		world_clock(&f, p, timer, &local, now, tick, fg, muted, accent, ink, card, outline);
	else
		palette(&f, p, entries, count, fg, muted, accent, ink, card, outline);
	if (render(host, &p->surface)) {
		p->nhits = f.nhits;
		shape(host, &p->surface, p->win, w, h, &body, 1, radius, p->pad);
		blit(host, &p->surface, p->win);
	}
	return t < 1;
}

static int
parse_time(const char *input, int *parts, int fields)
{
	int count = 0;
	while (*input && count < fields) {
		if (!isdigit((unsigned char)*input))
			return 0;
		char *end;
		long n = strtol(input, &end, 10);
		if (n > 1440 || end == input)
			return 0;
		parts[count++] = n;
		if (!*end)
			return count;
		if (*end != ':' || !end[1])
			return 0;
		input = end + 1;
	}
	return 0;
}

int
shell_timer_set(ShellTimer *timer, int at, const char *input, char error[96])
{
	int p[3] = {0};
	int n = parse_time(input, p, at ? 2 : 3);
	const char *message = at ? "Use a local time such as 16:30." : "Use minutes or HH:MM:SS, up to 24 hours.";
	if (at && n == 2 && p[0] < 24 && p[1] < 60) {
		time_t current = time(NULL);
		struct tm local;
		if (!localtime_r(&current, &local))
			goto invalid;
		if (p[0]*60+p[1] <= local.tm_hour*60+local.tm_min)
			++local.tm_mday;
		local.tm_hour = p[0];
		local.tm_min = p[1];
		local.tm_sec = 0;
		local.tm_isdst = -1;
		time_t wall = mktime(&local);
		if (wall <= current || local.tm_hour != p[0] || local.tm_min != p[1]) {
			message = "This local time does not occur on the due date.";
			goto invalid;
		}
		return shell_timer_preset(timer, 1, wall, error);
	}
	if (!at && n >= 1) {
		int seconds = n == 1 ? 60*p[0] : p[0]*3600 + p[1]*60 + p[2];
		if (seconds > 0 && seconds <= 86400 && p[1] < 60 && p[2] < 60)
			return shell_timer_preset(timer, 0, seconds, error);
	}
invalid:
	snprintf(error, 96, "%s", message);
	return 0;
}

int
shell_timer_preset(ShellTimer *timer, int at, time_t value, char error[96])
{
	double duration = at ? difftime(value, time(NULL)) : value;
	if (duration <= 0) {
		snprintf(error, 96, "That time has passed. Choose an upcoming time.");
		return 0;
	}
	*timer = (ShellTimer){.active = 1, .at = at, .duration = duration,
	                      .wall = at ? value : 0, .due = at ? 0 : shell_seconds() + duration};
	return 1;
}

double
shell_timer_remaining(const ShellTimer *timer, time_t now, double tick)
{
	return timer->at ? difftime(timer->wall, now) : timer->due - tick;
}

void
shell_timer_notify(const Config *config, int xfd)
{
	pid_t child = fork();
	if (child < 0) {
		perror("mwm: timer");
		return;
	}
	if (child)
		return;
	close(xfd);
	if (config->hook[0]) {
		execlp(config->hook, config->hook, "Timer complete", "Your mwm timer has finished.", (char *)NULL);
		perror("mwm: timer hook");
	}
	execlp("notify-send", "notify-send", "--app-name=mwm", "--icon=alarm-symbolic",
	       "Timer complete", "Your mwm timer has finished.", (char *)NULL);
	perror("mwm: notify-send");
	_exit(127);
}
