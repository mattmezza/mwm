#include "shell.h"

#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <fontconfig/fontconfig.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cui/cui_icon.h"
#include "cui/cui_styled_slider_events.h"
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
	cui_rect group;
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

int
shell_surface_begin(const ShellHost *host, const ShellFont *font, unsigned revision,
                    ShellSurface *surface, Window win, int w, int h, double elapsed)
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
      const cui_rect *boxes, int n, int radius, int outset, int input, int top, int bottom)
{
	if (!host->shape || (surface->shape_count == n && surface->shape_radius == radius
	    && surface->shape_outset == outset && surface->shape_top == top &&
	    surface->shape_bottom == bottom && !memcmp(surface->shapes, boxes, n * sizeof *boxes)))
		return;
	memcpy(surface->shapes, boxes, n * sizeof *boxes);
	surface->shape_count = n;
	surface->shape_radius = radius;
	surface->shape_outset = outset;
	surface->shape_top = top;
	surface->shape_bottom = bottom;
	Display *dpy = host->dpy;
	Pixmap mask = XCreatePixmap(dpy, win, w, h, 1);
	GC gc = XCreateGC(dpy, mask, 0, NULL);
	XSetForeground(dpy, gc, 0);
	XFillRectangle(dpy, mask, gc, 0, 0, w, h);
	XSetForeground(dpy, gc, 1);
	for (int i = 0; i < n; ++i)
		rounded(dpy, mask, gc, boxes[i], radius);
	XRectangle input_bounds = {0, top, w, MAX(0, bottom-top)};
	if (input) {
		XShapeCombineMask(dpy, win, ShapeInput, 0, 0, mask, ShapeSet);
		if (top > 0 || bottom < h)
			XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, &input_bounds, 1, ShapeIntersect, Unsorted);
	} else {
		XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
	}
	if (outset)
		for (int i = 0; i < n; ++i) {
			cui_rect bounds = {boxes[i].x - outset, boxes[i].y - outset,
			                   boxes[i].w + 2*outset, boxes[i].h + 2*outset};
			rounded(dpy, mask, gc, bounds, radius + outset);
		}
	XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, mask, ShapeSet);
	if (top > 0 || bottom < h)
		XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, &input_bounds, 1, ShapeIntersect, Unsorted);
	XFreeGC(dpy, gc);
	XFreePixmap(dpy, mask);
}

int
shell_surface_draw(const ShellHost *host, ShellSurface *surface, Window win,
                   const cui_rect *body, int radius, int outset)
{
	if (!render(host, surface))
		return 0;
	if (body) {
		int w = surface->image->width, h = surface->image->height;
		shape(host, surface, win, w, h, body, 1, radius, outset, 1, 0, h);
	}
	blit(host, surface, win);
	return 1;
}

int
shell_popup_padding(const ShellHost *host, const Config *config)
{
	return host->composited && config->shadows && config->shadow_opacity
	       ? 4 * config->panel_shadow_blur : 0;
}

static int
popup_duration(const Config *c, int duration)
{
	return c->reduced_motion || !c->duration || !c->popup_distance ? 0 : duration;
}

int
shell_popup_entrance(const Config *c, double started, double now, int direction, int *offset)
{
	int duration = popup_duration(c, c->popup_enter);
	double t = duration ? fmin(1, fmax(0, (now-started) * 1000 / duration)) : 1;
	double remaining = 1-t;
	*offset = -direction * (int)round(c->popup_distance * remaining * remaining * remaining);
	return t < 1;
}

void
shell_popup_exit_reset(const ShellHost *host, ShellPopupExit *exit)
{
	if (exit->win)
		XDestroyWindow(host->dpy, exit->win);
	shell_surface_free(host, &exit->surface);
	*exit = (ShellPopupExit){0};
}

void
shell_popup_dismiss(const ShellHost *host, const Config *c, ShellPopupExit *exit,
                   Window win, ShellSurface *surface, int direction)
{
	if (!win)
		return;
	Display *dpy = host->dpy;
	shell_popup_exit_reset(host, exit);
	XWindowAttributes attrs;
	int duration = popup_duration(c, c->popup_exit);
	if (!duration || !host->shape || !surface->image || !XGetWindowAttributes(dpy, win, &attrs)) {
		XDestroyWindow(dpy, win);
		shell_surface_free(host, surface);
		return;
	}
	XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
	*exit = (ShellPopupExit){.surface = *surface, .win = win, .x = attrs.x, .y = attrs.y,
	                        .distance = direction * c->popup_distance, .duration = duration, .started = shell_seconds()};
	*surface = (ShellSurface){0};
}

int
shell_popup_exit_tick(const ShellHost *host, ShellPopupExit *exit, double now)
{
	if (!exit->win)
		return 0;
	double t = fmax(0, (now-exit->started) * 1000 / exit->duration);
	if (t >= 1) {
		shell_popup_exit_reset(host, exit);
		return 0;
	}
	double progress = t * t * (3 - 2*t);
	XMoveWindow(host->dpy, exit->win, exit->x,
	            exit->y + (int)round(exit->distance * progress));
	blit(host, &exit->surface, exit->win);
	return 1;
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

void
shell_popup_background(cui_context *ui, const ShellHost *host, const Config *c,
                      cui_rect body, float opacity)
{
	Frame f = {.ui = ui, .config = c, .composite = host->composited};
	cui_color fg = c->palette[SchemeStatus][ColFg];
	cui_color bg = alpha(c->palette[SchemeStatus][ColBg], 255);
	cui_color outline = blend(bg, fg, 0.19f);
	int a = host->composited ? c->panel_opacity * opacity : 255;
	box(&f, 100, body, c->panelradius, alpha(bg, a), outline, 1, 2);
}

static float
fontwidth(const ShellFont *font, const char *s, float size, cui_text_style style)
{
	return ceilf(cui_font_measure_styled(font->font, cui_string(s), size, style).width);
}

static float
textwidth(const Frame *f, const char *s, float size, cui_text_style style)
{
	return fontwidth(f->font, s, size, style);
}

static void
ellipsize(const ShellFont *font, char *out, size_t capacity, const char *s,
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
	float measured = fontwidth(font, out, size, style);
	if (!s[n] && (measured > 0 || n == 0) && measured <= width)
		return;
	if (fontwidth(font, "…", size, style) > width) {
		*out = 0;
		return;
	}
	do {
		memcpy(out + n, "…", sizeof "…");
		measured = fontwidth(font, out, size, style);
		if (!n || (measured > 0 && measured <= width))
			return;
		do {
			--n;
		} while (((unsigned char)s[n] & 0xc0) == 0x80);
	} while (1);
}

void
shell_text(cui_context *ui, const ShellFont *font, int id, cui_rect r, const char *s,
           cui_color color, float size, cui_text_style style, cui_align align)
{
	if (r.w <= 0 || r.h <= 0 || !s[0])
		return;
	char text[640];
	ellipsize(font, text, sizeof text, s, r.w, size, style);
	if (!*text)
		return;
	cui_begin_absolute_box(ui, (cui_id)id, (cui_box){
		.width = r.w, .height = r.h, .align_x = align, .align_y = CUI_ALIGN_CENTER,
	}, r);
	cui_text_line(ui, cui_string(text), size, color, ceilf(size + 4), style);
	cui_end_box(ui);
}

static void
label(Frame *f, int id, cui_rect r, const char *s, cui_color color, float size,
      cui_text_style style, cui_align align)
{
	shell_text(f->ui, f->font, id, r, s, color, size, style, align);
}

static const char *icons[ActTimerFoldOpen + 1] = {
	/* Outline glyphs drawn on the same 24px grid as Heroicons. */
	[ActCalendar] = "<rect x='3' y='5' width='18' height='16' rx='3'/><path d='M7 3v4m10-4v4M3 10h18m-14 5h2m3 0h2m3 0h1m-11 3h2m3 0h2'/>",
	[ActClock] = "<circle cx='12' cy='12' r='9'/><path d='M12 7v5l3 2'/>",
	[ActPalette] = "<circle cx='10.5' cy='10.5' r='5.5'/><path d='m15 15 5 5'/>",
	[ActPast] = "<path d='m14.5 6-6 6 6 6'/>",
	[ActFuture] = "<path d='m9.5 6 6 6-6 6'/>",
	[ActTimerToggle] = "<path d='m6 9 6 6 6-6'/>",
	[ActTimerFoldOpen] = "<path d='m6 15 6-6 6 6'/>",
	[ActPresetPrevious] = "<path d='m14.5 6-6 6 6 6'/>",
	[ActPresetNext] = "<path d='m9.5 6 6 6-6 6'/>",
};

/* Runware layout concepts redrawn on the bar's 24px outline grid. */
static const char *layout_icons[] = {
	"<rect x='3' y='5.5' width='18' height='13' rx='1.5'/><path d='M9 5.5v13m6-13v13'/>",
	"<rect x='3' y='5.5' width='18' height='13' rx='1.5'/><path d='M12 5.5v13m0-6.5h9'/>",
	"<path d='M7 15.5H4.5A1.5 1.5 0 0 1 3 14V5.5A1.5 1.5 0 0 1 4.5 4h12A1.5 1.5 0 0 1 18 5.5V8'/>"
	"<rect x='7' y='8' width='14' height='12' rx='1.5'/>",
};

static const char *usage_icons[MetricControl] = {
	[UsageCpu] = "<rect x='5' y='5' width='14' height='14' rx='2.5'/>"
	             "<rect x='9' y='9' width='6' height='6' rx='1'/>"
	             "<path d='M9 2.5V5m6-2.5V5M9 19v2.5m6-2.5v2.5M2.5 9H5m-2.5 6H5m14-6h2.5m-2.5 6h2.5'/>",
	[UsageRam] = "<rect x='2.5' y='6.5' width='19' height='10' rx='1.5'/>"
	             "<path d='M6 10h4v3H6zM14 10h4v3h-4zM6 16.5v3m4-3v3m4-3v3m4-3v3'/>",
	[UsageGpu] = "<rect x='3' y='6' width='18' height='12' rx='2'/>"
	             "<circle cx='11' cy='12' r='3.6'/><path d='M11 8.4 12 12l-3 2.8M4 18v2m3 0h5m5-10v4M1.5 8v8'/>",
	[MetricNetwork] = "<path d='M7.5 4v16M4 7.5 7.5 4 11 7.5M16.5 20V4M13 16.5l3.5 3.5 3.5-3.5'/>",
	[MetricDisk] = "<rect x='4' y='3' width='16' height='18' rx='3'/><circle cx='12' cy='10' r='3.5'/>"
	               "<path d='m12 10 4 6m-8 1h.01'/>",
};

/* The silhouette and fill share a 24px grid so the level clips cleanly. */
static const char *control_outline[ControlCount] = {
	[ControlBattery] = "<rect x='6.5' y='4.5' width='11' height='17.5' rx='2.6'/><path d='M10 2h4'/>",
	[ControlVolume] = "<path d='M4 9h3l5-4v14l-5-4H4zM15.5 9a4.4 4.4 0 0 1 0 6m2.8-8.8a8 8 0 0 1 0 11.6'/>",
	[ControlBrightness] = "<circle cx='12' cy='12' r='5.1'/>"
	                      "<path d='M12 2v2m0 16v2M2 12h2m16 0h2M4.9 4.9l1.4 1.4m11.4 11.4 1.4 1.4"
	                      "m0-14.2-1.4 1.4M6.3 17.7l-1.4 1.4'/>",
};
static const char *control_fill[ControlCount] = {
	[ControlBattery] = "<rect x='8.4' y='6.3' width='7.2' height='13.9' rx='1.15' fill='#fff' stroke='none'/>",
	[ControlVolume] = "<path d='M4 9h3l5-4v14l-5-4H4z' fill='#fff' stroke='none'/>",
	[ControlBrightness] = "<circle cx='12' cy='12' r='4.8' fill='#fff' stroke='none'/>",
};
static const char *control_name[ControlCount] = {"Battery", "Volume", "Brightness"};
static void
icon(Frame *f, int id, cui_rect rect, const char *paths, cui_color fg, cui_icon_state *state)
{
	if (!state->internal) {
		char svg[512];
		snprintf(svg, sizeof svg,
		         "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
		         "<g fill='none' stroke='#ffffff' stroke-width='1.7' stroke-linecap='round' "
		         "stroke-linejoin='round'>%s</g></svg>", paths);
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
static cui_icon_state layout_states[COUNT(layout_icons)];
static cui_icon_state usage_states[MetricControl];
static cui_icon_state control_outline_states[ControlCount], control_fill_states[ControlCount];
static cui_icon_state charge_state, mute_state;

void
shell_icons_free(void)
{
	for (int i = 0; i < COUNT(icon_states); ++i)
		cui_icon_state_destroy(&icon_states[i]);
	for (int i = 0; i < COUNT(layout_states); ++i)
		cui_icon_state_destroy(&layout_states[i]);
	for (int i = 0; i < MetricControl; ++i)
		cui_icon_state_destroy(&usage_states[i]);
	for (int i = 0; i < ControlCount; ++i) {
		cui_icon_state_destroy(&control_outline_states[i]);
		cui_icon_state_destroy(&control_fill_states[i]);
	}
	cui_icon_state_destroy(&charge_state);
	cui_icon_state_destroy(&mute_state);
}

static void
glyph(Frame *f, int id, float x, float y, int size, enum ShellAction kind, cui_color color)
{
	icon(f, id, (cui_rect){x, y, size, size}, icons[kind], color, &icon_states[kind]);
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

static cui_rect
group_rect(const Frame *f, cui_rect r)
{
	if (!f->group.w)
		return r;
	float left = fmaxf(0, r.x), top = fmaxf(0, r.y);
	return (cui_rect){f->group.x+left, f->group.y+top,
	                  fmaxf(0, fminf(r.x+r.w, f->group.w)-left),
	                  fmaxf(0, fminf(r.y+r.h, f->group.h)-top)};
}

static void
hit(Frame *f, cui_rect r, enum ShellAction action, time_t value)
{
	r = group_rect(f, r);
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
	box(f, id, r, MIN(f->config->panelradius, 8), bg, transparent, 0, 0);
	if (icon_size) {
		enum ShellAction glyph_kind = action == ActZonePrevious ? ActPast
		                              : action == ActZoneNext ? ActFuture : action;
		glyph(f, id + 1, r.x + (r.w - icon_size) / 2, r.y + (r.h - icon_size) / 2,
		      icon_size, glyph_kind, fg);
	}
	label(f, id + 2, (cui_rect){r.x + 2, r.y, r.w - 4, r.h}, text, fg,
	      f->size, regular, CUI_ALIGN_CENTER);
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
	ellipsize(f->font, clipped, sizeof clipped, text, r.w, f->size, mono);
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
          cui_color bg, cui_color fg, cui_color accent, int with_icon, double progress)
{
	if (r.w < 12)
		return;
	if (inside(r, f->hover_x, f->hover_y))
		box(f, id, r, MIN(f->config->segradius, 7), blend(bg, fg, 0.12f * f->hover), transparent, 0, 0);
	if (with_icon) {
		float xx = r.x + 7, yy = r.y + (r.h - 18) / 2;
		if (progress < 0) {
			glyph(f, id + 1, xx, yy, 18, action, accent);
		} else {
			cui_rect circle = {xx+2, yy+2, 14, 14};
			box(f, 116, circle, 7, blend(bg, accent, 0.18f), accent, 1, 0);
			float fill = roundf(14 * progress);
			if (fill > 0) {
				cui_rect window = {circle.x, circle.y+14-fill, circle.w, fill};
				cui_begin_absolute_box(f->ui, 117, (cui_box){.width = 14, .height = fill, .clip = true}, window);
				box(f, 118, (cui_rect){0, fill-14, 14, 14}, 7, accent, transparent, 0, 0);
				cui_end_box(f->ui);
			}
		}
	}
	int pad = with_icon ? 31 : 3;
	label(f, id + 2, (cui_rect){r.x + pad, r.y, r.w - pad - 3, r.h}, text,
	      fg, f->size, mono, CUI_ALIGN_CENTER);
	hit(f, r, action, 0);
}

float
shell_reveal(ShellReveal *state, int shown, double elapsed, int duration)
{
	if (!duration) {
		*state = (ShellReveal){.progress = shown, .shown = shown};
		return shown;
	}
	/* Play the same easing curve backward when a segment closes, including
	 * when it reverses before the transition has finished. */
	double step = elapsed * 1000 / duration;
	float t = fmin(1, fmax(0, state->progress + (state->shown ? step : -step)));
	*state = (ShellReveal){.progress = t, .shown = shown};
	return t*t*(3 - 2*t);
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
	int width = 0;
	for (int i = 0; i < m->ntags; ++i)
		width += tag_width(f, &m->tags[i]);
	return width;
}

static void
bar_tags(Frame *f, ShellBar *bar, cui_rect area, const ShellBarModel *m,
         double tick, int duration)
{
	const Config *c = f->config;
	int width = area.w, y = area.y, height = area.h;
	float size = f->size;
	cui_color norm = c->palette[SchemeNorm][ColFg];
	cui_color norm_bg = surface_color(f, SchemeNorm);
	int leftw = 0;
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
	if (leftw > 0) {
		cui_rect left = {area.x, y, width, height};
		int x = area.x;
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
			float sx = left.x + fmaxf(bar->selected_x, 0);
			float sw = fminf(bar->selected_w, left.x + left.w - sx);
			box(f, 21, (cui_rect){sx, selected.y, sw, selected.h}, c->tagradius,
			    surface_color(f, SchemeSel), transparent, 0, 0);
		} else {
			bar->selected_w = 0;
		}
		x = area.x;
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
	} else {
		bar->selected_w = 0;
	}
}

static void
bar_layout(Frame *f, cui_rect r, int layout, cui_color accent)
{
	if (layout < 0 || layout >= COUNT(layout_icons))
		return;
	int size = MIN(22, (int)MIN(r.h, r.w));
	cui_rect image = {r.x + ((int)r.w-size)/2, r.y + ((int)r.h-size)/2, size, size};
	icon(f, 26, image, layout_icons[layout], accent, &layout_states[layout]);
	hit(f, r, ActLayout, 0);
}

enum { GroupPadding = 8, GroupGap = 9, SystemGap = 5, UsageIconSize = 18, UsageMinWidth = 16, LoadMinWidth = 18 };
static const char *metric_name[MetricControl] = {
	[UsageCpu] = "CPU utilization", [UsageGpu] = "GPU utilization", [UsageRam] = "RAM utilization",
	[MetricLoad] = "Load average", [MetricNetwork] = "Network activity", [MetricDisk] = "Disk activity",
};

static const cui_chart_datum *
latest_usage(const ShellUsage *usage)
{
	return usage->count ? &usage->history[usage->count-1] : NULL;
}

static const Temperature *
usage_temperature(const ShellSystem *system, int metric)
{
	if (metric == UsageCpu)
		return &system->activity.cpu_temperature;
	if (metric == UsageGpu)
		return &system->activity.gpu_temperature;
	return NULL;
}

static int
bar_temperature(const Frame *f, const Temperature *sensor, char *text, size_t size)
{
	if (!f->config->show_temperature || !sensor || !sensor->path[0])
		return 0;
	snprintf(text, size, "%d°C", (int)round(sensor->millidegrees / 1000.0));
	return MAX(textwidth(f, "000°C", f->size, mono), textwidth(f, text, f->size, mono));
}

static cui_color
temperature_color(const Config *c, int metric, int millidegrees, cui_color fg)
{
	int critical = metric == UsageCpu ? c->cpu_temp_critical : c->gpu_temp_critical;
	return millidegrees >= critical * 1000 ? c->usage_palette[UsageDanger] : fg;
}

static cui_color
usage_color(const Config *config, cui_color accent, double percent)
{
	cui_color warning = config->usage_palette[UsageWarning];
	cui_color danger = config->usage_palette[UsageDanger];
	if (percent <= 75)
		return blend(accent, warning, fmax(0, (percent - 50) / 25));
	return blend(warning, danger, fmin(1, (percent - 75) / 15));
}

static cui_color
control_color(const Config *c, enum Control kind, int level, cui_color accent)
{
	if (kind != ControlBattery || level > c->battery_low)
		return accent;
	if (level <= c->battery_critical)
		return c->usage_palette[UsageDanger];
	return blend(c->usage_palette[UsageDanger], c->usage_palette[UsageWarning],
	             (float)(level-c->battery_critical)/(c->battery_low-c->battery_critical));
}

static void
level_icon(Frame *f, int id, cui_rect r, enum Control kind, const ControlReading *data,
           int value, cui_color fg, cui_color bg, cui_color accent)
{
	static const float floor[] = {20.2f, 19, 16.8f}, ceiling[] = {6.3f, 5, 7.2f};
	cui_color color = control_color(f->config, kind, value, accent);
	int fill_value = data->available && !data->muted ? value : 0;
	float scale = r.h / 24;
	float cut = (floor[kind] - (floor[kind]-ceiling[kind]) * fill_value / 100) * scale;
	if (fill_value) {
		clip(f, id, (cui_rect){r.x, r.y+cut, r.w, r.h-cut});
		icon(f, id+1, (cui_rect){0, -cut, r.w, r.h}, control_fill[kind], color, &control_fill_states[kind]);
		cui_end_box(f->ui);
	}
	icon(f, id+2, r, control_outline[kind], data->available ? color : alpha(fg, 80),
	     &control_outline_states[kind]);
	if (data->charging)
		icon(f, id+3, r, "<path d='m12.9 7.8-3 5h2.2l-.9 3.4 3-4.9h-2.1z' fill='#fff' stroke-width='.65'/>",
		     value >= 65 ? alpha(bg, 255) : fg, &charge_state);
	if (data->muted)
		icon(f, id+4, r, "<path d='m3 20 17-17'/>", fg, &mute_state);
}

static void
bar_usage(Frame *f, ShellUsageChart *chart, const ShellUsage *usage, const Temperature *sensor,
          int kind, int history, cui_rect r, cui_color fg, cui_color accent)
{
	int id = 60 + kind*8;
	cui_rect symbol = {r.x, r.y + ((int)r.h-UsageIconSize)/2, UsageIconSize, UsageIconSize};
	icon(f, id, symbol, usage_icons[kind], accent, &usage_states[kind]);

	int left = r.x + UsageIconSize + SystemGap;
	int width = (int)(r.x + r.w) - left;
	char temperature[24];
	int temperature_width = bar_temperature(f, sensor, temperature, sizeof temperature);
	if (temperature_width && width >= UsageMinWidth + SystemGap + temperature_width) {
		cui_rect value = {r.x+r.w-temperature_width, r.y, temperature_width, r.h};
		label(f, id+1, value, temperature, temperature_color(f->config, kind, sensor->millidegrees, fg),
		      f->size, mono, CUI_ALIGN_END);
		width -= SystemGap + temperature_width;
	}
	if (width < UsageMinWidth)
		return;
	int height = MIN(USAGE_CHART_HEIGHT, (int)r.h - 8);
	int top = r.y + (int)(r.h - height)/2;
	box(f, id+2, (cui_rect){left, top + height/2, width, 1}, 0,
	    alpha(fg, 32), transparent, 0, 0);

	int count = MIN(usage->count, history);
	if (count < 2)
		return;
	int span = MAX(2, (int)round((width - 1.0) * (count - 1) / (history - 1)) + 1);
	chart->state.pixels = chart->pixels;
	chart->state.byte_capacity = sizeof chart->pixels;
	cui_color color = usage_color(f->config, accent, latest_usage(usage)->value);
	cui_rect graph = {left + width - span, top, span, height};
	cui_begin_absolute_box(f->ui, id+3, (cui_box){.width = span, .height = height}, graph);
	cui_chart(f->ui, id+4, &chart->state, &(cui_chart_options){
		.kind = CUI_CHART_AREA, .curve = CUI_CHART_CURVE_LINEAR,
		.data = usage->history + usage->count - count, .count = count, .data_revision = usage->revision,
		.width = span, .height = height, .has_domain = true, .domain_min = 0, .domain_max = 100,
		.hide_axes = true, .hide_x_axis = true, .hide_grid = true, .hide_legend = true,
		.noninteractive = true, .disable_appear = true,
		.custom_y_padding = true, .y_padding_top = 1, .y_padding_bottom = 1,
		.series = {color}, .primary_fill = alpha(color, 55), .line_width = 1.5f,
	}, cui_string(metric_name[kind]));
	cui_end_box(f->ui);
}

static void
bar_load(Frame *f, const ShellSystem *system, cui_rect r, cui_color fg, cui_color accent)
{
	const int gap = 3;
	int width = ((int)r.w - (LoadPeriods-1)*gap) / LoadPeriods;
	int height = MIN(20, (int)r.h - 8);
	int x = r.x + ((int)r.w - LoadPeriods*width - (LoadPeriods-1)*gap)/2;
	int y = r.y + ((int)r.h-height)/2;
	float radius = MIN(2, width/2.f);
	/* The square-root scale leaves room for spikes above one job per logical CPU
	 * while keeping low load differences legible in a short track. */
	int capacity = round(height * sqrt(1 / 1.5));
	for (int i = 0; i < LoadPeriods; ++i, x += width + gap) {
		int id = 100 + 3*i;
		box(f, id, (cui_rect){x, y, width, height}, radius, alpha(fg, 29), transparent, 0, 0);
		if (system->load_valid && system->load[i] > 0) {
			double ratio = system->load[i] / system->cpus;
			int fill = MAX(1, round(height * sqrt(fmin(ratio / 1.5, 1))));
			cui_color color = usage_color(f->config, accent, 100 * ratio);
			box(f, id+1, (cui_rect){x, y+height-fill, width, fill}, radius,
			    color, transparent, 0, 0);
		}
		box(f, id+2, (cui_rect){x, y+height-capacity, width, 1}, 0, alpha(fg, 104), transparent, 0, 0);
	}
}

static void
bar_traffic(Frame *f, const Traffic *traffic, int metric, cui_rect r, cui_color fg, cui_color accent)
{
	int network = metric == MetricNetwork;
	int id = network ? 2200 : 2250;
	cui_rect symbol = {r.x, r.y + ((int)r.h-UsageIconSize)/2, UsageIconSize, UsageIconSize};
	icon(f, id, symbol, usage_icons[metric], accent, &usage_states[metric]);
	int left = r.x + UsageIconSize + SystemGap;
	int width = (int)(r.x + r.w) - left;
	if (width < 12)
		return;
	int middle = r.y + (int)r.h/2;
	int half = MIN(10, ((int)r.h - 8)/2);
	int cols = MIN(TRAFFIC_SAMPLES, width/3);
	int first = MAX(0, traffic->count-cols);
	double peak = network ? 64*1024 : 1024*1024;
	for (int i = first; i < traffic->count; ++i)
		for (int k = 0; k < 2; ++k)
			peak = fmax(peak, traffic->history[i][k]);
	box(f, id+1, (cui_rect){left, middle, width, 1}, 0, alpha(fg, 52), transparent, 0, 0);
	for (int i = 0; i < cols; ++i) {
		int sample = traffic->count - cols + i;
		int x = left + i*width/cols;
		int w = (i+1)*width/cols - i*width/cols - 1;
		for (int k = 0; k < 2; ++k) {
			double value = sample < 0 ? 0 : traffic->history[sample][k];
			int height = value > 0 ? MAX(1, round(half * sqrt(value / peak))) : 0;
			cui_color color = k ? blend(accent, fg, 0.54f) : accent;
			if (!network) {
				/* Read and write use two parallel rows, both filling upward. */
				int floor = middle + (k ? half+1 : -2);
				box(f, id+2+i*4+k, (cui_rect){x, floor-1, w, 1}, 0, alpha(fg, 48), transparent, 0, 0);
				height = height ? MAX(1, height-2) : 0;
				box(f, id+4+i*4+k, (cui_rect){x, floor-height, w, height}, 1, color, transparent, 0, 0);
			} else {
				box(f, id+2+i*4+k, (cui_rect){x, k ? middle+2 : middle-height, w, height},
				    1, color, transparent, 0, 0);
			}
		}
	}
}

int
shell_bar_metric(const ShellBar *bar, int x, int y)
{
	for (int i = 0; i < MetricCount; ++i)
		if (inside(bar->metrics[i], x, y))
			return i;
	return MetricNone;
}

typedef struct {
	int wanted, minimum, width;
} ItemSize;

typedef struct {
	int wanted, minimum, width, gap, x, priority;
	float level;
	enum BarAlign align;
} BarSlot;

static const enum BarItem item_priority[] = {
	BarTags, BarTime, BarBattery, BarVolume, BarBrightness, BarCpu, BarGpu, BarRam, BarLoad, BarNetwork, BarDisk,
	BarLayout, BarDate, BarTray, BarStatus, BarTitle,
};

static void
place_segments(const Config *c, BarSlot slots[MAX_BAR_SEGMENTS], int w, int margin)
{
	int order[MAX_BAR_SEGMENTS];
	for (int i = 0; i < c->nsegments; ++i) {
		int j = i;
		while (j > 0 && slots[order[j-1]].priority > slots[i].priority) {
			order[j] = order[j-1];
			--j;
		}
		order[j] = i;
	}
	int free = w - 2*margin;
	for (int i = 0; i < c->nsegments; ++i) {
		BarSlot *s = &slots[order[i]];
		int width = roundf(s->minimum * s->level);
		int gap = roundf(c->barseggap * s->level);
		if (width && width + gap <= free) {
			s->width = width;
			s->gap = gap;
			free -= width + gap;
		}
	}
	for (int i = 0; i < c->nsegments; ++i) {
		BarSlot *s = &slots[order[i]];
		if (s->width) {
			int extra = MIN(free, (int)roundf(s->wanted * s->level) - s->width);
			s->width += extra;
			free -= extra;
		}
	}

	int widths[BarAlignCount] = {0}, trailing[BarAlignCount] = {0};
	for (int i = 0; i < c->nsegments; ++i) {
		const BarSlot *s = &slots[i];
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
		BarSlot *s = &slots[i];
		if (s->width) {
			s->x = x[s->align];
			x[s->align] += s->width + s->gap;
		}
	}
}

static void
measure_segment(BarSlot *slot, const ItemSize items[BarItemCount], const BarPlacement *group)
{
	int count = 0;
	unsigned used = 0;
	for (int i = 0; i < group->count; ++i) {
		enum BarItem item = group->items[i];
		if (!items[item].wanted)
			continue;
		slot->minimum += items[item].minimum;
		slot->wanted += items[item].wanted;
		used |= 1u << item;
		++count;
	}
	if (count) {
		int padding = 2*GroupPadding + (count-1)*GroupGap;
		slot->minimum += padding;
		slot->wanted += padding;
	}
	for (slot->priority = 0; slot->priority < BarItemCount; ++slot->priority)
		if (used & (1u << item_priority[slot->priority]))
			break;
}

static void
fit_items(ItemSize items[BarItemCount], const BarPlacement *group, int width)
{
	int free = width - 2*GroupPadding + GroupGap;
	for (int i = 0; i < group->count; ++i) {
		ItemSize *item = &items[group->items[i]];
		if (item->wanted) {
			item->width = item->minimum;
			free -= item->width + GroupGap;
		}
	}
	for (int i = 0; i < BarItemCount; ++i) {
		ItemSize *item = &items[item_priority[i]];
		if (item->width && free > 0) {
			int extra = MIN(free, item->wanted - item->width);
			item->width += extra;
			free -= extra;
		}
	}
}

static void
bar_item_draw(Frame *f, ShellBar *bar, const ShellBarModel *m, enum BarItem kind, cui_rect r,
              const char *date, const char *clock, const ShellTimer *timer,
              time_t now, double tick, int duration, cui_color bg, cui_color accent)
{
	const Config *c = f->config;
	cui_color fg = c->palette[SchemeStatus][ColFg];
	switch (kind) {
	case BarTags:
		bar_tags(f, bar, r, m, tick, duration);
		break;
	case BarLayout:
		bar_layout(f, r, m->layout, accent);
		break;
	case BarTitle: {
		cui_color ink = c->palette[SchemeTitle][ColFg];
		glyph(f, 41, r.x+1, r.y+(r.h-17)/2, 17, ActPalette, accent);
		char title[256];
		ellipsize(f->font, title, sizeof title, bar->title, r.w-24, f->size, medium);
		label(f, 42, (cui_rect){r.x+24, r.y, r.w-24, r.h}, title,
		      ink, f->size, medium, CUI_ALIGN_START);
		if (m->title[0])
			hit(f, r, ActPalette, 0);
		break;
	}
	case BarStatus:
		status_label(f, r, bar->status, 36, fg);
		break;
	case BarDate:
	case BarTime: {
		int is_time = kind == BarTime;
		const char *text = is_time ? clock : date;
		r.y += 2;
		r.h -= 4;
		int with_icon = r.w >= textwidth(f, text, f->size, mono) + 34;
		double progress = is_time && timer->active && c->timer_indicator && timer->duration > 0
		                  ? 1 - fmin(fmax(shell_timer_remaining(timer, now, tick) / timer->duration, 0), 1) : -1;
		bar_clock(f, r, text, is_time ? ActClock : ActCalendar,
		          is_time ? 15 : 11, bg, fg, accent, with_icon, progress);
		break;
	}
	case BarTray: {
		cui_rect strip = {r.x+r.w-m->tray_width, r.y, m->tray_width, r.h};
		cui_rect available = group_rect(f, r);
		cui_rect visible = group_rect(f, strip);
		int left = MAX(available.x, visible.x);
		bar->tray_x = f->group.x + strip.x;
		bar->tray_clip_x = MAX(0, left - bar->tray_x);
		bar->tray_clip_width = MAX(0, (int)MIN(available.x+available.w, visible.x+visible.w) - left);
		break;
	}
	case BarCpu:
	case BarGpu:
	case BarRam: {
		int metric = kind - BarCpu;
		int history[] = {c->cpu_history, c->gpu_history, c->ram_history};
		bar->metrics[metric] = group_rect(f, r);
		bar_usage(f, &bar->usage_charts[metric], &m->system->usage[metric], usage_temperature(m->system, metric),
		          metric, history[metric], r, fg, accent);
		break;
	}
	case BarLoad:
		bar->metrics[MetricLoad] = group_rect(f, r);
		bar_load(f, m->system, r, fg, accent);
		break;
	case BarNetwork:
	case BarDisk: {
		int metric = kind - BarCpu;
		bar->metrics[metric] = group_rect(f, r);
		bar_traffic(f, kind == BarNetwork ? &m->system->activity.network : &m->system->activity.disk,
		            metric, r, fg, accent);
		break;
	}
	case BarBattery:
	case BarVolume:
	case BarBrightness: {
		enum Control control = kind - BarBattery;
		const ControlReading *data = &m->controls->reading[control];
		int size = 22;
		int level = control == ControlBattery ? data->level : controls_value(m->controls, control);
		cui_rect symbol = {r.x+((int)r.w-size)/2, r.y+((int)r.h-size)/2, size, size};
		level_icon(f, 1000+control*10, symbol, control, data, level, fg, bg, accent);
		bar->metrics[MetricControl+control] = group_rect(f, r);
		hit(f, r, ActBattery+control, 0);
		break;
	}
	case BarItemCount:
		break;
	}
}

int
shell_bar_draw(const ShellHost *host, const ShellFont *font, unsigned revision,
               const Config *c, ShellBar *bar, Window window, const ShellBarModel *m,
               const ShellTimer *timer, time_t now, double tick)
{
	int w = m->w, h = m->h, y = m->pad + m->slide;
	int height = h - 2*m->pad;
	double elapsed = bar->last_frame ? tick - bar->last_frame : 0;
	if (w < 1 || height < 1
	    || !shell_surface_begin(host, font, revision, &bar->surface, window, w, h, fmin(elapsed, 0.1)))
		return 0;

	int duration = c->reduced_motion ? 0 : c->duration;
	bar->animating = 0;
	if (m->status[0])
		snprintf(bar->status, sizeof bar->status, "%s", m->status);
	if (m->title[0])
		snprintf(bar->title, sizeof bar->title, "%s", m->title);
	ShellHit hover = shell_hit(bar->hits, bar->nhits, bar->hover_x, bar->hover_y);
	int hovered = hover.action * (MAX_TAGS + 1) + hover.value;
	if (bar->hovered != hovered) {
		bar->hovered = hovered;
		bar->hover_since = tick;
	}
	float fade = duration ? fminf(1, (tick - bar->hover_since) * 1000 / duration) : 1;
	Frame f = {bar->surface.ui, font, c, bar->hits, 0, COUNT(bar->hits),
	           bar->hover_x, bar->hover_y, host->composited, fade, font->bar_size, {0, 0, 0, 0}};
	cui_set_reduced_motion(f.ui, c->reduced_motion != 0);
	memset(bar->metrics, 0, sizeof bar->metrics);
	bar->tray_clip_width = 0;
	cui_rect shapes[MAX_BAR_SEGMENTS];
	int n = 0, margin = MIN(c->barmargin, w / 8);
	float radius = c->segradius, size = f.size;
	cui_color accent = alpha(c->palette[SchemeSel][ColBg], 255);
	cui_color outline = alpha(c->palette[SchemeNorm][ColBorder], 170);
	cui_color status_bg = surface_color(&f, SchemeStatus), bar_bg = surface_color(&f, SchemeBar);
	cui_rect strip = {0, y, w, height};
	if (bar_bg.a)
		box(&f, 1, strip, 0, bar_bg, transparent, 0, 0);

	struct tm local;
	char date[80] = "", clock[40] = "";
	if (localtime_r(&now, &local)) {
		strftime(date, sizeof date, "%a %d %b", &local);
		strftime(clock, sizeof clock, "%H:%M", &local);
	}
	char status[512];
	cui_text_color_span spans[64];
	status_parse(bar->status, status, spans, c->palette[SchemeStatus][ColFg]);
	int tags = tags_width(&f, m), title = bar->title[0] ? textwidth(&f, bar->title, size, medium) + 24 : 0;
	int statusw = status[0] ? textwidth(&f, status, size, mono) : 0;
	if (m->title[0])
		statusw = MIN(statusw, w/3);
	int dw = textwidth(&f, date, size, mono), cw = textwidth(&f, clock, size, mono);
	ItemSize sizes[BarItemCount] = {
		[BarTags] = {tags, MIN(tags, 32), 0},
		[BarLayout] = {m->layout < 0 ? 0 : LayoutButtonWidth-6, m->layout < 0 ? 0 : 18, 0},
		[BarTitle] = {title, MIN(38, title), 0},
		[BarStatus] = {statusw, MIN(statusw, 24), 0},
		[BarCpu] = {UsageIconSize+SystemGap+c->cpu_width, UsageIconSize, 0},
		[BarGpu] = {UsageIconSize+SystemGap+c->gpu_width, UsageIconSize, 0},
		[BarRam] = {UsageIconSize+SystemGap+c->ram_width, UsageIconSize, 0},
		[BarLoad] = {c->load_width, LoadMinWidth, 0},
		[BarNetwork] = {UsageIconSize+SystemGap+c->network_width, UsageIconSize, 0},
		[BarDisk] = {UsageIconSize+SystemGap+c->disk_width, UsageIconSize, 0},
		[BarDate] = {dw+39, dw+6, 0},
		[BarTime] = {cw+39, cw+6, 0},
		[BarTray] = {m->tray_width, host->shape ? MIN(m->tray_width, 16) : m->tray_width, 0},
		[BarBattery] = {24, 24, 0},
		[BarVolume] = {24, 24, 0},
		[BarBrightness] = {24, 24, 0},
	};
	for (int metric = UsageCpu; metric <= UsageGpu; ++metric) {
		char temperature[24];
		int width = bar_temperature(&f, usage_temperature(m->system, metric), temperature, sizeof temperature);
		if (width)
			sizes[BarCpu+metric].wanted += SystemGap + width;
	}
	unsigned inactive = (!m->title[0] ? 1u<<BarTitle : 0) | (!m->status[0] ? 1u<<BarStatus : 0) |
	                    (!m->tray_shown ? 1u<<BarTray : 0);
	ItemSize current[BarItemCount];
	memcpy(current, sizes, sizeof current);
	for (int i = 0; i < BarItemCount; ++i)
		if (inactive & (1u << i))
			current[i] = (ItemSize){0};
	BarSlot slots[MAX_BAR_SEGMENTS] = {0};
	for (int i = 0; i < c->nsegments; ++i) {
		const BarPlacement *g = &c->segments[i];
		BarSlot *s = &slots[i];
		int item = g->items[0];
		s->align = !c->custom_segments && g->count == 1 && item == BarTitle
		           ? (enum BarAlign)c->titlealign : g->align;
		measure_segment(s, g->count == 1 ? sizes : current, g);
		int shown = s->wanted && !(m->hidden & (1u << i)) &&
		            (g->count > 1 || !(inactive & (1u << item)));
		int motion = duration;
		if (!bar->last_frame)
			motion = 0;
		for (int j = 0; !host->shape && j < g->count; ++j)
			if (g->items[j] == BarTray)
				motion = 0;
		s->level = shell_reveal(&bar->segment_reveal[i], shown, elapsed, motion);
		bar->animating |= bar->segment_reveal[i].progress != shown;
	}
	bar->last_frame = tick;
	place_segments(c, slots, w, margin);

	int tags_drawn = 0;
	for (int i = 0; i < c->nsegments; ++i) {
		const BarSlot *s = &slots[i];
		const BarPlacement *g = &c->segments[i];
		if (!s->width)
			continue;
		cui_rect r = {s->x, y, s->width, height};
		shapes[n++] = r;
		cui_color bg = status_bg;
		if (g->items[0] == BarTags || g->items[0] == BarLayout)
			bg = surface_color(&f, SchemeNorm);
		if (g->items[0] == BarTitle)
			bg = surface_color(&f, SchemeTitle);
		int id = 4000 + i*32;
		box(&f, id, r, radius, bg, outline, bg.a != 0, 1);
		int sliding = s->level < 1;
		Frame child = f;
		if (sliding) {
			clip(&f, id+1, r);
			child.group = r;
			child.hover_x -= r.x;
			child.hover_y -= r.y;
		}
		ItemSize items[BarItemCount];
		cui_rect visible_charts[MetricControl] = {{0, 0, 0, 0}};
		memcpy(items, g->count == 1 ? sizes : current, sizeof items);
		int full = MAX(s->minimum, (int)roundf(r.w / s->level));
		fit_items(items, g, full);
		int x = GroupPadding + (sliding ? 0 : (int)r.x) + (s->align == BarRight ? r.w-full : 0);
		int item_y = sliding ? 0 : r.y;
		int drawn = 0;
		for (int j = 0; j < g->count; ++j) {
			enum BarItem kind = g->items[j];
			int width = items[kind].width;
			if (!width)
				continue;
			if (drawn++ && c->separators)
				box(&child, id+2+j, (cui_rect){x-GroupGap/2-1, item_y+8, 1, height-16},
				    0, outline, transparent, 0, 0);
			cui_rect item = {x, item_y, width, height};
			if (inside(item, child.hover_x, child.hover_y) &&
			    kind != BarTags && kind != BarDate && kind != BarTime && kind != BarTray)
				box(&child, id+16+j, (cui_rect){x-2, item_y+3, width+4, height-6}, radius,
				    blend(bg, c->palette[SchemeStatus][ColFg], 0.08f), transparent, 0, 0);
			if (sliding && kind >= BarCpu && kind <= BarDisk)
				visible_charts[kind-BarCpu] = group_rect(&child, item);
			else
				bar_item_draw(&child, bar, m, kind, item, date, clock, timer, now, tick, duration, bg, accent);
			x += width + GroupGap;
			tags_drawn |= kind == BarTags;
		}
		if (sliding) {
			cui_end_box(f.ui);
			for (int j = 0; j < MetricControl; ++j) {
				cui_rect chart = visible_charts[j];
				int minimum = j == MetricLoad ? LoadMinWidth : UsageIconSize;
				if (chart.w >= minimum)
					bar_item_draw(&f, bar, m, BarCpu+j, chart, date, clock, timer, now, tick, duration, bg, accent);
			}
		}
		f.nhits = child.nhits;
	}
	if (!tags_drawn)
		bar->selected_w = 0;
	bar->animating |= hovered && fade < 1;
	if (render(host, &bar->surface)) {
		bar->nhits = f.nhits;
		if (bar_bg.a)
			shape(host, &bar->surface, window, w, h, &strip, 1, 0, 0, 1, m->clip_top, m->clip_bottom);
		else
			shape(host, &bar->surface, window, w, h, shapes, n, radius, host->composited ? m->pad : 0, 1,
			      m->clip_top, m->clip_bottom);
		if (m->map)
			XMapRaised(host->dpy, window);
		blit(host, &bar->surface, window);
		return 1;
	}
	return 0;
}

void
shell_tooltip_hide(const ShellHost *host, const Config *config, ShellTooltip *tip)
{
	if (tip->win)
		shell_popup_dismiss(host, config, &tip->exit, tip->win, &tip->surface, config->topbar ? 1 : -1);
	tip->win = None;
	tip->mapped = 0;
}

void
shell_tooltip_close(const ShellHost *host, ShellTooltip *tip)
{
	if (tip->win)
		XDestroyWindow(host->dpy, tip->win);
	shell_surface_free(host, &tip->surface);
	shell_popup_exit_reset(host, &tip->exit);
	*tip = (ShellTooltip){0};
}

int
shell_tooltip_motion(const ShellHost *host, const Config *config, ShellTooltip *tip, double now, int frame)
{
	int moving = 0;
	if (tip->mapped) {
		int offset;
		moving = shell_popup_entrance(config, tip->opened, now, config->topbar ? 1 : -1, &offset);
		if (frame && tip->offset != offset) {
			tip->offset = offset;
			XMoveWindow(host->dpy, tip->win, tip->geometry.x, tip->geometry.y + offset);
		}
		moving |= tip->offset != offset;
	}
	if (frame)
		shell_popup_exit_tick(host, &tip->exit, now);
	return moving || tip->exit.win;
}

static void
format_rate(char *text, size_t size, double rate)
{
	const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s"};
	int unit = 0;
	while (rate >= 1024 && unit < 3) {
		rate /= 1024;
		++unit;
	}
	snprintf(text, size, unit ? "%.1f %s" : "%.0f %s", rate, units[unit]);
}

void
shell_tooltip_draw(const ShellHost *host, const ShellFont *font, unsigned revision,
                   const Config *c, ShellTooltip *tip, const ShellSystem *system,
                   const Controls *controls, int metric, cui_rect monitor, cui_rect anchor)
{
	const int padding = 11, gap = 6;
	int load = metric == MetricLoad;
	int kind = metric - MetricControl;
	const char *name = kind >= 0 ? control_name[kind] : metric_name[metric];
	if (metric == UsageGpu && system->activity.gpu_kind == GpuAwake)
		name = "GPU active time";
	const Traffic *traffic = metric == MetricNetwork ? &system->activity.network
	                         : metric == MetricDisk ? &system->activity.disk : NULL;
	const ControlReading *reading = kind >= 0 ? &controls->reading[kind] : NULL;
	const Temperature *sensor = usage_temperature(system, metric);
	char temperature[24] = "Unavailable";
	if (sensor && sensor->path[0])
		snprintf(temperature, sizeof temperature, "%.1f °C", sensor->millidegrees / 1000.0);
	float size = fmaxf(11, font->size - 3);
	Frame f = {.font = font, .config = c, .composite = host->composited, .size = size};
	int pad = host->composited && c->shadows && c->shadow_opacity ? 4*c->bar_shadow_blur : 0;
	int row = ceilf(size) + 8;
	int gpu_missing = metric == UsageGpu && system->activity.gpu_kind == GpuUnavailable;
	const char *example = (reading && !reading->available) || gpu_missing ? "Unavailable"
	                      : reading && reading->muted ? "Muted" : "100%";
	int width = load || traffic ? 13*size : textwidth(&f, name, size, medium) +
	            textwidth(&f, example, size, mono) + 2*padding + 14;
	if (!reading)
		width = MAX(width, textwidth(&f, "Update interval", size-2, regular) +
		            textwidth(&f, "60000 ms", size-2, mono) + 2*padding + 14);
	if (sensor)
		width = MAX(width, textwidth(&f, "Temperature", size-1, regular) +
		            textwidth(&f, temperature, size, mono) + 2*padding + 14);
	int rows = load || traffic ? 3 : 1 + (sensor != NULL);
	int height = (rows + !reading)*row + 12;
	int w = width + 2*pad, h = height + 2*pad;
	if (w > monitor.w || h + anchor.h + gap > monitor.h) {
		shell_tooltip_hide(host, c, tip);
		return;
	}
	int x = roundf(anchor.x + (anchor.w-w)/2);
	int y = c->topbar ? anchor.y + anchor.h + gap - pad : anchor.y - height - gap - pad;
	cui_rect geometry = {fmaxf(monitor.x, fminf(x, monitor.x + monitor.w - w)),
	                     fmaxf(monitor.y, fminf(y, monitor.y + monitor.h - h)), w, h};

	Display *dpy = host->dpy;
	if (!tip->win) {
		tip->opened = shell_seconds();
		shell_popup_entrance(c, tip->opened, tip->opened, c->topbar ? 1 : -1, &tip->offset);
		XSetWindowAttributes attrs = {.override_redirect = True, .background_pixel = 0,
		                              .border_pixel = 0, .colormap = host->cmap, .event_mask = ExposureMask};
		tip->win = XCreateWindow(dpy, host->root, geometry.x, geometry.y + tip->offset, w, h, 0,
		                         host->depth, InputOutput, host->visual,
		                         CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &attrs);
		XClassHint klass = {"mwm-tooltip", "mwm"};
		XSetClassHint(dpy, tip->win, &klass);
		XStoreName(dpy, tip->win, "mwm system tooltip");
	} else if (memcmp(&geometry, &tip->geometry, sizeof geometry)) {
		XMoveResizeWindow(dpy, tip->win, geometry.x, geometry.y + tip->offset, w, h);
	}
	tip->geometry = geometry;
	if (!shell_surface_begin(host, font, revision, &tip->surface, tip->win, w, h, 0))
		return;

	f.ui = tip->surface.ui;
	cui_color fg = c->palette[SchemeStatus][ColFg], bg = alpha(c->palette[SchemeStatus][ColBg], 255);
	cui_color accent = alpha(c->palette[SchemeSel][ColBg], 255);
	cui_color muted = blend(bg, fg, 0.75f), outline = blend(bg, fg, 0.22f);
	cui_rect body = {pad, pad, width, height};
	box(&f, 1, body, c->panelradius, alpha(bg, host->composited ? c->panel_opacity : 255), outline, 1, 1);
	int inner = width - 2*padding;
	label(&f, 2, (cui_rect){pad+padding, pad+6, inner, row}, name,
	      fg, size, medium, CUI_ALIGN_START);
	if (load) {
		static const char *period[] = {"1 min", "5 min", "15 min"};
		int col = inner / LoadPeriods;
		for (int i = 0; i < LoadPeriods; ++i) {
			char text[24] = "–";
			cui_color value_color = fg;
			if (system->load_valid) {
				snprintf(text, sizeof text, "%.2f", system->load[i]);
				value_color = usage_color(c, accent, 100 * system->load[i] / system->cpus);
			}
			int xx = pad+padding+i*col;
			label(&f, 3+i*2, (cui_rect){xx, pad+6+row, col, row}, period[i],
			      muted, size-2, regular, CUI_ALIGN_CENTER);
			label(&f, 4+i*2, (cui_rect){xx, pad+6+2*row, col, row}, text,
			      value_color, size, mono, CUI_ALIGN_CENTER);
		}
	} else if (traffic) {
		const char *labels[] = {metric == MetricNetwork ? "Download" : "Read",
		                        metric == MetricNetwork ? "Upload" : "Write"};
		for (int i = 0; i < 2; ++i) {
			char rate[40] = "–";
			if (traffic->count)
				format_rate(rate, sizeof rate, traffic->history[traffic->count-1][i]);
			cui_rect line = {pad+padding, pad+6+(i+1)*row, inner, row};
			label(&f, 3+i*2, line, labels[i], muted, size-1, regular, CUI_ALIGN_START);
			label(&f, 4+i*2, line, rate, i ? blend(accent, fg, 0.54f) : accent, size, mono, CUI_ALIGN_END);
		}
	} else if (reading) {
		char text[20] = "Unavailable";
		int value = kind == ControlBattery ? reading->level : controls_value(controls, kind);
		if (reading->muted)
			snprintf(text, sizeof text, "Muted");
		else if (reading->available)
			snprintf(text, sizeof text, "%d%%", value);
		label(&f, 3, (cui_rect){pad+padding, pad+6, inner, row}, text,
		      reading->available ? control_color(c, kind, value, accent) : muted, size, mono, CUI_ALIGN_END);
	} else {
		char text[20] = "–";
		const cui_chart_datum *latest = latest_usage(&system->usage[metric]);
		if (latest)
			snprintf(text, sizeof text, "%d%%", (int)round(latest->value));
		else if (gpu_missing)
			snprintf(text, sizeof text, "Unavailable");
		label(&f, 3, (cui_rect){pad+padding, pad+6, inner, row}, text,
		      latest ? usage_color(c, accent, latest->value) : muted, size, mono, CUI_ALIGN_END);
	}
	if (sensor) {
		cui_rect line = {pad+padding, pad+6+row, inner, row};
		cui_color color = sensor->path[0] ? temperature_color(c, metric, sensor->millidegrees, fg) : muted;
		label(&f, 4, line, "Temperature", muted, size-1, regular, CUI_ALIGN_START);
		label(&f, 5, line, temperature, color, size, mono, CUI_ALIGN_END);
	}
	if (!reading) {
		char interval[24];
		snprintf(interval, sizeof interval, "%d ms", system->interval);
		cui_rect footer = {pad+padding, pad+6+rows*row, inner, row};
		label(&f, 12, footer, "Update interval", muted, size-2, regular, CUI_ALIGN_START);
		label(&f, 13, footer, interval, accent, size-2, mono, CUI_ALIGN_END);
	}
	if (render(host, &tip->surface)) {
		shape(host, &tip->surface, tip->win, w, h, &body, 1, c->panelradius, pad, 0, 0, h);
		if (!tip->mapped)
			XMapRaised(dpy, tip->win);
		blit(host, &tip->surface, tip->win);
		tip->mapped = 1;
		tip->dirty = 0;
	}
}

int
shell_panel_control(enum ShellAction action)
{
	return action >= ActBattery && action <= ActBrightness ? (int)action - ActBattery : -1;
}

void
shell_panel_pointer(ShellPanel *p, cui_event_type type, int x, int y)
{
	if (p->surface.ui)
		cui_queue_event(p->surface.ui, (cui_event){.type = type, .x = x, .y = y});
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

enum { ClockHeader = 54, ClockZone = 40, ClockFooter = 44, ClockEditor = 130, ClockCancel = 47 };

static int
clock_height(const Config *config, int limit, int open, int active, int *zone_rows)
{
	int base = ClockHeader + ClockFooter + (open ? (active ? ClockCancel : ClockEditor) : 0);
	*zone_rows = MAX(0, MIN(config->nzones+1, (limit-base)/ClockZone));
	return base + *zone_rows*ClockZone;
}

void
shell_clock_resize(const ShellHost *host, const Config *config, ShellPanel *panel, int open, int active)
{
	int rows;
	int height = clock_height(config, panel->clock_limit, open, active, &rows);
	if (height > panel->clock_limit)
		return;
	panel->timer_open = open;
	panel->zone_rows = rows;
	panel->height = height;
	panel->preset_area = (cui_rect){0};
	panel->window_y = panel->clock_anchor - (config->topbar ? 0 : height + 2*panel->pad);
	shell_popup_entrance(config, panel->opened, shell_seconds(), config->topbar ? 1 : -1, &panel->offset_y);
	XMoveResizeWindow(host->dpy, panel->win, panel->window_x, panel->window_y + panel->offset_y,
	                  panel->width+2*panel->pad, height+2*panel->pad);
}

int
shell_panel_open(const ShellHost *host, const Config *config, ShellPanel *panel,
                 enum ShellAction kind, int monitor, int x, int y, int w, int h,
                 int by, int bh, float anchor, Time event_time)
{
	int control = shell_panel_control(kind);
	if (w < 210 || h < (control >= 0 ? 100 : 160))
		return 0;
	shell_panel_close(host, panel);
	int pad = shell_popup_padding(host, config);
	int width = MIN(control >= 0 ? config->controls_width : kind == ActPalette ? config->palette_width
	                : kind == ActClock ? config->clock_width : config->calendar_width, w - 12 - 2*pad);
	const int separation = 7;
	int bar_inset = shell_bar_inset(config);
	int py = kind == ActPalette ? y + (h - MIN(420, h-16))/4
	         : config->topbar ? by + bh - bar_inset + separation - pad
	                          : by + bar_inset - separation + pad;
	int available = kind == ActPalette ? y + h - py - 8
	                : config->topbar ? y + h - py - 6 : py - y - 6;
	int zone_rows = 0, clock_anchor = py, clock_limit = available - 2*pad;
	int height = control >= 0 ? (control == ControlBattery ? 134 : 113)
	             : kind == ActCalendar ? 364 : kind == ActPalette ? 420
	             : clock_height(config, clock_limit, 0, 0, &zone_rows);
	height = MIN(height, available - 2*pad);
	if (height < (control >= 0 ? 100 : kind == ActClock ? ClockHeader+ClockFooter+ClockZone
	              : kind == ActCalendar ? 240 : 160))
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
	                      .zone_rows = zone_rows, .clock_limit = clock_limit, .clock_anchor = clock_anchor,
	                      .window_x = px, .window_y = py,
	                      .hover_x = -1, .hover_y = -1, .control_request = -1,
	                      .opened = shell_seconds()};
	XSetWindowAttributes attrs = {
		.override_redirect = True, .background_pixel = 0, .border_pixel = 0, .colormap = host->cmap,
		.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask,
	};
	Display *dpy = host->dpy;
	shell_popup_entrance(config, panel->opened, panel->opened, kind == ActPalette || config->topbar ? 1 : -1,
	                     &panel->offset_y);
	panel->win = XCreateWindow(dpy, host->root, px, py + panel->offset_y, width+2*pad, height+2*pad,
	                           0, host->depth, InputOutput, host->visual,
	                           CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask,
	                           &attrs);
	XClassHint klass = {"mwm-panel", "mwm"};
	XSetClassHint(dpy, panel->win, &klass);
	XStoreName(dpy, panel->win, control >= 0 ? control_name[control] : kind == ActCalendar ? "mwm calendar"
	           : kind == ActClock ? "mwm clock" : "mwm command palette");
	XMapRaised(dpy, panel->win);
	int pointer = XGrabPointer(dpy, panel->win, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
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
text_link(Frame *f, int id, cui_rect r, const char *text, float size, cui_color fg,
          enum ShellAction action, time_t value, int mono_text)
{
	if (inside(r, f->hover_x, f->hover_y))
		box(f, id, r, f->config->panelradius, alpha(fg, 25), transparent, 0, 0);
	label(f, id+1, r, text, fg, size, mono_text ? mono : medium, CUI_ALIGN_CENTER);
	hit(f, r, action, value);
}

static void
calendar_grid(Frame *f, const ShellPanel *p, cui_color fg, cui_color accent, cui_color ink, cui_color card)
{
	int months = p->calendar_view == CalendarMonths;
	int first = months ? 0 : p->browse_year;
	float x = p->pad+17, y = p->pad+69, width = p->width-34;
	float row = (p->height-127)/4.0f, col = width/3;
	for (int i = 0; i < 12 && (months || first+i < 10000); ++i) {
		int value = first+i;
		int current = months ? p->browse_year == p->year && value == p->month : value == p->year;
		char text[32];
		if (months) {
			struct tm month = {.tm_mon = value};
			strftime(text, sizeof text, "%b", &month);
		} else {
			snprintf(text, sizeof text, "%d", value);
		}
		cui_rect r = {x+i%3*col, y+i/3*row, col-5, row-5};
		int id = 200+i*3;
		box(f, id, r, f->config->panelradius, current ? accent : card, transparent, 0, 0);
		text_link(f, id+1, r, text, f->size, current ? ink : fg,
		          months ? ActCalendarMonth : ActCalendarYear, value, !months);
	}
}

static void
calendar(Frame *f, const ShellPanel *p, const struct tm *local, cui_color fg,
         cui_color muted, cui_color accent, cui_color ink, cui_color card)
{
	struct tm first = {.tm_year = p->year-1900, .tm_mon = p->month, .tm_mday = 1, .tm_hour = 12, .tm_isdst = -1};
	mktime(&first);
	float x = p->pad, y = p->pad, w = p->width, h = p->height;
	float header_room = MAX(55, w-124);
	char year[24];
	if (p->calendar_view == CalendarYears) {
		snprintf(year, sizeof year, "%d – %d", p->browse_year, MIN(9999, p->browse_year+CalendarYearPage-1));
		text_link(f, 110, (cui_rect){x+15, y+12, MIN(137, header_room), 36}, year, f->size, accent,
		          ActYearGrid, 0, 1);
	} else if (p->calendar_view == CalendarMonths) {
		snprintf(year, sizeof year, "%d", p->browse_year);
		text_link(f, 110, (cui_rect){x+15, y+12, MIN(92, header_room), 36}, year, f->size+3, accent,
		          ActYearGrid, 0, 1);
	} else {
		char month[64];
		strftime(month, sizeof month, "%B", &first);
		float vacancy = MAX(27, header_room-55);
		if (textwidth(f, month, f->size+2, medium) + 10 > vacancy)
			strftime(month, sizeof month, "%b", &first);
		float month_width = MIN(vacancy, textwidth(f, month, f->size+2, medium)+14);
		snprintf(year, sizeof year, "%d", p->year);
		text_link(f, 110, (cui_rect){x+13, y+12, month_width, 36}, month, f->size+2, fg, ActMonthGrid, 0, 0);
		text_link(f, 113, (cui_rect){x+13+month_width, y+12, 55, 36}, year, f->size, accent, ActYearGrid, 0, 1);
	}
	button(f, 120, (cui_rect){x+w-100, y+12, 38, 36}, "", ActPast, card, fg, 18);
	button(f, 130, (cui_rect){x+w-55, y+12, 38, 36}, "", ActFuture, card, fg, 18);
	box(f, 140, (cui_rect){x+17, y+56, w-34, 1}, 0, card, transparent, 0, 0);
	button(f, 310, (cui_rect){x+17, y+h-44, 80, 32}, "Today", ActToday, card, fg, 0);
	char today[80];
	float date_size = MAX(9, f->size-1);
	cui_rect date = {x+108, y+h-44, w-125, 32};
	strftime(today, sizeof today, "%A, %d %B", local);
	if (textwidth(f, today, date_size, mono) > date.w)
		strftime(today, sizeof today, "%a, %d %b", local);
	label(f, 320, date, today, muted, date_size, mono, CUI_ALIGN_END);
	if (p->calendar_view != CalendarDays) {
		calendar_grid(f, p, fg, accent, ink, card);
		return;
	}
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
		else if (inside(r, f->hover_x, f->hover_y))
			box(f, 200+i, r, f->config->panelradius, card, transparent, 0, 0);
		char text[8];
		snprintf(text, sizeof text, "%d", day);
		label(f, 250+i, r, text, today ? ink : i%7 > 4 ? muted : fg,
		      f->size, mono, CUI_ALIGN_CENTER);
		hit(f, r, ActCalendarDay, p->year*10000 + (p->month+1)*100 + day);
	}
}

static void
timer_presets(Frame *f, ShellPanel *p, cui_rect row, time_t now, const struct tm *local,
              cui_color accent)
{
	int count = p->at ? f->config->at_presets : f->config->nin_presets;
	p->preset_area = row;
	p->preset_more = 0;
	if (!count)
		return;
	const float side = 16, spacing = 7;
	float size = MAX(9, f->size-2);
	float left = row.x+side;
	float right = row.x+row.w-side;
	time_t candidate = now - local->tm_sec + 60;
	int last_hour = -1;
	for (int i = 0; i < count; ++i) {
		time_t value;
		char caption[16];
		if (p->at) {
			struct tm hour;
			do {
				if (difftime(candidate, now) > 26*3600 || !localtime_r(&candidate, &hour))
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
		if (i < p->preset_start)
			continue;
		float width = textwidth(f, caption, size, mono) + spacing;
		if (left + width > right) {
			p->preset_more = 1;
			break;
		}
		text_link(f, 400+i*2, (cui_rect){left, row.y, width, row.h}, caption, size, accent,
		          p->at ? ActTimerAtPreset : ActTimerInPreset, value, 1);
		left += width;
	}
	if (p->preset_start)
		button(f, 454, (cui_rect){row.x, row.y, side, row.h}, "", ActPresetPrevious, transparent, accent, 12);
	if (p->preset_more)
		button(f, 458, (cui_rect){right, row.y, side, row.h}, "", ActPresetNext, transparent, accent, 12);
}

static void
world_clock(Frame *f, ShellPanel *p, const ShellTimer *timer, const struct tm *local,
            time_t now, double tick, cui_color fg, cui_color muted, cui_color accent,
            cui_color ink, cui_color card, cui_color outline)
{
	float x = p->pad, y = p->pad, w = p->width;
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
		float yy = y+ClockHeader+row*ClockZone;
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
		if (!i)
			hit(f, (cui_rect){x+17, yy, w-34, 36}, ActCopyTime, 0);
	}
	float fold = y + ClockHeader + p->zone_rows*ClockZone;
	cui_rect disclosure = {x+17, fold+5, w-34, 32};
	box(f, 300, (cui_rect){x+17, fold, w-34, 1}, 0, outline, transparent, 0, 0);
	if (inside(disclosure, f->hover_x, f->hover_y))
		box(f, 301, disclosure, radius, card, transparent, 0, 0);
	label(f, 302, (cui_rect){x+24, fold+5, 82, 32}, "Timer", fg, f->size, medium, CUI_ALIGN_START);
	glyph(f, 303, x+w-40, fold+13, 15, p->timer_open ? ActTimerFoldOpen : ActTimerToggle, accent);
	hit(f, disclosure, ActTimerToggle, 0);
	if (timer->active) {
		int seconds = MAX(0, ceil(shell_timer_remaining(timer, now, tick)));
		char remaining[32];
		snprintf(remaining, sizeof remaining, "%02d:%02d:%02d", seconds/3600, seconds/60%60, seconds%60);
		label(f, 304, (cui_rect){x+103, fold+5, w-151, 32}, remaining, accent,
		      f->size, mono, CUI_ALIGN_END);
	}
	p->preset_area = (cui_rect){0};
	p->preset_more = 0;
	if (!p->timer_open)
		return;
	if (timer->active) {
		label(f, 309, (cui_rect){x+24, fold+43, w-145, 33}, "Alert scheduled", muted, small, regular, CUI_ALIGN_START);
		button(f, 320, (cui_rect){x+w-103, fold+43, 86, 33}, "Cancel", ActTimerCancel, card, fg, 0);
		return;
	}
	float bw = (w-40)/2;
	button(f, 330, (cui_rect){x+17, fold+43, bw, 29}, "Alert me in", ActIn,
	       p->at ? card : accent, p->at ? fg : ink, 0);
	button(f, 340, (cui_rect){x+23+bw, fold+43, bw, 29}, "Alert me at", ActAt,
	       p->at ? accent : card, p->at ? ink : fg, 0);
	cui_rect input = {x+17, fold+80, w-115, 35};
	box(f, 350, input, radius, card, accent, 1, 0);
	label(f, 351, (cui_rect){input.x+12, input.y, input.w-21, input.h},
	      p->input[0] ? p->input : p->at ? "HH:MM" : "Minutes / HH:MM:SS",
	      p->input[0] ? fg : muted, small, mono, CUI_ALIGN_START);
	char prefix[sizeof p->input];
	snprintf(prefix, sizeof prefix, "%.*s", p->cursor, p->input);
	float caret = prefix[0] ? textwidth(f, prefix, small, mono) : 0;
	if (caret < input.w-21)
		box(f, 352, (cui_rect){input.x+10+caret, input.y+9, 1, 17}, 0, accent, transparent, 0, 0);
	hit(f, input, ActTimerInput, 0);
	button(f, 360, (cui_rect){x+w-90, input.y, 73, input.h}, "Set", ActTimerSet, accent, ink, 0);
	timer_presets(f, p, (cui_rect){x+17, fold+121, w-34, 23}, now, local, accent);
	if (p->message[0])
		label(f, 370, (cui_rect){x+17, fold+147, w-34, 20}, p->message,
		      f->config->palette[SchemeUrg][ColBg], small, regular, CUI_ALIGN_START);
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

static void
request_control(void *user, const cui_component_slider_request *request)
{
	ShellPanel *panel = user;
	if (request->phase == CUI_SLIDER_COMMIT)
		panel->control_request = (int)round(request->requested);
}

static void
control_panel(Frame *f, ShellPanel *p, const Controls *controls, enum Control kind,
              cui_color fg, cui_color bg, cui_color muted, cui_color accent)
{
	const ControlReading *data = &controls->reading[kind];
	int battery = kind == ControlBattery;
	int value = controls_value(controls, kind);
	int floor = battery && data->limit < 0 ? 0 : data->minimum;
	int adjustable = data->available && data->adjustable && floor < 100;
	float x = p->pad, y = p->pad, w = p->width;
	float track_y = y + MIN(52, p->height-53);
	cui_rect track = {x+17, track_y, w-34, 28};
	int bounded = MAX(floor, MIN(100, value));
	cui_theme theme = cui_theme_dark();
	theme.radius = 4;
	cui_begin_absolute_box(f->ui, 113, (cui_box){.width = track.w, .height = track.h}, track);
	cui_slider_events(f->ui, 114, &theme, cui_string(battery ? "Charging limit" : control_name[kind]),
	                  bounded, &p->slider,
	                  (cui_slider_options){.minimum = floor, .maximum = 100, .step = 1,
	                                       .disabled = !adjustable},
	                  (cui_slider_style_options){.width = track.w, .track_height = 7, .hit_height = track.h,
	                                             .hide_label = true, .custom_colors = true,
	                                             .track_color = alpha(fg, 36), .fill_color = accent},
	                  (cui_component_slider_handler){request_control, p});
	cui_end_box(f->ui);
	int shown = p->slider.dragging ? (int)p->slider.preview : value;
	if (adjustable) {
		float xx = track.x + track.w * (MAX(floor, shown)-floor) / (100-floor);
		box(f, 115, (cui_rect){xx-5, track.y+9, 10, 10}, 5, fg, bg, 1, 0);
	}
	int level = battery ? data->level : shown;
	level_icon(f, 130, (cui_rect){x+15, y+16, 22, 22}, kind, data, level, fg, bg, accent);
	label(f, 111, (cui_rect){x+45, y+12, w-105, 30}, control_name[kind], fg, f->size, medium, CUI_ALIGN_START);
	char text[18] = "–";
	if (data->available)
		snprintf(text, sizeof text, "%d%%", level);
	label(f, 112, (cui_rect){x+w-76, y+12, 59, 30}, text,
	      data->available ? control_color(f->config, kind, level, accent) : muted,
	      f->size, mono, CUI_ALIGN_END);
	char status[80] = "";
	if (data->failed)
		snprintf(status, sizeof status, "Could not change %s", battery ? "charging limit" : control_name[kind]);
	else if (!data->available)
		snprintf(status, sizeof status, "%s unavailable", control_name[kind]);
	else if (battery) {
		const char *state = !strcmp(data->detail, "Full") ? "Fully charged" : data->detail;
		snprintf(status, sizeof status, "%s", *state ? state : "Charge level");
		if (data->battery_seconds >= 0) {
			int minutes = (int)ceil(data->battery_seconds / 60.0);
			char estimate[32];
			if (minutes >= 60)
				snprintf(estimate, sizeof estimate, "~%dh %02dm", minutes/60, minutes%60);
			else
				snprintf(estimate, sizeof estimate, "~%dm", MAX(1, minutes));
			snprintf(status + strlen(status), sizeof status - strlen(status), " · %s %s", estimate,
			         data->charging ? "to charge" : "left");
		}
	}
	else if (data->muted)
		snprintf(status, sizeof status, "Muted");
	label(f, 116, (cui_rect){x+17, track.y+track.h, w-34, 22}, status, muted,
	      MAX(11, f->size-4), regular, CUI_ALIGN_START);
	if (battery && data->limit >= 0 && p->height >= 130) {
		char limit[40];
		snprintf(limit, sizeof limit, "Charge limit %d%%", shown);
		label(f, 117, (cui_rect){x+17, track.y+track.h+19, w-34, 21}, limit, muted,
		      MAX(10, f->size-5), regular, CUI_ALIGN_START);
	}
}

int
shell_panel_draw(const ShellHost *host, const ShellFont *font, unsigned revision,
                 const Config *c, ShellPanel *p, const ShellTimer *timer,
                 const Controls *controls, const ShellEntry *entries, int count, time_t now, double tick)
{
	int w = p->width+2*p->pad, h = p->height+2*p->pad;
	if (!p->win || !shell_surface_begin(host, font, revision, &p->surface, p->win, w, h, 0.016))
		return 0;
	Frame f = {p->surface.ui, font, c, p->hits, 0, COUNT(p->hits),
	           p->hover_x, p->hover_y, host->composited, 1, font->size, {0, 0, 0, 0}};
	cui_set_reduced_motion(f.ui, c->reduced_motion != 0);
	cui_color fg = c->palette[SchemeStatus][ColFg], bg = alpha(c->palette[SchemeStatus][ColBg], 255);
	cui_color accent = alpha(c->palette[SchemeSel][ColBg], 255), ink = c->palette[SchemeSel][ColFg];
	cui_color card = blend(bg, fg, 0.10f), muted = blend(bg, fg, 0.64f);
	cui_color outline = blend(bg, fg, 0.19f);
	cui_rect body = {p->pad, p->pad, p->width, p->height};
	shell_popup_background(f.ui, host, c, body, 1);
	struct tm local = {0};
	localtime_r(&now, &local);
	int control = shell_panel_control(p->kind);
	if (control >= 0)
		control_panel(&f, p, controls, control, fg, bg, muted, accent);
	else if (p->kind == ActCalendar)
		calendar(&f, p, &local, fg, muted, accent, ink, card);
	else if (p->kind == ActClock)
		world_clock(&f, p, timer, &local, now, tick, fg, muted, accent, ink, card, outline);
	else
		palette(&f, p, entries, count, fg, muted, accent, ink, card, outline);
	if (shell_surface_draw(host, &p->surface, p->win, &body, c->panelradius, p->pad))
		p->nhits = f.nhits;
	return 1;
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

static int
has_user_bus(void)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || runtime[0] != '/')
		return 0;
	char socket[PATH_MAX];
	struct stat info;
	int length = snprintf(socket, sizeof socket, "%s/bus", runtime);
	return length > 0 && (size_t)length < sizeof socket && stat(socket, &info) == 0 &&
	       S_ISSOCK(info.st_mode) && info.st_uid == geteuid();
}

void
shell_timer_notify(const Config *config, int xfd)
{
	const char *title = "Timer complete", *body = "Your mwm timer has finished.";
	pid_t child = fork();
	if (child < 0) {
		perror("mwm: timer");
		return;
	}
	if (child)
		return;
	close(xfd);
	if (config->hook[0]) {
		execlp(config->hook, config->hook, title, body, (char *)NULL);
		perror("mwm: timer hook");
	}

	char *const args[] = {"notify-send", "--app-name=mwm", "--icon=alarm-symbolic",
	                      (char *)title, (char *)body, NULL};
	if (getenv("DBUS_SESSION_BUS_ADDRESS") && has_user_bus()) {
		/* A private X session bus may not have the desktop notification server.
		 * Let notify-send discover the user bus if the session send fails. */
		extern char **environ;
		signal(SIGCHLD, SIG_DFL);
		if (posix_spawnp(&child, args[0], NULL, NULL, args, environ) == 0) {
			int status;
			pid_t done;
			do {
				done = waitpid(child, &status, 0);
			} while (done < 0 && errno == EINTR);
			if (done == child && WIFEXITED(status) && WEXITSTATUS(status) == 0)
				_exit(0);
		}
		unsetenv("DBUS_SESSION_BUS_ADDRESS");
	}
	execvp(args[0], args);
	perror("mwm: notify-send");
	_exit(127);
}
