#include "tray_window.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const cui_text_style regular;
static const cui_text_style medium = {.weight = CUI_TEXT_WEIGHT_MEDIUM};
static const cui_text_style mono = {.monospace = true};
static const cui_color transparent;
static const char *glyphs[GlyphCount] = {
	[GlyphWifi0] = "<path d='M12 19h.01'/><path opacity='.28' d='M8.5 15.5a5 5 0 0 1 7 0M5.5 12.5a9 9 0 0 1 13 0M2.5 9.5a13.4 13.4 0 0 1 19 0'/>",
	[GlyphWifi1] = "<path d='M12 19h.01M8.5 15.5a5 5 0 0 1 7 0'/><path opacity='.28' d='M5.5 12.5a9 9 0 0 1 13 0M2.5 9.5a13.4 13.4 0 0 1 19 0'/>",
	[GlyphWifi2] = "<path d='M12 19h.01M8.5 15.5a5 5 0 0 1 7 0M5.5 12.5a9 9 0 0 1 13 0'/><path opacity='.28' d='M2.5 9.5a13.4 13.4 0 0 1 19 0'/>",
	[GlyphWifi3] = "<path d='M12 19h.01M8.5 15.5a5 5 0 0 1 7 0M5.5 12.5a9 9 0 0 1 13 0M2.5 9.5a13.4 13.4 0 0 1 19 0'/>",
	[GlyphWifi4] = "<path stroke-width='2.2' d='M12 19h.01M8.5 15.5a5 5 0 0 1 7 0M5.5 12.5a9 9 0 0 1 13 0M2.5 9.5a13.4 13.4 0 0 1 19 0'/>",
	[GlyphWifiOff] = "<path d='m3 3 18 18M12 19h.01M8.5 15.5a5 5 0 0 1 5.9-.8M5.5 12.5a9 9 0 0 1 4.1-2.4m6.8.6a9 9 0 0 1 2.1 1.8M2.5 9.5a13.1 13.1 0 0 1 4-2.8m4.5-1.1a13.4 13.4 0 0 1 10.5 3.9'/>",
	[GlyphBluetooth] = "<path d='m6 7 12 10-5.5 4.5v-19L18 7 6 17'/>",
	[GlyphBluetoothOff] = "<path d='m3 3 18 18M12.5 12v9l3.6-3M12.5 6.5v-4L18 7l-1.9 1.6M6 17l3-2.5'/>",
	[GlyphDevice] = "<rect x='6.5' y='3' width='11' height='18' rx='2.5'/><path d='M11.5 17.5h1'/>",
	[GlyphVpn] = "<path d='M12 3 4.5 6v5.5c0 4.2 3.1 7.5 7.5 9.5 4.4-2 7.5-5.3 7.5-9.5V6L12 3zM9 12l2 2 4-4'/>",
	[GlyphLock] = "<rect x='6' y='10' width='12' height='10' rx='2'/><path d='M8.5 10V7a3.5 3.5 0 0 1 7 0v3M12 14v2'/>",
	[GlyphRefresh] = "<path d='M20 5v5h-5M4 19v-5h5M19.7 10A8 8 0 0 0 6.5 6M4.3 14a8 8 0 0 0 13.2 4'/>",
	[GlyphForget] = "<path d='M4 7h16m-14 0 1 13h10l1-13M9 7V4h6v3m-5 4v5m4-5v5'/>",
	[GlyphHeadphones] = "<path d='M4 15v-3a8 8 0 0 1 16 0v3'/><rect x='4' y='13' width='4.5' height='7' rx='1.4'/><rect x='15.5' y='13' width='4.5' height='7' rx='1.4'/>",
	[GlyphKeyboard] = "<rect x='2.5' y='6' width='19' height='12' rx='2'/><path d='M6 10h.01M10 10h.01M14 10h.01M18 10h.01M6 14h.01M10 14h4m4 0h.01'/>",
	[GlyphMouse] = "<rect x='6' y='2.5' width='12' height='19' rx='6'/><path d='M12 6v4'/>",
	[GlyphComputer] = "<rect x='5' y='4' width='14' height='12' rx='1.5'/><path d='m5 16-2.5 4h19L19 16m-9 1.5h4'/>",
	[GlyphGamepad] = "<path d='M8 7h8a4 4 0 0 1 3.9 3l1.1 4.5a2.4 2.4 0 0 1-4 2.4L14.7 15H9.3L7 17a2.4 2.4 0 0 1-4-2.4l1.1-4.5A4 4 0 0 1 8 7Z'/><path d='M8 10v4m-2-2h4m5-1h.01m3 2h.01'/>",
	[GlyphBell] = "<path d='M10 20a2 2 0 0 0 4 0M18 9a6 6 0 0 0-12 0c0 4.5-1 6-2 7.5h16C19 15 18 13.5 18 9Z'/>",
	[GlyphBellOff] = "<path d='m3 3 18 18m-11-1a2 2 0 0 0 4 0M6.4 7A6 6 0 0 0 6 9c0 4.5-1 6-2 7.5h12.5M10.2 3.3A6 6 0 0 1 18 9c0 2 .2 3.5.6 4.7'/>",
	[GlyphClose] = "<path d='m6 6 12 12M18 6 6 18'/>",
	/* Undocked's display and arrangement artwork. See LICENSE.heroicons. */
	[GlyphMonitor] = "<path d='M9 17.25v1.007a3 3 0 0 1-.879 2.122L7.5 21h9l-.621-.621A3 3 0 0 1 15 18.257V17.25m6-12V15a2.25 2.25 0 0 1-2.25 2.25H5.25A2.25 2.25 0 0 1 3 15V5.25m18 0A2.25 2.25 0 0 0 18.75 3H5.25A2.25 2.25 0 0 0 3 5.25m18 0V12a2.25 2.25 0 0 1-2.25 2.25H5.25A2.25 2.25 0 0 1 3 12V5.25'/>",
	[GlyphDisplayRow] = "<rect x='3' y='4.5' width='18' height='15' rx='1.125'/><path d='M9 4.5v15m6-15v15'/>",
	[GlyphDisplayColumn] = "<rect x='4.5' y='3' width='15' height='18' rx='1.125'/><path d='M4.5 9h15m-15 6h15'/>",
	[GlyphDisplayTwoAbove] = "<rect x='2.5' y='3.5' width='8' height='6.5' rx='1.1'/><rect x='13.5' y='3.5' width='8' height='6.5' rx='1.1'/><rect x='8' y='13.5' width='8' height='6.5' rx='1.1'/>",
};

static void
glyph(TrayWindow *win, cui_context *ui, int id, int x, int y, int size, int which, cui_color color)
{
	cui_icon_state *state = &win->glyphs[which];
	if (!state->internal) {
		char svg[640];
		snprintf(svg, sizeof svg, "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
		         "<g fill='none' stroke='#fff' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>"
		         "%s</g></svg>", glyphs[which]);
		cui_icon_set_data(state, cui_string(svg));
	}
	cui_begin_absolute_box(ui, id, (cui_box){.width = size, .height = size}, (cui_rect){x, y, size, size});
	cui_theme theme = cui_theme_dark();
	cui_icon(ui, id + 2000, &theme, state,
	         (cui_icon_options){.width = size, .height = size, .custom_width = true, .custom_height = true,
	                            .color = color, .custom_color = true, .decorative = true});
	cui_end_box(ui);
}

void
tray_ui_glyphs_free(TrayWindow *win)
{
	for (int i = 0; i < GlyphCount; ++i)
		cui_icon_state_destroy(&win->glyphs[i]);
}

static int
signal_icon(int strength)
{
	return GlyphWifi0 + (strength > 0) + (strength >= 33) + (strength >= 57) + (strength >= 80);
}

static int
main_icon(const Tray *tray)
{
	if (tray->kind == TrayBluetooth)
		return tray->enabled ? GlyphBluetooth : GlyphBluetoothOff;
	if (tray->kind == TrayNotifications)
		return tray->enabled ? GlyphBellOff : GlyphBell;
	if (tray->kind == TrayDisplays)
		return GlyphMonitor;
	return tray->enabled ? signal_icon(tray->strength) : GlyphWifiOff;
}

static void
box(cui_context *ui, int id, cui_rect r, int radius, cui_color fill, cui_color edge, int stroke)
{
	if (r.w < 1 || r.h < 1)
		return;
	cui_begin_absolute_box(ui, id, (cui_box){.width = r.w, .height = r.h, .radius = radius,
	                      .background = fill, .border_width = stroke, .border = edge}, r);
	cui_end_box(ui);
}

void
tray_ui_icon(Tray *tray)
{
	TrayWindow *win = tray->window;
	ShellHost *h = &win->host;
	int w = win->icon_w, height = win->icon_h;
	if (!win->icon || !shell_surface_begin(h, &win->font, win->revision, &win->icon_surface, win->icon, w, height, 0))
		return;
	cui_context *ui = win->icon_surface.ui;
	cui_color fg = tray->config.palette[SchemeStatus][ColFg], bg = tray->config.palette[SchemeStatus][ColBg];
	cui_color icon = tray->enabled ? win->accent : cui_color_mix_oklab(bg, fg, 0.63f);
	if (tray->kind == TrayNotifications)
		icon = tray->available && !tray->enabled ? win->accent : cui_color_mix_oklab(bg, fg, 0.63f);
	if (tray->answer)
		icon = tray->config.usage_palette[UsageWarning];
	if (!h->composited || h->depth != 32) {
		bg.a = 255;
		box(ui, 10, (cui_rect){0, 0, w, height}, 0, bg, transparent, 0);
	}
	if (win->icon_hover || win->panel) {
		cui_color hover = win->accent;
		hover.a = 27;
		box(ui, 11, (cui_rect){0, 0, w, height}, tray->config.tagradius, hover, transparent, 0);
	}
	int size = MIN(22, MIN(w, height) - 2);
	if (size > 0)
		glyph(win, ui, 30, (w-size)/2, (height-size)/2, size, main_icon(tray), icon);
	if (tray->kind == TrayNotifications && tray->unread && size > 7)
		box(ui, 31, (cui_rect){(w+size)/2-5, (height-size)/2, 5, 5}, 3, win->accent, bg, 1);
	shell_surface_draw(h, &win->icon_surface, win->icon, NULL, 0, 0);
}

static gboolean
in_tab(const TrayWindow *win, const TrayEntry *row)
{
	return (row->icon == TrayVpn) == (win->tab == 1);
}

int
tray_row_count(Tray *tray)
{
	int count = 0;
	for (guint i = 0; i < tray->rows->len; ++i)
		count += in_tab(tray->window, tray->rows->pdata[i]);
	return count;
}

TrayEntry *
tray_row(Tray *tray, int index)
{
	for (guint i = 0; index >= 0 && i < tray->rows->len; ++i) {
		TrayEntry *row = tray->rows->pdata[i];
		if (in_tab(tray->window, row) && index-- == 0)
			return row;
	}
	return NULL;
}

TrayEntry *
tray_expanded(Tray *tray)
{
	TrayWindow *win = tray->window;
	if (!win->expanded)
		return NULL;
	for (guint i = 0; i < tray->rows->len; ++i) {
		TrayEntry *row = tray->rows->pdata[i];
		if (!strcmp(win->expanded, row->id))
			return row;
	}
	g_clear_pointer(&win->expanded, g_free);
	return NULL;
}

enum { Header = 85, Footer = 35, RowSpacing = 44,
       NotifyHeader = 74, NotifyToolbar = 34, NotifyRow = 74, NotifyFooter = 28,
       DisplayHeader = 66, DisplayToolbar = 40, DisplayFooter = 65 };

static gboolean
display_toolbar(const Tray *tray)
{
	return tray->display_remaining ||
	       ((tray->config.display_mirror && tray->display_arrangeable > 1) ||
	        (tray->config.display_identify && tray->display_count));
}

int
tray_ui_height(Tray *tray, int available)
{
	TrayWindow *win = tray->window;
	if (tray->answer) {
		win->visible_rows = 0;
		int chrome = 120 + (tray->message && *tray->message ? 26 : 0);
		win->fields_visible = MIN(tray->prompt.count, MAX(1, (available-chrome)/56));
		win->field_scroll = CLAMP(win->field_scroll, 0, MAX(0, tray->prompt.count-win->fields_visible));
		return MIN(available, chrome + 56*win->fields_visible);
	}
	if (tray->kind == TrayNotifications) {
		int chrome = NotifyHeader+NotifyToolbar+NotifyFooter;
		int rows = MAX(0, (available-chrome)/NotifyRow);
		win->visible_rows = MIN(MIN(rows, tray->config.applet_rows), (int)tray->rows->len);
		int content = tray_expanded(tray) ? CLAMP((int)ceilf(win->detail_bounds.h)+4, NotifyRow, 4*NotifyRow)
		                                  : MAX(1, win->visible_rows)*NotifyRow;
		return MIN(available, chrome+content);
	}
	if (tray->kind == TrayDisplays) {
		int chrome = DisplayHeader + DisplayFooter + (display_toolbar(tray) ? DisplayToolbar : 0);
		int content = tray->display_remaining || !tray->available ? 0 : MAX(1, tray_row_count(tray));
		win->visible_rows = MIN(MIN(MAX(0, (available-chrome)/RowSpacing), tray->config.applet_rows), content);
		return MIN(available, chrome + win->visible_rows * RowSpacing);
	}
	int toolbar = tray->kind == TrayWifi ? 75 : 41;
	int rows = MAX(1, (available-Header-Footer-toolbar)/RowSpacing);
	win->visible_rows = MIN(MIN(rows, tray->config.applet_rows), MAX(1, tray_row_count(tray)));
	return MIN(available, Header + toolbar + win->visible_rows*RowSpacing + Footer);
}

typedef struct {
	TrayWindow *win;
	cui_context *ui;
	cui_color fg, bg, accent, muted, line, card, ink, danger, success;
	float size;
	int radius;
} Drawing;

static void
text_at(Drawing *d, int id, cui_rect rect, const char *value, cui_color color, float size,
        cui_text_style style, cui_align align)
{
	if (value)
		shell_text(d->ui, &d->win->font, id, rect, value, color, size, style, align);
}

static gboolean
inside(cui_rect r, int x, int y)
{
	return x >= r.x && x < r.x+r.w && y >= r.y && y < r.y+r.h;
}

static void
hit(Drawing *d, cui_rect rect, enum TrayHitKind kind, int value, const char *row)
{
	TrayWindow *w = d->win;
	if (w->count < (int)G_N_ELEMENTS(w->hits))
		w->hits[w->count++] = (TrayHit){rect, kind, value, g_strdup(row)};
}

static void
button(Drawing *d, int id, cui_rect rect, const char *label, enum TrayHitKind kind, int value,
       gboolean primary, gboolean enabled)
{
	cui_color fill = primary ? d->accent : d->card;
	if (enabled && inside(rect, d->win->hover_x, d->win->hover_y))
		fill = cui_color_mix_oklab(fill, d->fg, 0.16f);
	box(d->ui, id, rect, d->radius, fill, primary ? transparent : d->line, primary ? 0 : 1);
	text_at(d, id+1, (cui_rect){rect.x+5, rect.y, rect.w-10, rect.h}, label,
	        !enabled ? d->muted : primary ? d->ink : d->fg, d->size-3, medium, CUI_ALIGN_CENTER);
	if (enabled)
		hit(d, rect, kind, value, NULL);
}

static void
list_row(Drawing *d, TrayEntry *entry, int id, int index, cui_rect r, gboolean enabled)
{
	gboolean hover = inside(r, d->win->hover_x, d->win->hover_y) || index == d->win->selected;
	cui_color fill = d->card;
	if (hover || entry->active)
		fill = cui_color_mix_oklab(d->bg, d->accent, hover ? 0.21f : 0.12f);
	box(d->ui, id, r, d->radius, fill, entry->active ? d->line : transparent, entry->active);
	static const int entry_icons[] = {
		[TrayDevice] = GlyphDevice, [TrayVpn] = GlyphVpn, [TrayHeadphones] = GlyphHeadphones,
		[TrayKeyboard] = GlyphKeyboard, [TrayMouse] = GlyphMouse, [TrayComputer] = GlyphComputer,
		[TrayGamepad] = GlyphGamepad, [TrayMonitor] = GlyphMonitor,
		[TrayDisplayRow] = GlyphDisplayRow, [TrayDisplayColumn] = GlyphDisplayColumn,
		[TrayDisplayTwoAbove] = GlyphDisplayTwoAbove,
	};
	int which = entry->icon == TraySignal ? signal_icon(entry->strength) : entry_icons[entry->icon];
	glyph(d->win, d->ui, id+1, r.x+9, r.y+9, 20, which, entry->active || hover ? d->accent : d->muted);
	gboolean can_forget = enabled && entry->saved && entry->icon != TrayVpn;
	int reserve = can_forget ? 34 : 10;
	if (entry->icon == TraySignal && strstr(entry->detail, "Secured")) {
		glyph(d->win, d->ui, id+2, r.x+r.w-reserve-21, r.y+10, 17, GlyphLock, d->muted);
		reserve += 22;
	}
	text_at(d, id+3, (cui_rect){r.x+38, r.y+1, r.w-38-reserve, 21}, entry->label,
	        d->fg, d->size-2, medium, CUI_ALIGN_START);
	text_at(d, id+4, (cui_rect){r.x+38, r.y+21, r.w-38-reserve, 16}, entry->detail,
	        entry->active ? d->success : d->muted, d->size-5, regular, CUI_ALIGN_START);
	if (enabled)
		hit(d, r, HitRow, index, entry->id);
	if (can_forget) {
		cui_rect forget = {r.x+r.w-32, r.y+6, 26, 26};
		if (inside(forget, d->win->hover_x, d->win->hover_y))
			box(d->ui, id+5, forget, d->radius, d->line, transparent, 0);
		glyph(d->win, d->ui, id+6, forget.x+5, forget.y+5, 16, GlyphForget, d->muted);
		hit(d, forget, HitForget, index, entry->id);
	}
}

static void
list_panel(Tray *tray, Drawing *d, int x, int y, int w)
{
	TrayWindow *v = d->win;
	int total = tray_row_count(tray);
	v->scroll = CLAMP(v->scroll, 0, MAX(0, total-v->visible_rows));
	v->selected = MIN(v->selected, total-1);
	box(d->ui, 101, (cui_rect){x, y+13, 40, 40}, d->radius, d->card, d->line, 1);
	glyph(v, d->ui, 102, x+8, y+21, 24, main_icon(tray), d->accent);
	text_at(d, 103, (cui_rect){x+52, y+9, w-138, 25}, v->title, d->fg, d->size+1, medium, CUI_ALIGN_START);
	text_at(d, 104, (cui_rect){x+52, y+34, w-138, 21}, tray->headline,
	        d->muted, d->size-3, regular, CUI_ALIGN_START);
	button(d, 105, (cui_rect){x+w-72, y+17, 72, 32}, tray->enabled ? "On" : "Off", HitRadio, 0,
	       tray->enabled, tray->available && !tray->busy);
	text_at(d, 107, (cui_rect){x, y+59, w, 20}, tray->subtitle, d->muted, d->size-4, regular, CUI_ALIGN_START);
	y += Header;
	box(d->ui, 108, (cui_rect){x, y-3, w, 1}, 0, d->line, transparent, 0);
	if (tray->kind == TrayWifi) {
		button(d, 110, (cui_rect){x, y+3, 72, 28}, "Wi-Fi", HitWifi, 0, !v->tab, TRUE);
		button(d, 112, (cui_rect){x+78, y+3, 68, 28}, "VPN", HitVpn, 0, v->tab == 1, TRUE);
		y += 34;
	}
	const char *label = tray->kind == TrayBluetooth ? "Devices" : v->tab ? "Saved VPNs" : "Nearby networks";
	text_at(d, 120, (cui_rect){x, y+1, w-165, 32}, label, d->fg, d->size-3, medium, CUI_ALIGN_START);
	if (!v->tab) {
		if (tray->kind == TrayWifi && tray->enabled)
			button(d, 123, (cui_rect){x+w-160, y+2, 75, 25}, "Hidden", HitHidden, 0, FALSE, !tray->busy);
		if (tray->kind == TrayBluetooth && tray->enabled)
			button(d, 123, (cui_rect){x+w-184, y+2, 99, 25}, tray->discoverable ? "Visible" : "Make visible",
			       HitVisible, 0, tray->discoverable, !tray->busy);
		const char *scan = tray->scanning ? (tray->kind == TrayWifi ? "Scanning" : "Stop") : "Scan";
		button(d, 126, (cui_rect){x+w-76, y+2, 76, 25}, scan, HitScan, 0, FALSE, tray->can_scan);
	}
	y += 34;
	for (int i = 0; i < v->visible_rows; ++i) {
		int index = v->scroll+i;
		TrayEntry *row = tray_row(tray, index);
		if (!row)
			break;
		list_row(d, row, 200+i*8, index, (cui_rect){x, y+i*RowSpacing, w, 39}, !tray->busy);
	}
	if (!total) {
		const char *empty = !tray->available ? "No adapter available" : v->tab ? "No VPN profiles"
		                    : !tray->enabled ? "Radio is off" : tray->scanning ? "Searching…"
		                    : tray->kind == TrayWifi ? "No networks found" : "Scan to find devices";
		text_at(d, 129, (cui_rect){x, y, w, 38}, empty, d->muted, d->size-3, regular, CUI_ALIGN_CENTER);
	}
	char footer[96] = "";
	const char *status = tray->message && *tray->message ? tray->message : footer;
	if (total > v->visible_rows)
		snprintf(footer, sizeof footer, "%d–%d of %d  ·  Scroll for more", v->scroll+1, v->scroll+v->visible_rows, total);
	text_at(d, 130, (cui_rect){x, v->pad+v->height-Footer, w, Footer-4}, status,
	        tray->failed ? d->danger : d->muted, d->size-4, regular, CUI_ALIGN_START);
}

static void
displays_panel(Tray *tray, Drawing *d, int x, int y, int w)
{
	TrayWindow *v = d->win;
	glyph(v, d->ui, 600, x+8, y+20, 24, GlyphMonitor, d->accent);
	text_at(d, 601, (cui_rect){x+52, y+9, w-52, 25}, v->title, d->fg, d->size+1, medium, CUI_ALIGN_START);
	text_at(d, 602, (cui_rect){x+52, y+34, w-52, 21}, tray->headline, d->muted, d->size-3, regular, CUI_ALIGN_START);
	y += DisplayHeader;
	box(d->ui, 603, (cui_rect){x, y-5, w, 1}, 0, d->line, transparent, 0);

	gboolean ready = tray->available && !tray->busy;
	int half = (w-8)/2;
	if (tray->display_remaining) {
		button(d, 604, (cui_rect){x, y, half, 31}, "Revert", HitDisplayAction, TrayRevert, FALSE, ready);
		button(d, 606, (cui_rect){x+half+8, y, w-half-8, 31}, "Keep layout", HitDisplayAction, TrayKeep, TRUE, ready);
	} else if (display_toolbar(tray)) {
		gboolean mirror = tray->config.display_mirror && tray->display_arrangeable > 1;
		gboolean identify = tray->config.display_identify && tray->display_count;
		if (mirror)
			button(d, 604, (cui_rect){x, y, identify ? half : w, 31},
			       tray->display_mirrored ? "Extend" : "Mirror", HitDisplayAction, TrayMirror, FALSE, ready);
		if (identify)
			button(d, 606, (cui_rect){x+(mirror ? half+8 : 0), y, mirror ? w-half-8 : w, 31},
			       "Identify", HitDisplayAction, TrayIdentify, FALSE, ready);
	}
	if (display_toolbar(tray))
		y += DisplayToolbar;
	int total = tray_row_count(tray);
	v->scroll = CLAMP(v->scroll, 0, MAX(0, total-v->visible_rows));
	if (!tray->display_remaining) {
		for (int i = 0; i < v->visible_rows; ++i) {
			int index = v->scroll+i;
			TrayEntry *row = tray_row(tray, index);
			if (row)
				list_row(d, row, 640+i*8, index, (cui_rect){x, y+i*RowSpacing, w, 39}, ready);
		}
		if (v->visible_rows && !total)
			text_at(d, 608, (cui_rect){x, y, w, 38}, "No presets for these displays",
			        d->muted, d->size-3, regular, CUI_ALIGN_CENTER);
	}
	char status[96] = "";
	if (tray->display_remaining)
		snprintf(status, sizeof status, "Reverts in %d s", tray->display_remaining);
	else if (v->visible_rows && total > v->visible_rows)
		snprintf(status, sizeof status, "%d–%d of %d  ·  Scroll for more", v->scroll+1, v->scroll+v->visible_rows, total);
	const char *message = tray->message && *tray->message ? tray->message : status;
	int footer = v->pad+v->height-DisplayFooter;
	text_at(d, 609, (cui_rect){x, footer, w, 20}, message,
	        tray->failed ? d->danger : d->muted, d->size-4, regular, CUI_ALIGN_START);
	button(d, 610, (cui_rect){x, footer+22, w, 31}, "Open Undocked", HitDisplayAction, TrayOpenEditor, FALSE, !tray->busy);
}

static void
text_button(Drawing *d, int id, cui_rect rect, const char *label, enum TrayHitKind kind)
{
	if (inside(rect, d->win->hover_x, d->win->hover_y))
		box(d->ui, id, rect, d->radius, d->card, transparent, 0);
	text_at(d, id+1, rect, label, d->accent, d->size-4, medium, CUI_ALIGN_CENTER);
	hit(d, rect, kind, 0, NULL);
}

static void
notification_remove(Drawing *d, int id, cui_rect rect, const char *row)
{
	gboolean hover = inside(rect, d->win->hover_x, d->win->hover_y);
	if (hover)
		box(d->ui, id, rect, d->radius, d->line, transparent, 0);
	glyph(d->win, d->ui, id+1, rect.x+5, rect.y+5, 14, GlyphClose, hover ? d->fg : d->muted);
	hit(d, rect, HitForget, 0, row);
}

static void
notification_detail(Drawing *d, TrayEntry *entry, int x, int y, int w)
{
	TrayWindow *v = d->win;
	text_button(d, 430, (cui_rect){x, y+2, 58, 25}, "‹ Back", HitBack);
	notification_remove(d, 433, (cui_rect){x+w-26, y+2, 24, 24}, entry->id);
	y += NotifyToolbar;
	int height = MAX(0, v->pad+v->height-13-y);
	box(d->ui, 435, (cui_rect){x, y, w, height}, d->radius, d->card, d->line, 1);
	if (height < 20)
		return;
	cui_rect clip = {x+12, y+9, w-24, height-18};
	v->detail_viewport = clip.h;
	v->detail_scroll = CLAMP(v->detail_scroll, 0, MAX(0, (int)v->detail_bounds.h-v->detail_viewport));
	cui_begin_absolute_box(d->ui, 436, (cui_box){.width = clip.w, .height = clip.h, .clip = true,
	                       .offset_y = -v->detail_scroll}, clip);
	cui_begin_box(d->ui, 437, (cui_box){.width = clip.w, .gap = 5});
	cui_text_ex(d->ui, cui_string(entry->detail), d->size-5, d->muted, d->size, mono);
	cui_text_ex(d->ui, cui_string(entry->label), d->size, d->fg, d->size+4, medium);
	if (*entry->body)
		cui_text_ex(d->ui, cui_string(entry->body), d->size-3, d->fg, d->size+3, regular);
	cui_end_box(d->ui);
	cui_end_box(d->ui);
	cui_track_bounds(d->ui, 437, &v->detail_bounds);
}

static void
notifications_panel(Tray *tray, Drawing *d, int x, int y, int w)
{
	TrayWindow *v = d->win;
	int total = tray->rows->len;
	v->scroll = CLAMP(v->scroll, 0, MAX(0, total-v->visible_rows));
	v->selected = MIN(v->selected, total-1);
	box(d->ui, 400, (cui_rect){x, y+13, 40, 40}, d->radius, d->card, d->line, 1);
	glyph(v, d->ui, 401, x+8, y+21, 24, main_icon(tray), d->accent);
	text_at(d, 402, (cui_rect){x+51, y+10, w-123, 25}, v->title, d->fg, d->size+1, medium, CUI_ALIGN_START);
	text_at(d, 403, (cui_rect){x+51, y+35, w-123, 20}, tray->headline, d->muted, d->size-4, regular, CUI_ALIGN_START);
	if (tray->available)
		button(d, 404, (cui_rect){x+w-61, y+17, 61, 30}, tray->enabled ? "On" : "Off", HitRadio, 0,
		       tray->enabled, !tray->busy);
	y += NotifyHeader;
	box(d->ui, 406, (cui_rect){x, y-4, w, 1}, 0, d->line, transparent, 0);
	TrayEntry *expanded = tray_expanded(tray);
	if (expanded) {
		notification_detail(d, expanded, x, y, w);
		return;
	}
	text_at(d, 407, (cui_rect){x, y, w-95, 28}, "Recent", d->fg, d->size-3, medium, CUI_ALIGN_START);
	if (total)
		text_button(d, 408, (cui_rect){x+w-77, y+1, 77, 25}, "Clear all", HitClear);
	y += NotifyToolbar;
	for (int i = 0; i < v->visible_rows; ++i) {
		int index = v->scroll+i;
		TrayEntry *row = tray->rows->pdata[index];
		int id = 500+i*8;
		cui_rect r = {x, y+i*NotifyRow, w, NotifyRow-6};
		gboolean hover = inside(r, v->hover_x, v->hover_y) || index == v->selected;
		cui_color fill = hover ? cui_color_mix_oklab(d->bg, d->accent, 0.18f) : d->card;
		box(d->ui, id, r, d->radius, fill, transparent, 0);
		if (row->active)
			box(d->ui, id+1, (cui_rect){r.x+5, r.y+9, 2, r.h-18}, 1, d->danger, transparent, 0);
		text_at(d, id+2, (cui_rect){r.x+12, r.y+4, w-48, 20}, row->label,
		        d->fg, d->size-2, medium, CUI_ALIGN_START);
		g_autofree char *preview = tray_label(row->body);
		text_at(d, id+3, (cui_rect){r.x+12, r.y+25, w-24, 19}, preview,
		        d->fg, d->size-4, regular, CUI_ALIGN_START);
		text_at(d, id+4, (cui_rect){r.x+12, r.y+46, w-24, 16}, row->detail,
		        d->muted, d->size-5, mono, CUI_ALIGN_START);
		hit(d, r, HitRow, index, row->id);
		notification_remove(d, id+5, (cui_rect){r.x+w-30, r.y+4, 24, 24}, row->id);
	}
	if (!total) {
		const char *empty = !tray->available ? tray->subtitle : !tray->history_available ? "Update nod to view recent notifications"
		                    : "No recent notifications";
		text_at(d, 412, (cui_rect){x, y, w, 46}, empty, d->muted, d->size-4, regular, CUI_ALIGN_CENTER);
	}
	char page[64] = "";
	if (total > v->visible_rows && v->visible_rows)
		snprintf(page, sizeof page, "%d–%d of %d", v->scroll+1, v->scroll+v->visible_rows, total);
	const char *status = tray->message && *tray->message ? tray->message : page;
	text_at(d, 413, (cui_rect){x, v->pad+v->height-NotifyFooter, w, NotifyFooter-4}, status,
	        tray->failed ? d->danger : d->muted, d->size-5, regular, CUI_ALIGN_START);
}

static void
prompt_panel(Tray *tray, Drawing *d, int x, int y, int w)
{
	TrayWindow *v = d->win;
	const TrayPrompt *p = &tray->prompt;
	text_at(d, 140, (cui_rect){x, y+9, w, 27}, p->title, d->fg, d->size+1, medium, CUI_ALIGN_START);
	text_at(d, 141, (cui_rect){x, y+37, w, 23}, p->detail, d->muted, d->size-3, regular, CUI_ALIGN_START);
	if (v->fields_visible < p->count) {
		char page[64];
		snprintf(page, sizeof page, "%d–%d of %d fields · Scroll", v->field_scroll+1,
		         v->field_scroll+v->fields_visible, p->count);
		text_at(d, 142, (cui_rect){x, y+57, w, 16}, page, d->muted, d->size-6, mono, CUI_ALIGN_END);
	}
	for (int row = 0; row < v->fields_visible; ++row) {
		int i = v->field_scroll + row;
		const TrayField *f = &p->fields[i];
		int id = 150+i*5;
		cui_rect field = {x, y+86+56*row, w, 32};
		text_at(d, id, (cui_rect){x, field.y-25, w, 24}, f->label, d->fg, d->size-3, medium, CUI_ALIGN_START);
		box(d->ui, id+1, field, d->radius, d->card, i == v->field ? d->accent : d->line, 1);
		char value[TrayFieldSize+4] = {0};
		size_t pos = i == v->field ? (size_t)v->cursor : strlen(f->value);
		if (f->secret) {
			size_t chars = g_utf8_strlen(f->value, -1);
			pos = g_utf8_strlen(f->value, pos);
			memset(value, '*', chars);
		} else {
			g_strlcpy(value, f->value, sizeof value);
		}
		if (i == v->field && !v->select_all) {
			memmove(value+pos+1, value+pos, strlen(value+pos)+1);
			value[pos] = '|';
		}
		if (i == v->field && v->select_all)
			box(d->ui, id+3, (cui_rect){x+5, field.y+4, w-10, field.h-8}, d->radius, d->line, transparent, 0);
		/* Keep the caret visible on narrow panels or with long passphrases. */
		int room = MAX(8, (int)((w-24) / (d->size*0.62f)));
		char *start = value;
		if (pos >= (size_t)room && i == v->field)
			start = g_utf8_offset_to_pointer(value, MAX(0, g_utf8_strlen(value, pos) - room + 1));
		text_at(d, id+2, (cui_rect){x+10, field.y, w-20, field.h}, start, d->fg, d->size-2, mono, CUI_ALIGN_START);
		hit(d, field, HitField, i, NULL);
	}
	int foot = v->pad+v->height-42;
	int accept_width = *p->accept ? 108 : 0;
	if (tray->message && *tray->message)
		text_at(d, 193, (cui_rect){x, foot-24, w, 22}, tray->message,
		        tray->failed ? d->danger : d->muted, d->size-4, regular, CUI_ALIGN_START);
	button(d, 195, (cui_rect){x+w-92-accept_width, foot, 84, 31},
	       *p->accept ? "Cancel" : "Dismiss", HitCancel, 0, FALSE, TRUE);
	if (accept_width)
		button(d, 198, (cui_rect){x+w-100, foot, 100, 31}, p->accept, HitAccept, 0, TRUE, TRUE);
}

void
tray_ui_panel(Tray *tray)
{
	TrayWindow *v = tray->window;
	int w = v->width+2*v->pad, h = v->height+2*v->pad;
	if (!v->panel || !shell_surface_begin(&v->host, &v->font, v->revision, &v->panel_surface, v->panel, w, h, 0))
		return;
	cui_context *ui = v->panel_surface.ui;
	const Config *c = &tray->config;
	cui_color fg = c->palette[SchemeStatus][ColFg], bg = c->palette[SchemeStatus][ColBg];
	bg.a = 255;
	cui_rect body = {v->pad, v->pad, v->width, v->height};
	shell_popup_background(ui, &v->host, c, body, 1);
	Drawing d = {v, ui, fg, bg, v->accent, cui_color_mix_oklab(bg, fg, .68f), cui_color_mix_oklab(bg, fg, .19f),
	             cui_color_mix_oklab(bg, fg, .09f), c->palette[SchemeSel][ColFg], c->usage_palette[UsageDanger],
	             c->tray_success, v->font.size, MAX(2, c->panelradius-1)};
	for (int i = 0; i < v->count; ++i)
		g_free(v->hits[i].row);
	v->count = 0;
	int margin = 15;
	int detail_height = (int)v->detail_bounds.h;
	if (tray->answer)
		prompt_panel(tray, &d, v->pad+margin, v->pad, v->width-2*margin);
	else if (tray->kind == TrayNotifications)
		notifications_panel(tray, &d, v->pad+margin, v->pad, v->width-2*margin);
	else if (tray->kind == TrayDisplays)
		displays_panel(tray, &d, v->pad+margin, v->pad, v->width-2*margin);
	else
		list_panel(tray, &d, v->pad+margin, v->pad, v->width-2*margin);
	shell_surface_draw(&v->host, &v->panel_surface, v->panel, &body, c->panelradius, v->pad);
	if (v->expanded && (int)v->detail_bounds.h != detail_height)
		tray_changed(tray);
}
