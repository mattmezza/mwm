#include "tray_window.h"
#include "tray_wifi.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <glib-unix.h>

#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void close_panel(Tray *, gboolean);
static gboolean window_events(gint, GIOCondition, gpointer);
static void dock(Tray *);

static const struct {
	const char *command, *title, *instance, *icon_class, *panel_title;
} applets[TrayKindCount] = {
	[TrayWifi] = {"wifi", "Wi-Fi", "_MWM_WIFI_APPLET_S", "mwm-wifi-icon", "mwm Wi-Fi"},
	[TrayBluetooth] = {"bluetooth", "Bluetooth", "_MWM_BLUETOOTH_APPLET_S", "mwm-bluetooth-icon", "mwm Bluetooth"},
	[TrayNotifications] = {"notifications", "Notifications", "_MWM_NOTIFICATIONS_APPLET_S",
	                       "mwm-notifications-icon", "mwm Notifications"},
	[TrayDisplays] = {"displays", "Displays", "_MWM_DISPLAYS_APPLET_S", "mwm-displays-icon", "mwm Displays"},
};

static int
xerror(Display *dpy, XErrorEvent *error)
{
	if (error->error_code != BadWindow && error->error_code != BadDrawable && error->error_code != BadMatch) {
		char message[128];
		XGetErrorText(dpy, error->error_code, message, sizeof message);
		fprintf(stderr, "mwm-tray: %s (X11 request %d)\n", message, error->request_code);
	}
	return 0;
}

static void
set_class(Display *dpy, Window win, const char *name, const char *title)
{
	XClassHint hint = {(char *)name, "mwm"};
	XSetClassHint(dpy, win, &hint);
	XStoreName(dpy, win, title);
}

static Atom
screen_atom(TrayWindow *win, const char *name)
{
	char sized[80];
	snprintf(sized, sizeof sized, "%s%d", name, win->screen);
	return XInternAtom(win->host.dpy, sized, False);
}

static void
tray_palette(Tray *tray)
{
	TrayWindow *w = tray->window;
	w->accent = tray->config.palette[SchemeSel][ColBg];
	w->accent.a = 255;
	if (!w->manager)
		return;
	Atom type;
	int format;
	unsigned long count, left;
	unsigned char *data = NULL;
	if (XGetWindowProperty(w->host.dpy, w->manager, w->colors, 0, 12, False, XA_CARDINAL,
	                       &type, &format, &count, &left, &data) == Success &&
	    type == XA_CARDINAL && format == 32 && count == 12 && !left) {
		const unsigned long *color = (const unsigned long *)data;
		w->accent = (cui_color){color[0] >> 8, color[1] >> 8, color[2] >> 8, 255};
	}
	if (data)
		XFree(data);
}

static gboolean
read_config(Tray *tray)
{
	TrayWindow *win = tray->window;
	Config next;
	ShellFont font;
	char error[CONFIG_ERROR];
	config_defaults(&next);
	if (!config_read(&next, win->config_path, win->explicit_config, error) ||
	    !config_theme(&next, win->host.dpy, win->host.root, win->host.cmap, error) ||
	    !shell_font(&font, &next, error)) {
		fprintf(stderr, "mwm-tray: %s\n", error);
		return FALSE;
	}
	shell_popup_exit_reset(&win->host, &win->exit);
	shell_surface_free(&win->host, &win->icon_surface);
	shell_surface_free(&win->host, &win->panel_surface);
	shell_font_free(&win->font);
	gboolean command_changed = strcmp(tray->config.undocked_command, next.undocked_command) != 0;
	tray->config = next;
	win->font = font;
	++win->revision;
	tray_palette(tray);
	if (tray->kind == TrayWifi)
		wifi_configure(tray);
	else if (tray->kind == TrayBluetooth)
		bluetooth_configure(tray);
	else if (tray->kind == TrayDisplays)
		displays_configure(tray, command_changed);
	tray_changed(tray);
	return TRUE;
}

static void
make_icon(Tray *tray)
{
	TrayWindow *win = tray->window;
	ShellHost *h = &win->host;
	XSetWindowAttributes attrs = {
		.override_redirect = True, .background_pixel = 0, .border_pixel = 0, .colormap = h->cmap,
		.event_mask = ExposureMask | ButtonPressMask | EnterWindowMask | LeaveWindowMask | StructureNotifyMask,
		.cursor = win->cursors[TrayCursorHand],
	};
	win->icon_w = win->icon_h = 24;
	win->icon = XCreateWindow(h->dpy, h->root, 0, 0, 24, 24, 0, h->depth, InputOutput, h->visual,
	                          CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask | CWCursor,
	                          &attrs);
	set_class(h->dpy, win->icon, applets[tray->kind].icon_class, win->title);
	unsigned long embed[] = {0, 1}; /* Version 0, XEMBED_MAPPED. */
	XChangeProperty(h->dpy, win->icon, win->embed_info, win->embed_info, 32,
	                PropModeReplace, (unsigned char *)embed, 2);
}

static void
sync_display_icon(Tray *tray)
{
	TrayWindow *w = tray->window;
	int visibility = tray->config.display_visibility;
	gboolean visible = visibility == DisplaysAlways || (visibility == DisplaysExternal && tray->display_external);
	if (visible && !w->icon) {
		make_icon(tray);
		w->dock_sent = 0;
		dock(tray);
	} else if (!visible && w->icon) {
		close_panel(tray, FALSE);
		shell_surface_free(&w->host, &w->icon_surface);
		XDestroyWindow(w->host.dpy, w->icon);
		w->icon = None;
		w->icon_hover = FALSE;
	}
}

static void
dock(Tray *tray)
{
	TrayWindow *w = tray->window;
	ShellHost *h = &w->host;
	Window owner = XGetSelectionOwner(h->dpy, w->tray_selection);
	if (owner != w->manager) {
		w->manager = owner;
		w->dock_sent = 0;
		if (owner)
			XSelectInput(h->dpy, owner, StructureNotifyMask | PropertyChangeMask);
		tray_palette(tray);
		tray_changed(tray);
	}
	if (!owner || !w->icon)
		return;
	Window root, parent, *children = NULL;
	unsigned int count;
	int exists = XQueryTree(h->dpy, w->icon, &root, &parent, &children, &count);
	if (children)
		XFree(children);
	double now = shell_seconds();
	if (!exists || parent != root || now-w->dock_sent < 1)
		return;
	XEvent message = {.xclient = {
		.type = ClientMessage, .display = h->dpy, .window = owner, .message_type = w->tray_opcode,
		.format = 32, .data.l = {CurrentTime, 0, w->icon, 0, 0}
	}};
	XSendEvent(h->dpy, owner, False, NoEventMask, &message);
	XFlush(h->dpy);
	w->dock_sent = now;
}

static cui_rect
monitor_at(const TrayWindow *w, int x, int y)
{
	Display *dpy = w->host.dpy;
#ifdef XINERAMA
	if (XineramaIsActive(dpy)) {
		int count;
		XineramaScreenInfo *monitors = XineramaQueryScreens(dpy, &count);
		for (int i = 0; monitors && i < count; ++i) {
			XineramaScreenInfo m = monitors[i];
			if (x >= m.x_org && x < m.x_org+m.width && y >= m.y_org && y < m.y_org+m.height) {
				XFree(monitors);
				return (cui_rect){m.x_org, m.y_org, m.width, m.height};
			}
		}
		if (monitors)
			XFree(monitors);
	}
#else
	(void)x;
	(void)y;
#endif
	return (cui_rect){0, 0, DisplayWidth(dpy, w->screen), DisplayHeight(dpy, w->screen)};
}

static void
size_panel(Tray *tray)
{
	TrayWindow *w = tray->window;
	Display *dpy = w->host.dpy;
	Window child, root;
	int x, y, unused_x, unused_y;
	unsigned mask;
	XWindowAttributes icon;
	gboolean anchored = w->icon && w->manager && XGetWindowAttributes(dpy, w->icon, &icon) &&
	                   icon.map_state == IsViewable &&
	                   XTranslateCoordinates(dpy, w->icon, w->host.root, w->icon_w/2, 0, &x, &y, &child);
	if (!anchored && !XQueryPointer(dpy, w->host.root, &root, &child, &x, &y, &unused_x, &unused_y, &mask))
		x = y = 0;
	cui_rect m = monitor_at(w, x, y);
	w->direction = !anchored || y < m.y+m.h/2 ? 1 : -1;
	int edge = 8;
	w->pad = shell_popup_padding(&w->host, &tray->config);
	w->width = MAX(1, MIN(tray->config.applet_width, m.w-2*(edge+w->pad)));
	int available = m.h - 2*(edge+w->pad);
	int dock_y = w->direction > 0 ? y+w->icon_h+9 : y-9;
	if (anchored) {
		int remaining = w->direction > 0 ? m.y+m.h-dock_y-edge : dock_y-m.y-edge;
		available = MIN(available, remaining-w->pad);
	}
	w->height = tray_ui_height(tray, MAX(1, available));
	w->x = CLAMP(x-w->width+28, m.x+edge, MAX(m.x+edge, m.x+m.w-edge-w->width))-w->pad;
	w->y = anchored ? dock_y-w->pad - (w->direction < 0 ? w->height : 0)
	       : CLAMP(y-w->height/3, m.y+edge, MAX(m.y+edge, m.y+m.h-edge-w->height))-w->pad;
	shell_popup_entrance(&tray->config, w->opened, shell_seconds(), w->direction, &w->offset_y);
}

static gboolean
frame(gpointer data)
{
	Tray *tray = data;
	TrayWindow *w = tray->window;
	double now = shell_seconds();
	gboolean animating = FALSE;
	if (w->panel) {
		int offset;
		animating = shell_popup_entrance(&tray->config, w->opened, now, w->direction, &offset);
		if (offset != w->offset_y) {
			w->offset_y = offset;
			XMoveWindow(w->host.dpy, w->panel, w->x, w->y + offset);
		}
	}
	animating |= shell_popup_exit_tick(&w->host, &w->exit, now);
	XFlush(w->host.dpy);
	if (!animating)
		w->frame_id = 0;
	return animating ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
animate(Tray *tray)
{
	TrayWindow *w = tray->window;
	if (!w->frame_id)
		w->frame_id = g_timeout_add(16, frame, tray);
}

static void
focus_field(Tray *tray, int field)
{
	TrayWindow *w = tray->window;
	w->field = field;
	w->cursor = field >= 0 && field < tray->prompt.count ? strlen(tray->prompt.fields[field].value) : 0;
	if (field >= 0 && w->fields_visible > 0)
		w->field_scroll = CLAMP(w->field_scroll, MAX(0, field-w->fields_visible+1), field);
	w->select_all = w->paste_pending = FALSE;
}

static const TrayHit *
tray_hit(const TrayWindow *w, int x, int y)
{
	for (int i = w->count-1; i >= 0; --i) {
		cui_rect r = w->hits[i].rect;
		if (x >= r.x && x < r.x+r.w && y >= r.y && y < r.y+r.h)
			return &w->hits[i];
	}
	return NULL;
}

static gboolean
redraw(gpointer data)
{
	Tray *tray = data;
	TrayWindow *w = tray->window;
	w->redraw_id = 0;
	if (tray->kind == TrayDisplays)
		sync_display_icon(tray);
	if (w->form_generation != tray->prompt_serial) {
		w->form_generation = tray->prompt_serial;
		w->field_scroll = 0;
		focus_field(tray, tray->prompt.count ? 0 : -1);
	}
	if (w->panel) {
		int x = w->x, y = w->y, width = w->width, height = w->height, pad = w->pad, offset = w->offset_y;
		size_panel(tray);
		if (w->x != x || w->y != y || w->width != width || w->height != height ||
		    w->pad != pad || w->offset_y != offset)
			XMoveResizeWindow(w->host.dpy, w->panel, w->x, w->y+w->offset_y,
			                  w->width+2*w->pad, w->height+2*w->pad);
		tray_ui_panel(tray);
		const TrayHit *hit = tray_hit(w, w->hover_x, w->hover_y);
		enum TrayCursor style = !hit ? TrayCursorNormal : hit->kind == HitField ? TrayCursorText : TrayCursorHand;
		if (w->pointer != style) {
			XChangeActivePointerGrab(w->host.dpy, ButtonPressMask | PointerMotionMask,
			                         w->cursors[style], CurrentTime);
			w->pointer = style;
		}
	}
	tray_ui_icon(tray);
	XFlush(w->host.dpy);
	/* Round trips during rendering can enqueue events in Xlib's own queue. */
	if (XEventsQueued(w->host.dpy, QueuedAlready))
		window_events(ConnectionNumber(w->host.dpy), G_IO_IN, tray);
	return G_SOURCE_REMOVE;
}

void
tray_changed(Tray *tray)
{
	TrayWindow *win = tray->window;
	if (!win->redraw_id)
		win->redraw_id = g_idle_add(redraw, tray);
}

gboolean
tray_panel_open(Tray *tray)
{
	TrayWindow *win = tray->window;
	return win->panel != None;
}

void
tray_nod_claim(Tray *tray, gboolean claim)
{
	TrayWindow *win = tray->window;
	Display *dpy = win->host.dpy;
	Window owner = XGetSelectionOwner(dpy, win->nod_client);
	if (claim && !owner)
		XSetSelectionOwner(dpy, win->nod_client, win->instance_window, CurrentTime);
	if (!claim && owner == win->instance_window)
		XSetSelectionOwner(dpy, win->nod_client, None, CurrentTime);
	XFlush(dpy);
}

gboolean
tray_popup(Tray *tray)
{
	TrayWindow *v = tray->window;
	ShellHost *h = &v->host;
	if (v->panel) {
		tray_changed(tray);
		return TRUE;
	}
	v->opened = shell_seconds();
	v->hover_x = v->hover_y = v->selected = -1;
	v->pointer = TrayCursorNormal;
	g_clear_pointer(&v->expanded, g_free);
	size_panel(tray);
	XSetWindowAttributes attrs = {
		.override_redirect = True, .background_pixel = 0, .border_pixel = 0, .colormap = h->cmap,
		.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | KeyPressMask,
	};
	v->panel = XCreateWindow(h->dpy, h->root, v->x, v->y+v->offset_y, v->width+2*v->pad, v->height+2*v->pad,
	                         0, h->depth, InputOutput, h->visual,
	                         CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &attrs);
	set_class(h->dpy, v->panel, "mwm-panel", applets[tray->kind].panel_title);
	XMapRaised(h->dpy, v->panel);
	int pointer = XGrabPointer(h->dpy, v->panel, False, ButtonPressMask | PointerMotionMask,
	                           GrabModeAsync, GrabModeAsync, None, v->cursors[v->pointer], CurrentTime);
	int keyboard = XGrabKeyboard(h->dpy, v->panel, False, GrabModeAsync, GrabModeAsync, CurrentTime);
	if (pointer != GrabSuccess || keyboard != GrabSuccess) {
		XDestroyWindow(h->dpy, v->panel);
		v->panel = None;
		tray_changed(tray);
		return FALSE;
	}
	if (v->im) {
		v->ic = XCreateIC(v->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
		                  XNClientWindow, v->panel, XNFocusWindow, v->panel, NULL);
		if (v->ic)
			XSetICFocus(v->ic);
	}
	tray_changed(tray);
	animate(tray);
	if (tray->kind == TrayWifi && !tray->answer)
		wifi_show(tray);
	if (tray->kind == TrayNotifications || tray->kind == TrayDisplays)
		v->scroll = 0;
	if (tray->kind == TrayNotifications)
		tray->unread = FALSE;
	if (tray->kind == TrayDisplays)
		displays_show(tray);
	return TRUE;
}

static void
close_panel(Tray *tray, gboolean abort)
{
	TrayWindow *w = tray->window;
	if (!w->panel)
		return;
	if (w->ic) {
		XUnsetICFocus(w->ic);
		XDestroyIC(w->ic);
		w->ic = NULL;
	}
	XUngrabPointer(w->host.dpy, CurrentTime);
	XUngrabKeyboard(w->host.dpy, CurrentTime);
	shell_popup_dismiss(&w->host, &tray->config, &w->exit, w->panel, &w->panel_surface, w->direction);
	w->panel = None;
	w->paste_pending = FALSE;
	if (abort && tray->answer)
		tray->answer(tray, FALSE);
	if (w->exit.win)
		animate(tray);
	tray_changed(tray);
}

void
tray_hide(Tray *tray)
{
	close_panel(tray, FALSE);
}

static void
answer(Tray *tray, gboolean accept)
{
	if (!tray->answer)
		return;
	if (accept) {
		for (int i = 0; i < tray->prompt.count; ++i) {
			TrayField *f = &tray->prompt.fields[i];
			if (f->required && !*f->value) {
				focus_field(tray, i);
				tray_status(tray, TRUE, "Fill in the required fields");
				return;
			}
		}
	}
	tray->answer(tray, accept);
}

static void
field_edit(Tray *tray, const char *bytes, size_t length, KeySym key)
{
	TrayWindow *w = tray->window;
	if (w->field < 0 || w->field >= tray->prompt.count || (length && !g_utf8_validate(bytes, length, NULL)))
		return;
	TrayField *f = &tray->prompt.fields[w->field];
	char *value = f->value;
	size_t len = strlen(value);
	char *cursor = value + w->cursor;
	char *before = cursor > value ? g_utf8_find_prev_char(value, cursor) : cursor;
	char *after = *cursor ? g_utf8_next_char(cursor) : cursor;
	if (key == XK_Left || key == XK_Right || key == XK_Home || key == XK_End) {
		w->cursor = key == XK_Home ? 0 : key == XK_End ? (int)len : (int)((key == XK_Left ? before : after)-value);
		w->select_all = FALSE;
	} else if (length || key == XK_BackSpace || key == XK_Delete) {
		for (size_t i = 0; i < length; ++i)
			if ((unsigned char)bytes[i] < ' ' || bytes[i] == 127 ||
			    (f->digits && (bytes[i] < '0' || bytes[i] > '9')))
				return;
		char *start = w->select_all ? value : key == XK_BackSpace ? before : cursor;
		char *end = w->select_all ? value+len : key == XK_Delete ? after : cursor;
		if (len - (end-start) + length >= sizeof f->value)
			return;
		memmove(start+length, end, strlen(end)+1);
		if (length)
			memcpy(start, bytes, length);
		w->cursor = start-value+length;
		w->select_all = FALSE;
	}
	tray_changed(tray);
}

static void
row_action(Tray *tray, const char *id)
{
	if (tray->kind != TrayNotifications) {
		tray_action(tray, TrayRow, id);
		return;
	}
	TrayWindow *w = tray->window;
	g_free(w->expanded);
	w->expanded = g_strdup(id);
	w->detail_scroll = 0;
	w->detail_bounds = (cui_rect){0};
	tray_changed(tray);
}

static void
detail_scroll(Tray *tray, int delta)
{
	TrayWindow *w = tray->window;
	w->detail_scroll = CLAMP(w->detail_scroll + delta, 0, MAX(0, (int)w->detail_bounds.h-w->detail_viewport));
	tray_changed(tray);
}

static void
choose(Tray *tray, int index, enum TrayAction action)
{
	TrayEntry *row = tray_row(tray, index);
	if (row && action == TrayRow)
		row_action(tray, row->id);
	else if (row)
		tray_action(tray, action, row->id);
}

static void
set_tab(Tray *tray, int tab)
{
	TrayWindow *w = tray->window;
	w->tab = tab;
	w->selected = -1;
	w->scroll = 0;
	tray_changed(tray);
}

static void
panel_key(Tray *tray, XKeyEvent *event)
{
	TrayWindow *w = tray->window;
	KeySym key = NoSymbol;
	char bytes[64];
	Status status = 0;
	int length = w->ic ? Xutf8LookupString(w->ic, event, bytes, sizeof bytes, &key, &status)
	                   : XLookupString(event, bytes, sizeof bytes, &key, NULL);
	if (status == XBufferOverflow)
		return;
	if (key == XK_Escape) {
		if (tray->answer)
			answer(tray, FALSE);
		else
			close_panel(tray, FALSE);
		return;
	}
	if (tray->answer) {
		int count = tray->prompt.count;
		if (key == XK_Return || key == XK_KP_Enter) {
			answer(tray, TRUE);
		} else if (count && (key == XK_Tab || key == XK_ISO_Left_Tab || key == XK_Up || key == XK_Down)) {
			int step = key == XK_ISO_Left_Tab || key == XK_Up || (event->state & ShiftMask) ? count-1 : 1;
			focus_field(tray, (w->field + step) % count);
			tray_changed(tray);
		} else if (count && (event->state & ControlMask) && (key == XK_a || key == XK_u || key == XK_v)) {
			if (key == XK_v) {
				w->paste_pending = TRUE;
				XConvertSelection(w->host.dpy, w->clipboard, w->utf8, w->paste, w->panel, event->time);
			} else {
				w->select_all = TRUE;
				if (key == XK_u)
					field_edit(tray, NULL, 0, XK_Delete);
			}
		} else {
			field_edit(tray, bytes, MAX(0, length), key);
		}
		return;
	}
	if (tray->kind == TrayWifi && (key == XK_Tab || key == XK_Left || key == XK_Right)) {
		set_tab(tray, !w->tab);
		return;
	}
	if (tray->kind == TrayDisplays) {
		if (key == XK_o || key == XK_O) {
			tray_action(tray, TrayOpenEditor, NULL);
			return;
		}
		if (tray->display_remaining) {
			if (key == XK_k || key == XK_K)
				tray_action(tray, TrayKeep, NULL);
			else if (key == XK_r || key == XK_R)
				tray_action(tray, TrayRevert, NULL);
			return;
		}
		if (key == XK_m || key == XK_M || key == XK_i || key == XK_I) {
			tray_action(tray, key == XK_m || key == XK_M ? TrayMirror : TrayIdentify, NULL);
			return;
		}
	}
	TrayEntry *expanded = tray_expanded(tray);
	if (expanded) {
		if (key == XK_Left || key == XK_BackSpace) {
			g_clear_pointer(&w->expanded, g_free);
			tray_changed(tray);
		} else if (key == XK_Delete) {
			tray_action(tray, TrayForget, expanded->id);
		} else if (key == XK_Up || key == XK_Down || key == XK_Page_Up || key == XK_Page_Down) {
			int delta = key == XK_Up ? -40 : key == XK_Down ? 40 :
			            key == XK_Page_Up ? -w->detail_viewport : w->detail_viewport;
			detail_scroll(tray, delta);
		}
		return;
	}
	if (key == XK_Return || key == XK_KP_Enter) {
		choose(tray, w->selected, TrayRow);
		return;
	}
	if (key == XK_Delete) {
		choose(tray, w->selected, TrayForget);
		return;
	}
	if (key == XK_r || key == XK_R) {
		tray_action(tray, TrayScan, NULL);
		return;
	}
	if (tray->kind == TrayBluetooth && (key == XK_v || key == XK_V)) {
		tray_action(tray, TrayVisible, NULL);
		return;
	}
	if (key == XK_space) {
		tray_action(tray, TrayToggle, NULL);
		return;
	}
	int step = key == XK_Up ? -1 : key == XK_Down ? 1 :
	           key == XK_Page_Up ? -w->visible_rows : key == XK_Page_Down ? w->visible_rows : 0;
	if (step) {
		w->selected = CLAMP(w->selected+step, -1, tray_row_count(tray)-1);
		if (w->selected >= 0)
			w->scroll = CLAMP(w->scroll, MAX(0, w->selected-w->visible_rows+1), w->selected);
		tray_changed(tray);
	}
}

static void
panel_click(Tray *tray, XButtonEvent *event)
{
	TrayWindow *w = tray->window;
	int x = event->x, y = event->y;
	if (x < w->pad || x >= w->pad+w->width || y < w->pad || y >= w->pad+w->height) {
		int toggle = FALSE;
		if (event->button == Button3 && tray->kind != TrayDisplays && w->icon) {
			int ix, iy;
			Window child;
			toggle = XTranslateCoordinates(w->host.dpy, w->host.root, w->icon,
			                               event->x_root, event->y_root, &ix, &iy, &child) &&
			         ix >= 0 && ix < w->icon_w && iy >= 0 && iy < w->icon_h;
		}
		if (event->button == Button1 || event->button == Button3)
			close_panel(tray, FALSE);
		if (toggle)
			tray_action(tray, TrayToggle, NULL);
		return;
	}
	if (event->button == Button4 || event->button == Button5) {
		int delta = event->button == Button4 ? -1 : 1;
		if (tray->answer)
			w->field_scroll += delta;
		else if (tray_expanded(tray))
			detail_scroll(tray, 40*delta);
		else {
			w->scroll += delta;
			w->selected = -1;
		}
		tray_changed(tray);
		return;
	}
	const TrayHit *target = tray_hit(w, x, y);
	if (target) {
		TrayHit hit = *target;
		if (event->button == Button3 && hit.kind == HitRow && tray->kind != TrayDisplays)
			tray_action(tray, TrayForget, hit.row);
		if (event->button != Button1)
			return;
		switch (hit.kind) {
		case HitRadio: tray_action(tray, TrayToggle, NULL); break;
		case HitScan: tray_action(tray, TrayScan, NULL); break;
		case HitWifi: set_tab(tray, 0); break;
		case HitVpn: set_tab(tray, 1); break;
		case HitHidden: tray_action(tray, TrayHidden, NULL); break;
		case HitVisible: tray_action(tray, TrayVisible, NULL); break;
		case HitRow: row_action(tray, hit.row); break;
		case HitForget: tray_action(tray, TrayForget, hit.row); break;
		case HitField: focus_field(tray, hit.value); break;
		case HitAccept: answer(tray, TRUE); break;
		case HitCancel: answer(tray, FALSE); break;
		case HitClear: tray_action(tray, TrayClear, NULL); break;
		case HitBack: g_clear_pointer(&w->expanded, g_free); break;
		case HitDisplayAction: tray_action(tray, hit.value, NULL); break;
		}
		tray_changed(tray);
	}
}

static void
paste(Tray *tray, const XSelectionEvent *selection)
{
	TrayWindow *w = tray->window;
	if (!w->paste_pending || selection->requestor != w->panel || selection->property != w->paste)
		return;
	w->paste_pending = FALSE;
	Atom type;
	int format;
	unsigned long count, remainder;
	unsigned char *bytes = NULL;
	if (XGetWindowProperty(w->host.dpy, w->panel, w->paste, 0, TrayFieldSize/4, True, w->utf8,
	                       &type, &format, &count, &remainder, &bytes) == Success &&
	    type == w->utf8 && format == 8 && !remainder)
		field_edit(tray, (char *)bytes, count, NoSymbol);
	if (bytes) {
		volatile unsigned char *wipe = bytes;
		if (format == 8)
			for (unsigned long i = 0; i < count; ++i)
				wipe[i] = 0;
		XFree(bytes);
	}
	XDeleteProperty(w->host.dpy, w->panel, w->paste);
}

static gboolean
window_events(gint fd, GIOCondition condition, gpointer data)
{
	(void)fd;
	Tray *tray = data;
	TrayWindow *v = tray->window;
	Display *dpy = v->host.dpy;
	if (condition & (G_IO_ERR | G_IO_HUP)) {
		g_main_loop_quit(v->loop);
		return G_SOURCE_REMOVE;
	}
	while (XPending(dpy)) {
		XEvent event;
		XNextEvent(dpy, &event);
		if (XFilterEvent(&event, None))
			continue;
		Window window = event.xany.window;
		if (event.type == ClientMessage && event.xclient.message_type == v->manager_atom &&
		    (Atom)event.xclient.data.l[1] == v->tray_selection) {
			dock(tray);
		} else if (event.type == DestroyNotify &&
		           (event.xdestroywindow.window == v->manager || event.xdestroywindow.window == v->icon)) {
			if (event.xdestroywindow.window == v->icon) {
				shell_surface_free(&v->host, &v->icon_surface);
				make_icon(tray);
				v->dock_sent = 0;
			}
			dock(tray);
		} else if (event.type == PropertyNotify && event.xproperty.state == PropertyNewValue && window == v->host.root &&
		           (event.xproperty.atom == v->config_serial || event.xproperty.atom == XA_RESOURCE_MANAGER)) {
			read_config(tray);
		} else if (event.type == PropertyNotify && window == v->manager && event.xproperty.atom == v->colors) {
			tray_palette(tray);
			tray_changed(tray);
		} else if (window == v->icon) {
			if (event.type == ButtonPress && event.xbutton.button == Button1)
				tray_popup(tray);
			if (event.type == ButtonPress && event.xbutton.button == Button3 && tray->kind != TrayDisplays)
				tray_action(tray, TrayToggle, NULL);
			if (event.type == Expose || event.type == EnterNotify || event.type == LeaveNotify) {
				if (event.type != Expose)
					v->icon_hover = event.type == EnterNotify;
				tray_changed(tray);
			}
			if (event.type == ConfigureNotify) {
				v->icon_w = event.xconfigure.width;
				v->icon_h = event.xconfigure.height;
				tray_changed(tray);
			}
		} else if (v->panel && window == v->panel) {
			if (event.type == ButtonPress)
				panel_click(tray, &event.xbutton);
			if (event.type == KeyPress)
				panel_key(tray, &event.xkey);
			if (event.type == MotionNotify) {
				v->hover_x = event.xmotion.x;
				v->hover_y = event.xmotion.y;
				tray_changed(tray);
			}
			if (event.type == SelectionNotify)
				paste(tray, &event.xselection);
			if (event.type == Expose)
				tray_changed(tray);
		}
	}
	return G_SOURCE_CONTINUE;
}

static gboolean
housekeeping(gpointer data)
{
	Tray *tray = data;
	TrayWindow *v = tray->window;
	dock(tray);
	if (tray->kind == TrayDisplays)
		displays_tick(tray);
	int composited = shell_composited(&v->host);
	if (v->host.composited != composited) {
		v->host.composited = composited;
		shell_popup_exit_reset(&v->host, &v->exit);
		tray_changed(tray);
	}
	if (v->panel && !tray->answer) {
		XWindowAttributes attrs;
		if (!v->manager || !XGetWindowAttributes(v->host.dpy, v->icon, &attrs) || attrs.map_state != IsViewable)
			close_panel(tray, FALSE);
	}
	window_events(ConnectionNumber(v->host.dpy), G_IO_IN, tray);
	return G_SOURCE_CONTINUE;
}

static gboolean
reload_signal(gpointer data)
{
	read_config(data);
	return G_SOURCE_CONTINUE;
}

static gboolean
stop(gpointer data)
{
	Tray *tray = data;
	TrayWindow *win = tray->window;
	g_main_loop_quit(win->loop);
	return G_SOURCE_REMOVE;
}

int
main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "-v")) {
		puts("mwm-tray-" VERSION);
		return EXIT_SUCCESS;
	}
	int first = 1;
	char config_path[4096];
	TrayWindow win = {.field = -1};
	if (argc >= 4 && !strcmp(argv[first], "-c")) {
		win.config_path = argv[first+1];
		win.explicit_config = TRUE;
		first += 2;
	} else if (config_default_path(config_path, sizeof config_path)) {
		win.config_path = config_path;
	}
	enum TrayKind kind = TrayKindCount;
	if (argc == first+1)
		for (int i = 0; i < TrayKindCount; ++i)
			if (!strcmp(argv[first], applets[i].command))
				kind = i;
	if (!win.config_path || kind == TrayKindCount) {
		fputs("usage: mwm-tray [-c config.ini] wifi|bluetooth|notifications|displays\n", stderr);
		return EXIT_FAILURE;
	}
	Tray tray = {.window = &win, .kind = kind};
	win.title = applets[kind].title;
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fputs("mwm-tray: cannot open X display\n", stderr);
		return EXIT_FAILURE;
	}
	fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);
	XSetErrorHandler(xerror);
	win.cursors[TrayCursorNormal] = XCreateFontCursor(dpy, XC_left_ptr);
	win.cursors[TrayCursorHand] = XCreateFontCursor(dpy, XC_hand2);
	win.cursors[TrayCursorText] = XCreateFontCursor(dpy, XC_xterm);
	win.screen = DefaultScreen(dpy);
	ShellHost *h = &win.host;
	*h = (ShellHost){.dpy = dpy, .root = RootWindow(dpy, win.screen), .visual = DefaultVisual(dpy, win.screen),
	                 .depth = DefaultDepth(dpy, win.screen), .cmap = DefaultColormap(dpy, win.screen)};
	int shape_event, shape_error;
	h->shape = XShapeQueryExtension(dpy, &shape_event, &shape_error);
	XVisualInfo visual;
	XRenderPictFormat *format;
	if (XMatchVisualInfo(dpy, win.screen, 32, TrueColor, &visual) &&
	    (format = XRenderFindVisualFormat(dpy, visual.visual)) && format->direct.alphaMask == 255) {
		h->visual = visual.visual;
		h->depth = visual.depth;
		h->cmap = XCreateColormap(dpy, h->root, h->visual, AllocNone);
	}
	h->composited = shell_composited(h);
	win.tray_selection = screen_atom(&win, "_NET_SYSTEM_TRAY_S");
	win.instance = screen_atom(&win, applets[kind].instance);
	if (kind == TrayNotifications)
		win.nod_client = screen_atom(&win, "_NOD_TRAY_CLIENT_S");
	win.tray_opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	win.embed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
	win.manager_atom = XInternAtom(dpy, "MANAGER", False);
	win.colors = XInternAtom(dpy, "_NET_SYSTEM_TRAY_COLORS", False);
	win.config_serial = XInternAtom(dpy, "_MWM_CONFIG_SERIAL", False);
	win.clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	win.utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	win.paste = XInternAtom(dpy, "_MWM_TRAY_PASTE", False);
	if (!read_config(&tray))
		return EXIT_FAILURE;
	if (kind != TrayDisplays)
		make_icon(&tray);
	XGrabServer(dpy);
	Window other = XGetSelectionOwner(dpy, win.instance);
	if (!other) {
		win.instance_window = XCreateSimpleWindow(dpy, h->root, 0, 0, 1, 1, 0, 0, 0);
		XSetSelectionOwner(dpy, win.instance, win.instance_window, CurrentTime);
	}
	XUngrabServer(dpy);
	if (other) {
		XCloseDisplay(dpy);
		return EXIT_SUCCESS;
	}
	XSelectInput(dpy, h->root, StructureNotifyMask | PropertyChangeMask);
	win.im = XOpenIM(dpy, NULL, NULL, NULL);
	tray_model(&tray, "Connecting…", kind == TrayNotifications ? "Waiting for nod" :
	           kind == TrayDisplays ? "" : "Waiting for the system service");
	tray_bus_start(&tray);
	win.loop = g_main_loop_new(NULL, FALSE);
	g_unix_fd_add(ConnectionNumber(dpy), G_IO_IN | G_IO_HUP | G_IO_ERR, window_events, &tray);
	g_unix_signal_add(SIGINT, stop, &tray);
	g_unix_signal_add(SIGTERM, stop, &tray);
	g_unix_signal_add(SIGHUP, reload_signal, &tray);
	g_timeout_add_seconds(1, housekeeping, &tray);
	dock(&tray);
	g_main_loop_run(win.loop);
	if (kind == TrayDisplays)
		displays_stop(&tray);
	close_panel(&tray, TRUE);
	shell_popup_exit_reset(h, &win.exit);
	shell_surface_free(h, &win.icon_surface);
	shell_font_free(&win.font);
	tray_ui_glyphs_free(&win);
	g_free(win.expanded);
	if (win.im)
		XCloseIM(win.im);
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}
