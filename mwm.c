/* mwm - matteo's window manager
 *
 * A skimmed-down, dwm-flavoured tiling window manager. Design highlights:
 *   - one global pool of tags (max MAX_TAGS); each tag is owned by exactly one
 *     monitor (window in tag, tag on monitor: window <= tag <= monitor);
 *   - two tiling layouts (cols "|||", master+stack "||="), plus floating;
 *   - configurable gaps, optional rounded corners (X Shape, compositor-free);
 *   - a translucent three-pill bar (left tags+layout, center title, right
 *     status read from the root window name a la dwm/slstatus) + XEmbed systray;
 *   - NO built-in keybindings: everything is driven through the `mwmc` client
 *     over a pure-X11 ClientMessage IPC channel (see ipc.h).
 *
 * The code intentionally mirrors dwm's structure where it can, so dwm patches
 * and idioms map over cleanly. See LICENSE (MIT/X) for copyright details.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */
#include <X11/extensions/Xrender.h>

#include "shell.h"
#include "util.h"
#include "ipc.h"

/* macros */
#define BUTTONMASK              (ButtonPressMask|ButtonReleaseMask)
#define INTERSECT(x,y,w,h,m)    (MAX(0, MIN((x)+(w),(m)->mx+(m)->mw) - MAX((x),(m)->mx)) \
                               * MAX(0, MIN((y)+(h),(m)->my+(m)->mh) - MAX((y),(m)->my)))
#define ISVISIBLE(C)            (isvisible(C))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define MOUSEMASK               (BUTTONMASK|PointerMotionMask)
#define ROOTMASK                (SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask \
                                 | PointerMotionMask | EnterWindowMask | LeaveWindowMask | StructureNotifyMask | PropertyChangeMask)
#define WIDTH(X)                ((X)->w + 2 * (X)->bw)
#define HEIGHT(X)               ((X)->h + 2 * (X)->bw)

/* enums */
enum { CurNormal, CurResize, CurMove, CurLast }; /* cursor */
enum { NetSupported, NetWMName, NetWMState, NetWMCheck,
       NetSystemTray, NetSystemTrayOP, NetSystemTrayOrientation, NetSystemTrayOrientationHorz,
       NetSystemTrayVisual, NetWMWindowType, NetWMWindowTypeDock, NetWMWindowTypeDialog,
       NetWMFullscreen, NetActiveWindow, NetWMStateDemandsAttention, NetClientList, NetLast };
enum { Manager, Xembed, XembedInfo, XLast }; /* Xembed atoms */
enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMLast }; /* default atoms */
enum { MwmMsg, MwmCmd, MwmReply, MwmLast }; /* IPC atoms */

typedef struct Monitor Monitor;
typedef struct Client Client;

struct Client {
	char name[256];
	char class[64], instance[64];
	float mina, maxa;
	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
	int basew, baseh, incw, inch, maxw, maxh, minw, minh, hintsvalid;
	int bw, oldbw;
	int tag;
	int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen, issticky;
	int depth; /* XEmbed icons can have a different visual from the tray. */
	Client *next;   /* attach/tile order (global list) */
	Client *snext;  /* focus stack order (global list)  */
	Window win;
};

struct Monitor {
	char ltsymbol[16];
	int num;
	int mx, my, mw, mh;   /* monitor geometry */
	int wx, wy, ww, wh;   /* window/work area  */
	int by;               /* bar y position    */
	int seltag;           /* currently shown tag (-1 = none) */
	Client *sel;          /* last focused client on this monitor */
	Window barwin;
	ShellBar bar;
	int bardirty, barmapped;
	Monitor *next;
};

typedef struct {
	int layout;     /* 0 cols, 1 stack, 2 float */
	int nmaster;
	float mfact;
	int mon;        /* owning monitor index, -1 = unassigned */
} Tag;


typedef struct Systray {
	Window win;
	Client *icons;
	int monitor, x, y, width, height, mapped;
	XRectangle clip;
	unsigned long bg;
} Systray;

enum { MaxMonitors = 64 };

/* Private handoff format shared by binaries that implement restart v1. */
#define RESTART_CHECK "--check-restart-v1"
#define RESTART_RESUME "--resume-restart-v1"
static const char restart_magic[] = "mwm-handoff-v1";

typedef struct {
	int x, y, w, h, tag;
	Window focus;
} RestartMonitor;

typedef struct {
	Window win;
	int tag, x, y, w, h, oldx, oldy, oldw, oldh, bw, oldbw;
	int floating, fullscreen, sticky, oldstate, urgent;
} RestartClient;

typedef struct {
	Window root, checkwin, requestor;
	int display_fd;
	int selected, nmon, bar, tray, gap, gap_override;
	size_t nclients, nicons;
	/* Keep this v1 layout readable by the first binaries with live restart. */
	struct { int active, at; double due; time_t wall; } timer;
	Tag tags[MAX_TAGS];
	RestartMonitor monitors[MaxMonitors];
} RestartState;

/* function declarations */
static void applyrules(Client *c);
static int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact);
static void arrange(Monitor *m);
static void arrangemon(Monitor *m);
static void attach(Client *c);
static void attachafter(Client *c, Client *after);
static void attachstack(Client *c);
static void buttonpress(XEvent *e);
static void checkotherwm(void);
static void cleanup(void);
static void clearround(Window w);
static Monitor *clientmon(const Client *c);
static void clientmessage(XEvent *e);
static void cols(Monitor *m, int t);
static void configure(Client *c);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static Monitor *createmon(int num);
static void destroynotify(XEvent *e);
static void detach(Client *c);
static void detachstack(Client *c);
static void dispatchcmd(int argc, char **argv, char *reply, size_t rsz);
static void dockicon(Window icon, const XWindowAttributes *wa, Client **link);
static Monitor *dirtomon(int dir);
static void drawbar(Monitor *m);
static void drawbars(void);
static void enternotify(XEvent *e);
static void expose(XEvent *e);
static void focus(Client *c);
static void focusclientbyclass(const char *cls);
static void focusin(XEvent *e);
static void focusmon(const char *arg);
static void focusstack(int dir);
static Atom getatomprop(Client *c, Atom prop);
static int getrootptr(int *x, int *y);
static long getstate(Window w);
static int gettextprop(Window w, Atom atom, char *text, unsigned int size);
static void grabbuttons(Client *c, int focused);
static void handleipc(Window requestor);
static void incnmaster(const char *arg);
static int isvisible(const Client *c);
static void killfocused(void);
static void manage(Window w, XWindowAttributes *wa);
static void mappingnotify(XEvent *e);
static void leavebar(XEvent *e);
static void maprequest(XEvent *e);
static void motionnotify(XEvent *e);
static void movemouse(void);
static void resizemouse(void);
static void focuslast(void);
static void movewinmon(int dir);
static void winmove(int dx, int dy);
static void winresize(int dw, int dh);
static void wincenter(void);
static void movetagtomon(const char *arg);
static void movewintotag(const char *arg);
static Monitor *montag(int t);
static Client *nexttiled(Client *c, int t);
static int occupied(int t);
static void propertynotify(XEvent *e);
static Monitor *recttomon(int x, int y, int w, int h);
static void removesystrayicon(Client *i);
static void resize(Client *c, int x, int y, int w, int h, int interact);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void resizerequest(XEvent *e);
static void restack(Monitor *m);
static void maskroundrect(Pixmap mask, GC g, int x, int y, int w, int h, int rad);
static void roundwin(Window win, int w, int h, int rad, int bw);
static void run(void);
static const char *restart(Window requestor);
static void scan(void);
static int sendevent(Window w, Atom proto, int mask, long d0, long d1, long d2, long d3, long d4);
static void sendmon(Client *c, int tag);
static void setclientstate(Client *c, long state);
static void setfocus(Client *c);
static void setfullscreen(Client *c, int fullscreen);
static void setgap(const char *arg);
static void setlayout(const char *arg);
static void setmfact(const char *arg);
static int settag(const char *arg, int cur);
static void setup(const RestartState *state);
static void seturgent(Client *c, int urg);
static void showhide(Client *c);
static void sigchld(int unused);
static void spawnreply(Window w, const char *s);
static int tagcount(void);
static int taglabel(int t, char *buf, size_t n);
static void tile(Monitor *m, int t);
static void togglebar(void);
static void toggletraybar(void);
static void togglefloating(void);
static void togglefullscreen(void);
static void togglesticky(void);
static void unfocus(Client *c, int setfocus);
static void unmanage(Client *c, int destroyed);
static void unmapnotify(XEvent *e);
static void updatebarpos(Monitor *m);
static void updatebars(void);
static void updateclientlist(void);
static int updategeom(int initial);
static void updatesizehints(Client *c);
static void updatestatus(void);
static void updatesystray(void);
static void painttray(XRectangle clip);
static void updatesystrayicongeom(Client *i, int w, int h);
static void updatesystrayiconstate(Client *i, XPropertyEvent *ev);
static void updatetitle(Client *c);
static void updatewindowtype(Client *c);
static void updatewmhints(Client *c);
static void viewtag(const char *arg);
static void warptoclient(Client *c);
static void warptomon(Monitor *m);
static Client *wintoclient(Window w);
static Monitor *wintomon(Window w);
static Client *wintosystrayicon(Window w);
static int xerror(Display *dpy, XErrorEvent *ee);
static int xerrordummy(Display *dpy, XErrorEvent *ee);
static int xerrorstart(Display *dpy, XErrorEvent *ee);
static void zoom(void);

/* XEmbed / systray protocol constants */
#define SYSTEM_TRAY_REQUEST_DOCK    0
#define _NET_SYSTEM_TRAY_ORIENTATION_HORZ 0
#define XEMBED_EMBEDDED_NOTIFY      0
#define XEMBED_WINDOW_ACTIVATE      1
#define XEMBED_FOCUS_IN             4
#define XEMBED_MODALITY_ON         10
#define XEMBED_MAPPED          (1 << 0)
#define VERSION_MAJOR               0
#define VERSION_MINOR               0
#define XEMBED_EMBEDDED_VERSION ((VERSION_MAJOR << 16) | VERSION_MINOR)

/* variables */
static const char broken[] = "broken";
static char stext[512];
static int screen;
static int sw, sh;              /* X display screen geometry width, height */
static int bh;                  /* bar height */
static int lrpad;               /* sum of left and right padding for text */
static int gappx_run;           /* runtime-adjustable gap */
static int (*xerrorxlib)(Display *, XErrorEvent *);
static void (*handler[LASTEvent]) (XEvent *) = {
	[ButtonPress] = buttonpress,
	[ClientMessage] = clientmessage,
	[ConfigureRequest] = configurerequest,
	[ConfigureNotify] = configurenotify,
	[DestroyNotify] = destroynotify,
	[EnterNotify] = enternotify,
	[LeaveNotify] = leavebar,
	[Expose] = expose,
	[FocusIn] = focusin,
	[MappingNotify] = mappingnotify,
	[MapRequest] = maprequest,
	[MotionNotify] = motionnotify,
	[PropertyNotify] = propertynotify,
	[ResizeRequest] = resizerequest,
	[UnmapNotify] = unmapnotify,
};
static Atom wmatom[WMLast], netatom[NetLast], xatom[XLast], mwmatom[MwmLast], xa_utf8;
static int running = 1;
static int shapeext = 0, shapeevbase = 0;
static Cursor cursor[CurLast];
static Display *dpy;
static Monitor *mons, *selmon;
static Client *clients = NULL;  /* global attach/tile order */
static Client *fstack = NULL;   /* global focus stack */
static Window root, wmcheckwin;
static Systray *systray = NULL;
static Tag tagstate[MAX_TAGS];

/* ARGB visual */
static Visual *visual;
static int depth;
static Colormap cmap;

static Config settings;
static ShellHost shell;
static ShellFont shellfont;
static ShellPanel panel;
static ShellTooltip tooltip;
static ShellTimer timer;
static ShellSystem system_stats;
static Controls controls;
static unsigned hidden_segments;
static double system_next;
static unsigned int theme_revision;
static const char *config_path;
static int required_config;
static char executable[PATH_MAX];
extern char **environ;
static int panel_dirty;
static int gap_overridden;
static const char *layoutsym[] = { "|||", "||=", "><>" };

static const char *reloadstyle(void);
static void openpanel(Monitor *, enum ShellAction, float anchor, Time);
static void adjust_control(int kind, int direction);
static void panel_event(XEvent *);
static int ui_tick(void);

static int
panel_action(enum ShellAction action)
{
	return action == ActCalendar || action == ActClock || action == ActPalette ||
	       shell_panel_control(action) >= 0;
}

/* ---- small helpers --------------------------------------------------- */

int
tagcount(void)
{
	return settings.ntags;
}

Monitor *
montag(int t)
{
	Monitor *m;
	if (t < 0 || t >= MAX_TAGS)
		return NULL;
	for (m = mons; m; m = m->next)
		if (m->num == tagstate[t].mon)
			return m;
	return NULL;
}

Monitor *
clientmon(const Client *c)
{
	return c ? montag(c->tag) : NULL;
}

int
isvisible(const Client *c)
{
	Monitor *o = montag(c->tag);
	return o && (o->seltag == c->tag || c->issticky);
}

int
occupied(int t)
{
	Client *c;
	for (c = clients; c; c = c->next)
		if (c->tag == t)
			return 1;
	return 0;
}

/* writes a printable label for tag t into buf, returns strlen */
int
taglabel(int t, char *buf, size_t n)
{
	if (t >= 0 && t < settings.ntags && settings.tags[t][0])
		snprintf(buf, n, "%s", settings.tags[t]);
	else
		snprintf(buf, n, "%d", t + 1);
	return strlen(buf);
}

/* next tiled client in `clients` order on monitor m showing tag t */
Client *
nexttiled(Client *c, int t)
{
	for (; c && (c->tag != t || c->isfloating || c->isfullscreen || !ISVISIBLE(c)); c = c->next)
		;
	return c;
}

/* ---- rules ----------------------------------------------------------- */

/* case-insensitive strstr: rule class/instance/title matches ignore
 * capitalisation, so a rule "pinentry" matches WM_CLASS res_class
 * "Pinentry-gtk" (and "Pavucontrol", "Arandr", etc. likewise). Portable
 * (no strcasestr/_GNU_SOURCE), uses only C99 tolower. */
static char *
ci_strstr(const char *hay, const char *needle)
{
	size_t i, nl = strlen(needle);

	if (!nl)
		return (char *)hay;
	for (; *hay; hay++) {
		for (i = 0; needle[i] && hay[i]; i++)
			if (tolower((unsigned char)hay[i]) != tolower((unsigned char)needle[i]))
				break;
		if (!needle[i])
			return (char *)hay;
	}
	return NULL;
}

static void
updateclass(Client *c, XClassHint *ch)
{
	XGetClassHint(dpy, c->win, ch);
	snprintf(c->class, sizeof c->class, "%s", ch->res_class ? ch->res_class : broken);
	snprintf(c->instance, sizeof c->instance, "%s", ch->res_name ? ch->res_name : broken);
}

void
applyrules(Client *c)
{
	int i;
	const Rule *r;
	Atom wt;
	XClassHint ch = { NULL, NULL };

	c->isfloating = 0;
	c->tag = -1;
	wt = getatomprop(c, netatom[NetWMWindowType]);
	updateclass(c, &ch);
	const char *class = ch.res_class ? ch.res_class : broken;
	const char *instance = ch.res_name ? ch.res_name : broken;

	for (i = 0; i < settings.nrules; i++) {
		r = &settings.rules[i];
		char type[sizeof "_NET_WM_WINDOW_TYPE_" + sizeof r->type];
		snprintf(type, sizeof type, "_NET_WM_WINDOW_TYPE_%s", r->type);
		if ((!r->title[0] || ci_strstr(c->name, r->title))
		&& (!r->class[0] || ci_strstr(class, r->class))
		&& (!r->instance[0] || ci_strstr(instance, r->instance))
		&& (!r->type[0] || (wt != None && wt == XInternAtom(dpy, type, False)))) {
			if (r->floating >= 0)
				c->isfloating = r->floating;
			if (r->tag[0]) {
				int t = settag(r->tag, -1);
				if (t >= 0)
					c->tag = t;
			}
		}
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);

	if (c->tag < 0) {
		/* default to the focused monitor's tag; if the monitor owns/shows no
		 * tag yet, claim the lowest UNOWNED tag for it (never steal a tag that
		 * already belongs to another monitor) */
		if (selmon->seltag < 0) {
			int t, nt = -1;
			for (t = 0; t < MAX_TAGS; t++)
				if (tagstate[t].mon < 0) { nt = t; break; }
			if (nt < 0)
				nt = 0;
			tagstate[nt].mon = selmon->num;
			selmon->seltag = nt;
		}
		c->tag = selmon->seltag;
	}
	/* if the target tag has no owner yet, give it to the focused monitor */
	if (tagstate[c->tag].mon < 0) {
		tagstate[c->tag].mon = selmon->num;
		if (montag(c->tag) && montag(c->tag)->seltag < 0)
			montag(c->tag)->seltag = c->tag;
	}
}

/* ---- geometry / size hints ------------------------------------------ */

int
applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact)
{
	int baseismin;
	Monitor *m = clientmon(c);

	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
		if (*x > sw) *x = sw - WIDTH(c);
		if (*y > sh) *y = sh - HEIGHT(c);
		if (*x + *w + 2 * c->bw < 0) *x = 0;
		if (*y + *h + 2 * c->bw < 0) *y = 0;
	} else if (m) {
		if (*x >= m->wx + m->ww) *x = m->wx + m->ww - WIDTH(c);
		if (*y >= m->wy + m->wh) *y = m->wy + m->wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= m->wx) *x = m->wx;
		if (*y + *h + 2 * c->bw <= m->wy) *y = m->wy;
	}
	if (*h < bh) *h = bh;
	if (*w < bh) *w = bh;
	if (settings.resizehints || c->isfloating) {
		if (!c->hintsvalid)
			updatesizehints(c);
		baseismin = c->basew == c->minw && c->baseh == c->minh;
		if (!baseismin) {
			*w -= c->basew;
			*h -= c->baseh;
		}
		if (c->mina > 0 && c->maxa > 0) {
			if (c->maxa < (float)*w / *h) *w = *h * c->maxa + 0.5;
			else if (c->mina < (float)*h / *w) *h = *w * c->mina + 0.5;
		}
		if (baseismin) {
			*w -= c->basew;
			*h -= c->baseh;
		}
		if (c->incw) *w -= *w % c->incw;
		if (c->inch) *h -= *h % c->inch;
		*w = MAX(*w + c->basew, c->minw);
		*h = MAX(*h + c->baseh, c->minh);
		if (c->maxw) *w = MIN(*w, c->maxw);
		if (c->maxh) *h = MIN(*h, c->maxh);
	}
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

/* ---- arrange / layouts ----------------------------------------------- */

void
arrange(Monitor *m)
{
	showhide(fstack);
	if (m) {
		arrangemon(m);
		restack(m);
	} else {
		for (m = mons; m; m = m->next)
			arrangemon(m);
		for (m = mons; m; m = m->next)
			restack(m);
	}
}

void
arrangemon(Monitor *m)
{
	int t = m->seltag;

	if (t < 0) {
		strncpy(m->ltsymbol, "", sizeof(m->ltsymbol));
		return;
	}
	strncpy(m->ltsymbol, layoutsym[tagstate[t].layout], sizeof(m->ltsymbol) - 1);
	switch (tagstate[t].layout) {
	case 0: cols(m, t); break;
	case 1: tile(m, t); break;
	default: break; /* floating: nothing to do */
	}
}

void
cols(Monitor *m, int t)
{
	Client *c, **arr;
	unsigned int i, n;
	int og = gappx_run, ig = gappx_run;
	int avail, base, extra, x, wcol, bw;

	for (n = 0, c = clients; c; c = c->next)
		if (c->tag == t && !c->isfloating && !c->isfullscreen && ISVISIBLE(c))
			n++;
	if (n == 0)
		return;
	arr = ecalloc(n, sizeof(Client *));
	for (i = 0, c = clients; c; c = c->next)
		if (c->tag == t && !c->isfloating && !c->isfullscreen && ISVISIBLE(c))
			arr[i++] = c;

	avail = m->ww - 2 * og - (int)(n - 1) * ig;
	if (avail < (int)n)
		avail = n;
	base = avail / n;
	extra = avail % n;
	x = m->wx + og;
	for (i = 0; i < n; i++) {
		bw = arr[i]->bw;
		wcol = base + ((int)i < extra ? 1 : 0);
		resize(arr[i], x, m->wy + og, wcol - 2 * bw, m->wh - 2 * og - 2 * bw, 0);
		x += wcol + ig;
	}
	free(arr);
}

void
tile(Monitor *m, int t)
{
	Client *c, **arr;
	unsigned int i, n, nmm, ns;
	int og = gappx_run, ig = gappx_run;
	int mw, sx, sw, availh, base, extra, y, hh, bw;

	for (n = 0, c = clients; c; c = c->next)
		if (c->tag == t && !c->isfloating && !c->isfullscreen && ISVISIBLE(c))
			n++;
	if (n == 0)
		return;
	arr = ecalloc(n, sizeof(Client *));
	for (i = 0, c = clients; c; c = c->next)
		if (c->tag == t && !c->isfloating && !c->isfullscreen && ISVISIBLE(c))
			arr[i++] = c;

	nmm = tagstate[t].nmaster;
	if (nmm > n)
		nmm = n;
	ns = n - nmm;

	if (nmm == 0)
		mw = 0;
	else if (ns > 0)
		mw = (m->ww - 2 * og - ig) * tagstate[t].mfact;
	else
		mw = m->ww - 2 * og;

	/* master column(s) */
	if (nmm > 0) {
		availh = m->wh - 2 * og - (int)(nmm - 1) * ig;
		if (availh < (int)nmm) availh = nmm;
		base = availh / nmm;
		extra = availh % nmm;
		y = m->wy + og;
		for (i = 0; i < nmm; i++) {
			bw = arr[i]->bw;
			hh = base + ((int)i < extra ? 1 : 0);
			resize(arr[i], m->wx + og, y, mw - 2 * bw, hh - 2 * bw, 0);
			y += hh + ig;
		}
	}
	/* stack column */
	if (ns > 0) {
		sx = m->wx + og + (nmm ? mw + ig : 0);
		sw = m->ww - 2 * og - (nmm ? mw + ig : 0);
		availh = m->wh - 2 * og - (int)(ns - 1) * ig;
		if (availh < (int)ns) availh = ns;
		base = availh / ns;
		extra = availh % ns;
		y = m->wy + og;
		for (i = 0; i < ns; i++) {
			bw = arr[nmm + i]->bw;
			hh = base + ((int)i < extra ? 1 : 0);
			resize(arr[nmm + i], sx, y, sw - 2 * bw, hh - 2 * bw, 0);
			y += hh + ig;
		}
	}
	free(arr);
}

/* ---- list management ------------------------------------------------- */

void
attach(Client *c)
{
	c->next = clients;
	clients = c;
}

void
attachafter(Client *c, Client *after)
{
	if (!after) {
		attach(c);
		return;
	}
	c->next = after->next;
	after->next = c;
}

void
attachstack(Client *c)
{
	c->snext = fstack;
	fstack = c;
}

void
detach(Client *c)
{
	Client **tc;
	for (tc = &clients; *tc && *tc != c; tc = &(*tc)->next)
		;
	*tc = c->next;
}

void
detachstack(Client *c)
{
	Client **tc;
	for (tc = &fstack; *tc && *tc != c; tc = &(*tc)->snext)
		;
	*tc = c->snext;

	if (c == c->snext) /* paranoia */
		c->snext = NULL;
	{
		Monitor *m;
		for (m = mons; m; m = m->next)
			if (m->sel == c)
				m->sel = NULL;
	}
}

/* ---- focus ----------------------------------------------------------- */

void
focus(Client *c)
{
	if (!c || !ISVISIBLE(c))
		for (c = fstack; c && (!ISVISIBLE(c) || clientmon(c) != selmon); c = c->snext)
			;
	if (selmon->sel && selmon->sel != c)
		unfocus(selmon->sel, 0);
	if (c) {
		if (clientmon(c) && clientmon(c) != selmon)
			selmon = clientmon(c);
		if (c->isurgent)
			seturgent(c, 0);
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		XSetWindowBorder(dpy, c->win,
		                 0xff000000UL | shell_rgb(settings.palette[SchemeSel][ColBorder]));
		setfocus(c);
	} else {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
	selmon->sel = c;
	drawbars();
}

void
focusin(XEvent *e)
{
	XFocusChangeEvent *ev = &e->xfocus;

	if (!panel.win && selmon->sel && ev->window != selmon->sel->win)
		setfocus(selmon->sel);
}

void
setfocus(Client *c)
{
	if (!c->neverfocus) {
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		XChangeProperty(dpy, root, netatom[NetActiveWindow], XA_WINDOW, 32,
		                PropModeReplace, (unsigned char *) &(c->win), 1);
	}
	sendevent(c->win, wmatom[WMTakeFocus], NoEventMask, wmatom[WMTakeFocus], CurrentTime, 0, 0, 0);
}

void
unfocus(Client *c, int setfocus)
{
	if (!c)
		return;
	grabbuttons(c, 0);
	XSetWindowBorder(dpy, c->win, shell_rgb(settings.palette[SchemeNorm][ColBorder]) | 0xff000000UL);
	if (setfocus) {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
}

void
focusstack(int dir)
{
	Client *c = NULL, *i;

	if (!selmon->sel || selmon->sel->isfullscreen)
		return;
	if (dir > 0) {
		for (c = selmon->sel->next; c && !(ISVISIBLE(c) && clientmon(c) == selmon); c = c->next)
			;
		if (!c)
			for (c = clients; c && !(ISVISIBLE(c) && clientmon(c) == selmon); c = c->next)
				;
	} else {
		for (i = clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i) && clientmon(i) == selmon)
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i) && clientmon(i) == selmon)
					c = i;
	}
	if (c) {
		focus(c);
		restack(selmon);
		warptoclient(c);
	}
}

void
focusclientbyclass(const char *cls)
{
	Client *c;
	for (c = clients; c; c = c->next)
		if (strstr(c->class, cls)) {
			Monitor *m = montag(c->tag);
			if (m) {
				m->seltag = c->tag;
				selmon = m;
			}
			arrange(NULL);
			focus(c);
			restack(selmon);
			warptoclient(c);
			return;
		}
}

void
focusmon(const char *arg)
{
	Monitor *m;

	if (!mons->next)
		return;
	if (!strcmp(arg, "next"))
		m = dirtomon(+1);
	else if (!strcmp(arg, "prev"))
		m = dirtomon(-1);
	else {
		int n = atoi(arg);
		for (m = mons; m && m->num != n; m = m->next)
			;
		if (!m)
			return;
	}
	if (m == selmon)
		return;
	unfocus(selmon->sel, 0);
	selmon = m;
	warptomon(selmon);
	focus(NULL);
}

void
warptoclient(Client *c)
{
	/* move the pointer onto the focused client so focus-follows-mouse agrees
	 * with an explicit CLI focus change instead of overriding it */
	if (!settings.warpcursor || !c)
		return;
	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w / 2, c->h / 2);
}

void
grabbuttons(Client *c, int focused)
{
	/* While unfocused, grab clicks so the first click focuses the window
	 * (buttonpress() replays it to the app). Once focused, release the grab so
	 * clicks go straight to the client — exactly like dwm. */
	XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
	if (!focused) {
		XGrabButton(dpy, AnyButton, AnyModifier, c->win, False,
		            ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
	} else {
		/* on the focused window, plain clicks pass through to the app;
		 * only the move modifier + Btn1/Btn3 are grabbed for move/resize */
		XGrabButton(dpy, Button1, settings.movemod, c->win, False,
		            ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
		XGrabButton(dpy, Button3, settings.movemod, c->win, False,
		            ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
	}
}

void
warptomon(Monitor *m)
{
	/* move the pointer to the centre of m so focus-follows-mouse agrees with
	 * an explicit CLI monitor switch instead of immediately overriding it */
	if (!settings.warpcursor || !m)
		return;
	XWarpPointer(dpy, None, root, 0, 0, 0, 0, m->mx + m->mw / 2, m->my + m->mh / 2);
}

Monitor *
dirtomon(int dir)
{
	Monitor *m = NULL;

	if (dir > 0) {
		if (!(m = selmon->next))
			m = mons;
	} else if (selmon == mons) {
		for (m = mons; m->next; m = m->next)
			;
	} else {
		for (m = mons; m->next != selmon; m = m->next)
			;
	}
	return m;
}

/* ---- tags / monitors actions ---------------------------------------- */

/* parse a tag argument that is a name or 1-based number; cur is fallback */
int
settag(const char *arg, int cur)
{
	int tag = config_tag(&settings, arg);
	return tag < 0 ? cur : tag;
}

void
viewtag(const char *arg)
{
	int t, cur = selmon->seltag;
	int i;
	Monitor *o, *old = selmon;

	if (!strcmp(arg, "next") || !strcmp(arg, "prev")) {
		int dir = arg[0] == 'n' ? 1 : -1;
		t = cur < 0 ? (dir > 0 ? -1 : tagcount()) : cur;
		for (i = 0; i < tagcount(); i++) {
			t = (t + dir + tagcount()) % tagcount();
			/* land on the next tag that is free or already ours; skip tags
			 * currently shown on another monitor, then claim a free one */
			if (tagstate[t].mon < 0 || tagstate[t].mon == selmon->num) {
				if (tagstate[t].mon < 0)
					tagstate[t].mon = selmon->num;
				selmon->seltag = t;
				goto done;
			}
		}
		return; /* every other tag belongs to another monitor */
	}
	t = settag(arg, cur);
	if (t < 0)
		return;
	o = montag(t);
	if (!o) {
		/* unowned: claim it for the focused monitor */
		tagstate[t].mon = selmon->num;
		selmon->seltag = t;
	} else {
		o->seltag = t;
		selmon = o;
	}
done:
	if (selmon != old)
		warptomon(selmon);
	arrange(NULL);
	focus(NULL);
	drawbars();
}

void
sendmon(Client *c, int tag)
{
	if (c->tag == tag)
		return;
	unfocus(c, 1);
	c->tag = tag;
	focus(NULL);
	arrange(NULL);
}

void
movewintotag(const char *arg)
{
	Client *c = selmon->sel;
	int t, cur;

	if (!c)
		return;
	cur = c->tag;
	if (!strcmp(arg, "next"))
		t = (cur + 1) % tagcount();
	else if (!strcmp(arg, "prev"))
		t = (cur - 1 + tagcount()) % tagcount();
	else
		t = settag(arg, cur);
	if (t == cur)
		return;
	/* ensure target tag has an owner monitor (else it would vanish) */
	if (tagstate[t].mon < 0)
		tagstate[t].mon = selmon->num;
	sendmon(c, t);
	drawbars();
}

void
movetagtomon(const char *arg)
{
	int t = selmon->seltag, target, n;
	Monitor *m, *src = selmon;

	if (t < 0 || !mons->next)
		return;
	n = 0;
	for (m = mons; m; m = m->next)
		n++;
	if (!strcmp(arg, "next"))
		target = (selmon->num + 1) % n;
	else if (!strcmp(arg, "prev"))
		target = (selmon->num - 1 + n) % n;
	else {
		target = atoi(arg);
		if (target < 0 || target >= n)
			return;
	}
	if (target == src->num)
		return;

	tagstate[t].mon = target;
	/* source monitor falls back to another owned tag, or nothing */
	src->seltag = -1;
	{
		int i;
		for (i = 0; i < MAX_TAGS; i++)
			if (tagstate[i].mon == src->num) {
				src->seltag = i;
				break;
			}
	}
	/* show the moved tag on the target and follow it */
	for (m = mons; m && m->num != target; m = m->next)
		;
	if (m) {
		m->seltag = t;
		selmon = m;
		warptomon(selmon);
	}
	arrange(NULL);
	focus(NULL);
	drawbars();
}

/* ---- layout / geometry actions -------------------------------------- */

void
setlayout(const char *arg)
{
	int t = selmon->seltag, l;

	if (t < 0)
		return;
	if (!strcmp(arg, "cols"))
		l = 0;
	else if (!strcmp(arg, "stack"))
		l = 1;
	else if (!strcmp(arg, "float"))
		l = 2;
	else if (!strcmp(arg, "next"))
		l = (tagstate[t].layout + 1) % 3;
	else if (!strcmp(arg, "prev"))
		l = (tagstate[t].layout + 2) % 3;
	else
		return;
	tagstate[t].layout = l;
	arrange(selmon);
	drawbars();
}

void
incnmaster(const char *arg)
{
	int t = selmon->seltag, v;

	if (t < 0)
		return;
	if (arg[0] == '+' || arg[0] == '-')
		v = tagstate[t].nmaster + atoi(arg);
	else
		v = atoi(arg);
	tagstate[t].nmaster = MAX(v, 0);
	arrange(selmon);
}

void
setmfact(const char *arg)
{
	int t = selmon->seltag;
	float f;

	if (t < 0)
		return;
	f = atof(arg);
	if (arg[0] == '+' || arg[0] == '-')
		f += tagstate[t].mfact;
	if (f < 0.05f) f = 0.05f;
	if (f > 0.95f) f = 0.95f;
	tagstate[t].mfact = f;
	arrange(selmon);
}

void
setgap(const char *arg)
{
	int v;

	if (arg[0] == '+' || arg[0] == '-')
		v = gappx_run + atoi(arg);
	else
		v = atoi(arg);
	gappx_run = MAX(v, 0);
	gap_overridden = 1;
	arrange(NULL);
}

static int barvisible = 1;
static int systrayvisible = 1;
static ShellReveal whole_bar = {.progress = 1, .shown = 1};
static float bar_level = 1;
static double bar_tick;

static void
animate_bar(double tick)
{
	int duration = shapeext && !settings.reduced_motion ? settings.duration : 0;
	float previous = bar_level;
	bar_level = shell_reveal(&whole_bar, barvisible, bar_tick ? tick-bar_tick : 0, duration);
	bar_tick = tick;
	if (previous != bar_level)
		drawbars();
	if (previous > 0 && bar_level == 0 && !barvisible) {
		updatebars();
		if (systray)
			painttray((XRectangle){0});
		arrange(NULL);
	}
}

void
togglebar(void)
{
	double tick = shell_seconds();
	animate_bar(tick);
	barvisible = !barvisible;
	animate_bar(tick);
	if (!barvisible) {
		shell_panel_close(&shell, &panel);
		shell_tooltip_hide(&shell, &tooltip);
	}
	updatebars();
	arrange(NULL);
	drawbars();
}

void
toggletraybar(void)
{
	if (!settings.showsystray)
		return;
	systrayvisible = !systrayvisible;
	/* Visibility is animated by paintbar. Keep the XEmbed strip in place
	 * until that transition has clipped it away. */
	if (!systray)
		updatesystray();
	drawbars();
}

/* ---- client state actions ------------------------------------------- */

void
zoom(void)
{
	Client *c = selmon->sel;

	if (!c || c->isfloating || c->isfullscreen)
		return;
	detach(c);
	attach(c); /* move to head -> becomes master */
	arrange(selmon);
	focus(c);
}

static void
drag_event(XEvent *event)
{
	struct pollfd display = {.fd = ConnectionNumber(dpy), .events = POLLIN};
	int timeout = ui_tick();
	while (!XCheckMaskEvent(dpy, MOUSEMASK | ExposureMask | SubstructureRedirectMask, event)) {
		XFlush(dpy);
		if (poll(&display, 1, timeout) < 0 && errno != EINTR)
			die("mwm: poll:");
		timeout = ui_tick();
	}
}

void
movemouse(void)
{
	int x, y, ocx, ocy, nx, ny;
	Client *c;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;

	if (!(c = selmon->sel) || c->isfullscreen)
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
	                 None, cursor[CurMove], CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&x, &y)) {
		XUngrabPointer(dpy, CurrentTime);
		return;
	}
	if (!c->isfloating) {
		c->isfloating = 1;
		arrange(selmon);
	}
	do {
		drag_event(&ev);
		switch (ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;
			nx = ocx + (ev.xmotion.x - x);
			ny = ocy + (ev.xmotion.y - y);
			if ((m = clientmon(c))) {
				if (abs(m->wx - nx) < (int)settings.snap)
					nx = m->wx;
				else if (abs((m->wx + m->ww) - (nx + WIDTH(c))) < (int)settings.snap)
					nx = m->wx + m->ww - WIDTH(c);
				if (abs(m->wy - ny) < (int)settings.snap)
					ny = m->wy;
				else if (abs((m->wy + m->wh) - (ny + HEIGHT(c))) < (int)settings.snap)
					ny = m->wy + m->wh - HEIGHT(c);
			}
			resize(c, nx, ny, c->w, c->h, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	/* dragged onto another monitor: hand the window to that monitor's tag */
	if ((m = recttomon(c->x + c->w / 2, c->y + c->h / 2, 1, 1)) != selmon && m->seltag >= 0) {
		sendmon(c, m->seltag);
		selmon = m;
		focus(c);
	}
}

void
resizemouse(void)
{
	int ocx, ocy, nw, nh;
	Client *c;
	XEvent ev;
	Time lasttime = 0;

	if (!(c = selmon->sel) || c->isfullscreen)
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
	                 None, cursor[CurResize], CurrentTime) != GrabSuccess)
		return;
	if (!c->isfloating) {
		c->isfloating = 1;
		arrange(selmon);
	}
	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1, c->h + c->bw - 1);
	do {
		drag_event(&ev);
		switch (ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;
			nw = MAX(ev.xmotion.x - ocx - 2 * c->bw + 1, 1);
			nh = MAX(ev.xmotion.y - ocy - 2 * c->bw + 1, 1);
			resize(c, c->x, c->y, nw, nh, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1, c->h + c->bw - 1);
	XUngrabPointer(dpy, CurrentTime);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev))
		;
}

void
focuslast(void)
{
	Client *c;

	for (c = fstack; c; c = c->snext)
		if (ISVISIBLE(c) && clientmon(c) == selmon && c != selmon->sel) {
			focus(c);
			restack(selmon);
			warptoclient(c);
			return;
		}
}

void
movewinmon(int dir)
{
	Client *c = selmon->sel;
	Monitor *m;
	int t, i;

	if (!c || !mons->next)
		return;
	m = dirtomon(dir);
	if (!m || m == selmon)
		return;
	t = m->seltag;
	if (t < 0) {
		for (i = 0, t = -1; i < MAX_TAGS; i++)
			if (tagstate[i].mon < 0) { t = i; break; }
		if (t < 0)
			t = 0;
		tagstate[t].mon = m->num;
		m->seltag = t;
	}
	sendmon(c, t);
	drawbars();
}

void
winmove(int dx, int dy)
{
	Client *c = selmon->sel;

	if (!c || c->isfullscreen)
		return;
	if (!c->isfloating) {
		c->isfloating = 1;
		arrange(selmon);
	}
	resize(c, c->x + dx, c->y + dy, c->w, c->h, 1);
	restack(selmon);
}

void
winresize(int dw, int dh)
{
	Client *c = selmon->sel;

	if (!c || c->isfullscreen)
		return;
	if (!c->isfloating) {
		c->isfloating = 1;
		arrange(selmon);
	}
	resize(c, c->x, c->y, MAX(c->w + dw, 1), MAX(c->h + dh, 1), 1);
	restack(selmon);
}

void
wincenter(void)
{
	Client *c = selmon->sel;
	Monitor *m;

	if (!c || c->isfullscreen)
		return;
	if (!c->isfloating) {
		c->isfloating = 1;
		arrange(selmon);
	}
	m = clientmon(c);
	if (!m)
		m = selmon;
	resize(c, m->wx + (m->ww - WIDTH(c)) / 2, m->wy + (m->wh - HEIGHT(c)) / 2, c->w, c->h, 1);
	restack(selmon);
}

void
togglefloating(void)
{
	Client *c = selmon->sel;

	if (!c || c->isfullscreen)
		return;
	c->isfloating = !c->isfloating || c->isfixed;
	if (c->isfloating)
		resize(c, c->x, c->y, c->w, c->h, 0);
	arrange(selmon);
}

void
togglefullscreen(void)
{
	if (selmon->sel)
		setfullscreen(selmon->sel, !selmon->sel->isfullscreen);
}

void
togglesticky(void)
{
	Client *c = selmon->sel;
	if (!c)
		return;
	c->issticky = !c->issticky;
	if (c->issticky)
		c->isfloating = 1;
	arrange(selmon);
	drawbars();
}

void
killfocused(void)
{
	if (!selmon->sel)
		return;
	if (!sendevent(selmon->sel->win, wmatom[WMDelete], NoEventMask, wmatom[WMDelete], CurrentTime, 0, 0, 0)) {
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSetCloseDownMode(dpy, DestroyAll);
		XKillClient(dpy, selmon->sel->win);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
}

void
setfullscreen(Client *c, int fullscreen)
{
	Monitor *m = clientmon(c);

	if (fullscreen && !c->isfullscreen) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
		                PropModeReplace, (unsigned char*)&netatom[NetWMFullscreen], 1);
		c->isfullscreen = 1;
		c->oldstate = c->isfloating;
		c->oldbw = c->bw;
		c->bw = 0;
		c->isfloating = 1;
		if (m) {
			clearround(c->win);
			resizeclient(c, m->mx, m->my, m->mw, m->mh);
		}
		XRaiseWindow(dpy, c->win);
	} else if (!fullscreen && c->isfullscreen) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
		                PropModeReplace, (unsigned char*)0, 0);
		c->isfullscreen = 0;
		c->isfloating = c->oldstate;
		c->bw = c->oldbw;
		c->x = c->oldx;
		c->y = c->oldy;
		c->w = c->oldw;
		c->h = c->oldh;
		resizeclient(c, c->x, c->y, c->w, c->h);
		arrange(clientmon(c));
	}
}

void
seturgent(Client *c, int urg)
{
	XWMHints *wmh;

	c->isurgent = urg;
	if (!(wmh = XGetWMHints(dpy, c->win)))
		return;
	wmh->flags = urg ? (wmh->flags | XUrgencyHint) : (wmh->flags & ~XUrgencyHint);
	XSetWMHints(dpy, c->win, wmh);
	XFree(wmh);
}

/* ---- resize / restack ----------------------------------------------- */

void
resize(Client *c, int x, int y, int w, int h, int interact)
{
	if (applysizehints(c, &x, &y, &w, &h, interact))
		resizeclient(c, x, y, w, h);
}

void
resizeclient(Client *c, int x, int y, int w, int h)
{
	XWindowChanges wc;

	c->oldx = c->x; c->x = wc.x = x;
	c->oldy = c->y; c->y = wc.y = y;
	c->oldw = c->w; c->w = wc.width = w;
	c->oldh = c->h; c->h = wc.height = h;
	wc.border_width = c->bw;
	XConfigureWindow(dpy, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);
	configure(c);
	if (settings.roundcorners && !c->isfullscreen)
		roundwin(c->win, w, h, settings.cornerradius, c->bw);
	else
		clearround(c->win);
	XSync(dpy, False);
}

void
restack(Monitor *m)
{
	Client *c;
	XEvent ev;
	XWindowChanges wc;

	drawbar(m);
	if (!m->sel)
		return;
	if (m->sel->isfloating || m->sel->isfullscreen)
		XRaiseWindow(dpy, m->sel->win);
	/* raise the bar above tiled clients */
	wc.stack_mode = Below;
	wc.sibling = m->barwin;
	for (c = fstack; c; c = c->snext)
		if (!c->isfloating && ISVISIBLE(c) && clientmon(c) == m) {
			XConfigureWindow(dpy, c->win, CWSibling|CWStackMode, &wc);
			wc.sibling = c->win;
		}
	XSync(dpy, False);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev))
		;
}

void
showhide(Client *c)
{
	if (!c)
		return;
	if (ISVISIBLE(c)) {
		XMoveWindow(dpy, c->win, c->x, c->y);
		if ((tagstate[c->tag].layout == 2 || c->isfloating) && !c->isfullscreen)
			resize(c, c->x, c->y, c->w, c->h, 0);
		showhide(c->snext);
	} else {
		showhide(c->snext);
		XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);
	}
}

/* ---- rounded corners (X Shape) -------------------------------------- */

/* fill a rounded rectangle of 1s into a 1-bit shape mask at (x,y) */
void
maskroundrect(Pixmap mask, GC g, int x, int y, int w, int h, int rad)
{
	int d;

	if (w <= 0 || h <= 0)
		return;
	if (rad < 0) rad = 0;
	if (rad * 2 > w) rad = w / 2;
	if (rad * 2 > h) rad = h / 2;
	if (rad == 0) {
		XFillRectangle(dpy, mask, g, x, y, w, h);
		return;
	}
	d = rad * 2;
	XFillRectangle(dpy, mask, g, x + rad, y, w - d, h);
	XFillRectangle(dpy, mask, g, x, y + rad, w, h - d);
	XFillArc(dpy, mask, g, x,         y,         d, d, 0, 360 * 64);
	XFillArc(dpy, mask, g, x + w - d, y,         d, d, 0, 360 * 64);
	XFillArc(dpy, mask, g, x,         y + h - d, d, d, 0, 360 * 64);
	XFillArc(dpy, mask, g, x + w - d, y + h - d, d, d, 0, 360 * 64);
}

void
roundwin(Window win, int w, int h, int rad, int bw)
{
	Pixmap mask;
	GC g;
	int W = w + 2 * bw, H = h + 2 * bw;

	if (!shapeext || rad <= 0 || W <= 0 || H <= 0)
		return;
	/* The ShapeBounding region must include the border, which sits OUTSIDE the
	 * w*h content (at window-relative offset -bw). A content-only mask shapes
	 * the border away entirely, so it never renders. Build a mask covering
	 * content+border and combine it at offset -bw so it starts at the border's
	 * outer edge; round the outer corners by rad+bw to keep the border even. */
	mask = XCreatePixmap(dpy, win, W, H, 1);
	g = XCreateGC(dpy, mask, 0, NULL);
	XSetForeground(dpy, g, 0);
	XFillRectangle(dpy, mask, g, 0, 0, W, H);
	XSetForeground(dpy, g, 1);
	maskroundrect(mask, g, 0, 0, W, H, rad + bw);
	XShapeCombineMask(dpy, win, ShapeBounding, -bw, -bw, mask, ShapeSet);
	XFreeGC(dpy, g);
	XFreePixmap(dpy, mask);
}

void
clearround(Window w)
{
	if (shapeext)
		XShapeCombineMask(dpy, w, ShapeBounding, 0, 0, None, ShapeSet);
}

/* ---- bar presentation ------------------------------------------------ */

static Monitor *
traymonitor(void)
{
	if (settings.systraymon < 0)
		return selmon ? selmon : mons;
	for (Monitor *m = mons; m; m = m->next)
		if (m->num == settings.systraymon)
			return m;
	return mons;
}

void
drawbar(Monitor *m)
{
	if (m)
		m->bardirty = 1;
}

static void
paintbar(Monitor *m, time_t now, double tick)
{
	m->bardirty = 0;
	if (!bar_level)
		return;
	int owns_tray = systray && systray->monitor == m->num;
	int tray_width = owns_tray ? systray->width : 0;
	int inset = shell_bar_inset(&settings);
	int distance = settings.topbar ? m->by+bh-m->my : m->my+m->mh-m->by;
	int slide = roundf((settings.topbar ? -1 : 1) * distance * (1-bar_level));
	ShellBarModel state = {
		.w = m->mw, .h = bh, .slide = slide, .map = !m->barmapped,
		.pad = inset,
		.clip_top = MAX(0, m->my-m->by), .clip_bottom = MIN(bh, m->my+m->mh-m->by),
		.tray_width = tray_width, .tray_shown = tray_width && systrayvisible && settings.showsystray,
		.tag = m->seltag, .layout = m->seltag < 0 ? -1 : tagstate[m->seltag].layout,
		.title = m->sel ? m->sel->name : "", .status = stext, .system = &system_stats,
		.controls = &controls, .hidden = hidden_segments,
	};
	int urgency[MAX_TAGS] = {0}, occupancy[MAX_TAGS] = {0};
	for (Client *c = clients; c; c = c->next) {
		occupancy[c->tag] = 1;
		urgency[c->tag] |= c->isurgent;
	}
	for (int i = 0; i < MAX_TAGS; ++i) {
		if (tagstate[i].mon != m->num || (i != m->seltag && !occupancy[i]))
			continue;
		ShellTag *tag = &state.tags[state.ntags++];
		tag->id = i;
		tag->urgent = urgency[i];
		taglabel(i, tag->label, sizeof tag->label);
	}
	if (!shell_bar_draw(&shell, &shellfont, theme_revision, &settings, &m->bar,
	                    m->barwin, &state, &timer, now, tick))
		return;
	m->barmapped = 1;
	if (tooltip.mapped && shell_bar_metric(&m->bar) != MetricNone)
		tooltip.dirty = 1;
	if (owns_tray) {
		int x = m->mx + m->bar.tray_x;
		int y = m->by + inset + slide + settings.systrayvertpad;
		int top = MAX(0, m->my-y);
		int bottom = MIN(systray->height, m->my+m->mh-y);
		if (x != systray->x || y != systray->y) {
			systray->x = x;
			systray->y = y;
			XMoveWindow(dpy, systray->win, x, y);
		}
		painttray((XRectangle){m->bar.tray_clip_x, top, m->bar.tray_clip_width, MAX(0, bottom-top)});
	}
}

void
drawbars(void)
{
	Monitor *m;
	for (m = mons; m; m = m->next)
		drawbar(m);
}

void
updatebarpos(Monitor *m)
{
	m->wy = m->my;
	m->wh = m->mh;
	int inset = shell_bar_inset(&settings);
	int reserved = bh - 2*inset + settings.barvmargin;
	m->by = settings.topbar ? m->my + settings.barvmargin - inset
	                         : m->my + m->mh - reserved - inset;
	if (barvisible || bar_level > 0) {
		m->wh = MAX(1, m->wh - reserved);
		if (settings.topbar)
			m->wy = m->my + reserved;
	}
}

/* ---- events ---------------------------------------------------------- */

void
buttonpress(XEvent *e)
{
	Client *c;
	Monitor *m;
	XButtonPressedEvent *ev = &e->xbutton;

	m = wintomon(ev->window);
	if (m && m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	if ((c = wintoclient(ev->window))) {
		focus(c);
		restack(selmon);
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		if (ev->state & settings.movemod) {
			if (ev->button == Button1)
				movemouse();
			else if (ev->button == Button3)
				resizemouse();
		}
		return;
	}
	/* forward button press on the tray window to the icon under the cursor */
	if (systray && ev->window == systray->win) {
		Client *i;
		int off = 0;
		for (i = systray->icons; i; i = i->next) {
			if (ev->x >= off && ev->x < off + (int)i->w
			    && ev->y >= 0 && ev->y < i->h) {
				XButtonEvent fwd = *ev;
				fwd.window = i->win;
				fwd.x -= off;
				fwd.subwindow = None;
				XSendEvent(dpy, i->win, True, ButtonPressMask, (XEvent *)&fwd);
				XSync(dpy, False);
				return;
			}
			off += i->w + settings.systrayspacing;
		}
		return;
	}
	if (m && ev->window == m->barwin && barvisible) {
		ShellHit hit = shell_hit(m->bar.hits, m->bar.nhits, ev->x, ev->y);
		selmon = m;
		int control = shell_panel_control(hit.action);
		if (control >= 0 && (ev->button == Button4 || ev->button == Button5)) {
			adjust_control(control, ev->button == Button4 ? 1 : -1);
		} else if ((ev->button == Button4 || ev->button == Button5) &&
		    (hit.action == ActTag || hit.action == ActLayout)) {
			viewtag(ev->button == Button4 ? "prev" : "next");
		} else if (hit.action == ActLayout && (ev->button == Button1 || ev->button == Button3)) {
			setlayout(ev->button == Button3 ? "prev" : "next");
		} else if (hit.action == ActTag && ev->button == Button1) {
			char tag[16];
			snprintf(tag, sizeof tag, "%d", (int)hit.value + 1);
			viewtag(tag);
		} else if (ev->button == Button1 && panel_action(hit.action)) {
			openpanel(m, hit.action, m->mx + hit.rect.x + hit.rect.w / 2, ev->time);
		}
	}
}

void
enternotify(XEvent *e)
{
	Client *c;
	Monitor *m;
	XCrossingEvent *ev = &e->xcrossing;

	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != root)
		return;
	m = wintomon(ev->window);
	if (m && ev->window == m->barwin) {
		m->bar.hover_x = ev->x;
		m->bar.hover_y = ev->y;
		drawbar(m);
	}
	if (!settings.focusfollowsmouse)
		return;
	c = wintoclient(ev->window);
	if (c)
		m = clientmon(c);
	if (m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m ? m : selmon;
	}
	if (c && c != selmon->sel)
		focus(c);
}

void
motionnotify(XEvent *e)
{
	static int last = -1;
	Monitor *m;
	XMotionEvent *ev = &e->xmotion;

	if (ev->window != root) {
		if ((m = wintomon(ev->window)) && ev->window == m->barwin) {
			m->bar.hover_x = ev->x;
			m->bar.hover_y = ev->y;
			drawbar(m);
		}
		return;
	}
	m = recttomon(ev->x_root, ev->y_root, 1, 1);
	if (m && m->num != last && last >= 0) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	last = m ? m->num : -1;
}

static void
leavebar(XEvent *e)
{
	Monitor *m = wintomon(e->xcrossing.window);
	if (m && e->xcrossing.window == m->barwin) {
		m->bar.hover_x = m->bar.hover_y = -1;
		drawbar(m);
	}
}

void
expose(XEvent *e)
{
	Monitor *m;
	XExposeEvent *ev = &e->xexpose;

	if (ev->count)
		return;
	if (ev->window == tooltip.win) {
		tooltip.dirty = 1;
		return;
	}
	if ((m = wintomon(ev->window)))
		drawbar(m);
}

void
configurenotify(XEvent *e)
{
	Monitor *m;
	Client *c;
	XConfigureEvent *ev = &e->xconfigure;

	if (ev->window == root) {
		sw = ev->width;
		sh = ev->height;
		if (updategeom(0)) {
			for (m = mons; m; m = m->next) {
				for (c = clients; c; c = c->next)
					if (c->isfullscreen && clientmon(c) == m)
						resizeclient(c, m->mx, m->my, m->mw, m->mh);
				XMoveResizeWindow(dpy, m->barwin, m->mx, m->by, m->mw, bh);
			}
			focus(NULL);
			arrange(NULL);
		}
	}
}

void
configurerequest(XEvent *e)
{
	Client *c;
	Monitor *m;
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	XWindowChanges wc;

	if ((c = wintoclient(ev->window))) {
		if (ev->value_mask & CWBorderWidth)
			c->bw = ev->border_width;
		else if (c->isfloating) {
			m = clientmon(c);
			if (ev->value_mask & CWX) { c->oldx = c->x; c->x = (m ? m->mx : 0) + ev->x; }
			if (ev->value_mask & CWY) { c->oldy = c->y; c->y = (m ? m->my : 0) + ev->y; }
			if (ev->value_mask & CWWidth) { c->oldw = c->w; c->w = ev->width; }
			if (ev->value_mask & CWHeight) { c->oldh = c->h; c->h = ev->height; }
			if (ISVISIBLE(c))
				XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
		} else
			configure(c);
	} else {
		wc.x = ev->x;
		wc.y = ev->y;
		wc.width = ev->width;
		wc.height = ev->height;
		wc.border_width = ev->border_width;
		wc.sibling = ev->above;
		wc.stack_mode = ev->detail;
		XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
	}
	XSync(dpy, False);
}

void
configure(Client *c)
{
	XConfigureEvent ce;

	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = c->win;
	ce.window = c->win;
	ce.x = c->x;
	ce.y = c->y;
	ce.width = c->w;
	ce.height = c->h;
	ce.border_width = c->bw;
	ce.above = None;
	ce.override_redirect = False;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

void
destroynotify(XEvent *e)
{
	Client *c;
	XDestroyWindowEvent *ev = &e->xdestroywindow;

	if ((c = wintoclient(ev->window)))
		unmanage(c, 1);
	else if ((c = wintosystrayicon(ev->window))) {
		removesystrayicon(c);
		updatesystray();
	}
}

void
unmapnotify(XEvent *e)
{
	Client *c;
	XUnmapEvent *ev = &e->xunmap;

	if ((c = wintoclient(ev->window))) {
		if (ev->send_event)
			setclientstate(c, WithdrawnState);
		else
			unmanage(c, 0);
	} else if (ev->event != root && (c = wintosystrayicon(ev->window))) {
		/* KDE/GTK panel icons sometimes unmap themselves; re-embed */
		removesystrayicon(c);
		updatesystray();
	}
}

void
maprequest(XEvent *e)
{
	static XWindowAttributes wa;
	XMapRequestEvent *ev = &e->xmaprequest;
	Client *i;

	if ((i = wintosystrayicon(ev->window))) {
		sendevent(i->win, xatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_WINDOW_ACTIVATE, 0, systray->win, XEMBED_EMBEDDED_VERSION);
		updatesystray();
		return;
	}
	if (!XGetWindowAttributes(dpy, ev->window, &wa) || wa.override_redirect)
		return;
	if (!wintoclient(ev->window))
		manage(ev->window, &wa);
}

void
mappingnotify(XEvent *e)
{
	XMappingEvent *ev = &e->xmapping;
	XRefreshKeyboardMapping(ev);
}

void
propertynotify(XEvent *e)
{
	Client *c;
	Window trans;
	XPropertyEvent *ev = &e->xproperty;

	if ((c = wintosystrayicon(ev->window))) {
		if (ev->atom == XA_WM_NORMAL_HINTS) {
			updatesizehints(c);
			updatesystrayicongeom(c, c->w, c->h);
		} else
			updatesystrayiconstate(c, ev);
		updatesystray();
	}
	if ((ev->window == root) && (ev->atom == XA_WM_NAME)) {
		updatestatus();
	} else if (ev->state == PropertyDelete) {
		return;
	} else if ((c = wintoclient(ev->window))) {
		switch (ev->atom) {
		default: break;
		case XA_WM_TRANSIENT_FOR:
			if (!c->isfloating && (XGetTransientForHint(dpy, c->win, &trans)) &&
			    (c->isfloating = (wintoclient(trans)) != NULL))
				arrange(clientmon(c));
			break;
		case XA_WM_NORMAL_HINTS:
			c->hintsvalid = 0;
			break;
		case XA_WM_HINTS:
			updatewmhints(c);
			drawbars();
			break;
		}
		if (ev->atom == XA_WM_NAME || ev->atom == netatom[NetWMName]) {
			updatetitle(c);
			if (clientmon(c) && c == clientmon(c)->sel)
				drawbar(clientmon(c));
		}
		if (ev->atom == netatom[NetWMWindowType])
			updatewindowtype(c);
	}
}

void
resizerequest(XEvent *e)
{
	XResizeRequestEvent *ev = &e->xresizerequest;
	Client *i;

	if ((i = wintosystrayicon(ev->window))) {
		updatesystrayicongeom(i, ev->width, ev->height);
		updatesystray();
	}
}

void
clientmessage(XEvent *e)
{
	XWindowAttributes wa;
	XClientMessageEvent *cme = &e->xclient;
	Client *c;

	/* mwmc IPC */
	if (cme->message_type == mwmatom[MwmMsg]) {
		handleipc((Window)cme->data.l[0]);
		return;
	}

	/* system tray dock requests */
	if (settings.showsystray && systray && cme->window == systray->win && cme->message_type == netatom[NetSystemTrayOP]) {
		if (cme->data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
			Window icon = (Window)cme->data.l[2];
			if (!icon || wintosystrayicon(icon) || !XGetWindowAttributes(dpy, icon, &wa))
				return;
			dockicon(icon, &wa, &systray->icons);
			updatesystray();
		}
		return;
	}

	if (!(c = wintoclient(cme->window)))
		return;
	if (cme->message_type == netatom[NetWMState]) {
		if ((Atom)cme->data.l[1] == netatom[NetWMFullscreen] || (Atom)cme->data.l[2] == netatom[NetWMFullscreen])
			setfullscreen(c, (cme->data.l[0] == 1 /* _NET_WM_STATE_ADD */
			    || (cme->data.l[0] == 2 /* _NET_WM_STATE_TOGGLE */ && !c->isfullscreen)));
		else if ((Atom)cme->data.l[1] == netatom[NetWMStateDemandsAttention] || (Atom)cme->data.l[2] == netatom[NetWMStateDemandsAttention]) {
			if (c != selmon->sel && !c->isurgent)
				seturgent(c, 1);
		}
	} else if (cme->message_type == netatom[NetActiveWindow]) {
		if (c != selmon->sel && !c->isurgent)
			seturgent(c, 1);
	}
}

/* ---- IPC dispatch ---------------------------------------------------- */

void
spawnreply(Window w, const char *s)
{
	XChangeProperty(dpy, w, mwmatom[MwmReply], xa_utf8, 8, PropModeReplace,
	                (const unsigned char *)s, strlen(s));
	XSync(dpy, False);
}

void
handleipc(Window requestor)
{
	Atom type;
	int fmt, argc = 0;
	unsigned long nitems, after;
	unsigned char *data = NULL;
	char *argv[32];
	char *p, *end;
	char *reply;
	const size_t rsz = 1 << 16;

	if (XGetWindowProperty(dpy, requestor, mwmatom[MwmCmd], 0, (1 << 16), False,
	                       AnyPropertyType, &type, &fmt, &nitems, &after, &data) != Success || !data) {
		spawnreply(requestor, "error: no command");
		return;
	}
	/* split the NUL-separated argv */
	p = (char *)data;
	end = p + nitems;
	while (p < end && argc < (int)LENGTH(argv)) {
		argv[argc++] = p;
		p += strlen(p) + 1;
	}
	reply = ecalloc(1, rsz);
	if (argc == 1 && !strcmp(argv[0], "restart")) {
		/* The replacement sends the success reply once it can handle commands. */
		XFree(data);
		XDeleteProperty(dpy, requestor, mwmatom[MwmCmd]);
		const char *error = restart(requestor);
		snprintf(reply, rsz, "error: %s", error);
		spawnreply(requestor, reply);
		free(reply);
		return;
	} else if (argc > 0)
		dispatchcmd(argc, argv, reply, rsz);
	else
		snprintf(reply, rsz, "error: empty command");
	spawnreply(requestor, reply);
	free(reply);
	XFree(data);
	XDeleteProperty(dpy, requestor, mwmatom[MwmCmd]);
}

#define ARG(i)   ((i) < argc ? argv[i] : "")
#define EQ(a,b)  (!strcmp((a),(b)))

void
dispatchcmd(int argc, char **argv, char *reply, size_t rsz)
{
	const char *verb = argv[0];
	const char *noun = ARG(1);
	size_t n = 0;

	if (EQ(verb, "focus")) {
		if (EQ(noun, "next")) focusstack(+1);
		else if (EQ(noun, "prev")) focusstack(-1);
		else if (EQ(noun, "last")) focuslast();
		else if (EQ(noun, "win")) {
			if (EQ(ARG(2), "next")) focusstack(+1);
			else if (EQ(ARG(2), "prev")) focusstack(-1);
			else if (EQ(ARG(2), "last")) focuslast();
			else if (argc > 2) focusclientbyclass(argv[2]);
			else { snprintf(reply, rsz, "error: focus win next|prev|last|<class>"); return; }
		} else if (EQ(noun, "tag")) {
			if (argc > 2) viewtag(argv[2]);
			else { snprintf(reply, rsz, "error: focus tag next|prev|<name>"); return; }
		} else if (EQ(noun, "mon")) {
			if (argc > 2) focusmon(argv[2]);
			else { snprintf(reply, rsz, "error: focus mon next|prev|<n>"); return; }
		} else { snprintf(reply, rsz, "error: focus next|prev|win|tag|mon ..."); return; }
	} else if (EQ(verb, "move")) {
		if (EQ(noun, "win")) {
			if (EQ(ARG(2), "master")) zoom();
			else if (EQ(ARG(2), "mon")) {
				if (EQ(ARG(3), "next")) movewinmon(+1);
				else if (EQ(ARG(3), "prev")) movewinmon(-1);
				else { snprintf(reply, rsz, "error: move win mon next|prev"); return; }
			}
			else if (argc > 2) movewintotag(argv[2]);
			else { snprintf(reply, rsz, "error: move win next|prev|master|mon|<tag>"); return; }
		} else if (EQ(noun, "tag")) {
			if (argc > 2) movetagtomon(argv[2]);
			else { snprintf(reply, rsz, "error: move tag next|prev|<mon>"); return; }
		} else { snprintf(reply, rsz, "error: move win|tag ..."); return; }
	} else if (EQ(verb, "swap")) {
		/* swap focused window forward/back in the stack order */
		Client *c = selmon->sel, *nb = NULL;
		if (!c || c->isfloating) { snprintf(reply, rsz, "error: no tiled window"); return; }
		if (EQ(noun, "next")) {
			nb = nexttiled(c->next, c->tag);
			if (nb) { detach(c); attachafter(c, nb); }
		} else if (EQ(noun, "prev")) {
			Client *i;
			for (i = clients; i && i != c; i = i->next)
				if (i->tag == c->tag && !i->isfloating && ISVISIBLE(i)) nb = i;
			if (nb) {
				Client *before = NULL, *j;
				for (j = clients; j && j != nb; j = j->next) before = j;
				detach(c); attachafter(c, before);
			}
		} else { snprintf(reply, rsz, "error: swap next|prev"); return; }
		arrange(selmon); focus(c);
	} else if (EQ(verb, "layout")) {
		if (argc > 1) setlayout(noun);
		else { snprintf(reply, rsz, "error: layout cols|stack|float|next|prev"); return; }
	} else if (EQ(verb, "master")) {
		if (EQ(noun, "ratio")) {
			if (argc > 2) setmfact(argv[2]);
			else { snprintf(reply, rsz, "error: master ratio +0.05|-0.05|<f>"); return; }
		} else if (argc > 1) incnmaster(noun);
		else { snprintf(reply, rsz, "error: master +1|-1|<n>|ratio ..."); return; }
	} else if (EQ(verb, "gap")) {
		if (argc > 1) setgap(noun);
		else { snprintf(reply, rsz, "error: gap +5|-5|<px>"); return; }
	} else if (EQ(verb, "bar")) {
		if (EQ(noun, "toggle")) togglebar();
		else if (EQ(noun, "show")) { if (!barvisible) togglebar(); }
		else if (EQ(noun, "hide")) { if (barvisible) togglebar(); }
		else { snprintf(reply, rsz, "error: bar toggle|show|hide"); return; }
	} else if (EQ(verb, "traybar")) {
		if (EQ(noun, "toggle")) toggletraybar();
		else if (EQ(noun, "show")) { if (!systrayvisible) toggletraybar(); }
		else if (EQ(noun, "hide")) { if (systrayvisible) toggletraybar(); }
		else { snprintf(reply, rsz, "error: traybar toggle|show|hide"); return; }
	} else if (EQ(verb, "segment")) {
		if (argc == 2 && EQ(noun, "list")) {
			size_t n = 0;
			for (int i = 0; i < settings.nsegments; ++i)
				n += snprintf(reply+n, rsz-n, "%s %s\n", settings.segments[i].name,
				              hidden_segments & (1u << i) ? "hidden" : "shown");
			return;
		}
		int index = argc == 3 ? config_segment(&settings, ARG(2)) : -1;
		if (index < 0 || !(EQ(noun, "show") || EQ(noun, "hide") || EQ(noun, "toggle"))) {
			snprintf(reply, rsz, "error: segment list | show|hide|toggle <configured segment>");
			return;
		}
		unsigned bit = 1u << index;
		if (EQ(noun, "toggle")) hidden_segments ^= bit;
		if (EQ(noun, "show")) hidden_segments &= ~bit;
		if (EQ(noun, "hide")) hidden_segments |= bit;
		if (panel.win && (hidden_segments & bit)) {
			enum BarItem item = panel.kind == ActCalendar ? BarDate : panel.kind == ActClock ? BarTime
			                    : panel.kind == ActPalette ? BarTitle : BarBattery + shell_panel_control(panel.kind);
			const BarPlacement *group = &settings.segments[index];
			for (int i = 0; i < group->count; ++i)
				if (group->items[i] == item)
					shell_panel_close(&shell, &panel);
		}
		drawbars();
	} else if (EQ(verb, "win")) {
		if (EQ(noun, "close")) killfocused();
		else if (EQ(noun, "move")) {
			if (argc > 3) winmove(atoi(argv[2]), atoi(argv[3]));
			else { snprintf(reply, rsz, "error: win move <dx> <dy>"); return; }
		} else if (EQ(noun, "resize")) {
			if (argc > 3) winresize(atoi(argv[2]), atoi(argv[3]));
			else { snprintf(reply, rsz, "error: win resize <dw> <dh>"); return; }
		} else if (EQ(noun, "center")) {
			wincenter();
		} else if (EQ(noun, "tag")) {
			if (argc > 2) movewintotag(argv[2]);
			else { snprintf(reply, rsz, "error: win tag <name>"); return; }
		} else if (EQ(noun, "toggle")) {
			if (EQ(ARG(2), "float")) togglefloating();
			else if (EQ(ARG(2), "full")) togglefullscreen();
			else if (EQ(ARG(2), "sticky")) togglesticky();
			else { snprintf(reply, rsz, "error: win toggle float|full|sticky"); return; }
		} else { snprintf(reply, rsz, "error: win close|tag|toggle ..."); return; }
	} else if (EQ(verb, "kill")) {
		killfocused();
	} else if (EQ(verb, "palette")) {
		if (!panel.win || panel.kind != ActPalette)
			openpanel(selmon, ActPalette, 0, CurrentTime);
		if (!panel.win || panel.kind != ActPalette) {
			snprintf(reply, rsz, "error: cannot open palette");
			return;
		}
	} else if (EQ(verb, "reload")) {
		const char *err = reloadstyle();
		if (err) {
			snprintf(reply, rsz, "error: %s", err);
			return;
		}
	} else if (EQ(verb, "restart")) {
		snprintf(reply, rsz, "error: usage: mwmc restart");
		return;
	} else if (EQ(verb, "quit")) {
		running = 0;
	} else if (EQ(verb, "version")) {
		snprintf(reply, rsz, "mwm-%s", VERSION);
		return;
	} else if (EQ(verb, "query")) {
		Monitor *m;
		Client *c;
		char lbl[64];
		int i;
		if (EQ(noun, "tags")) {
			for (i = 0; i < tagcount() && n < rsz - 128; i++) {
				if (tagstate[i].mon < 0 && !occupied(i))
					continue;
				taglabel(i, lbl, sizeof lbl);
				n += snprintf(reply + n, rsz - n, "tag %d %s mon=%d sel=%d occ=%d layout=%s\n",
				              i, lbl, tagstate[i].mon,
				              (montag(i) && montag(i)->seltag == i),
				              occupied(i), layoutsym[tagstate[i].layout]);
			}
		} else if (EQ(noun, "windows")) {
			for (c = clients; c && n < rsz - 320; c = c->next)
				n += snprintf(reply + n, rsz - n, "win 0x%lx tag=%d class=%s float=%d full=%d sticky=%d focused=%d \"%s\"\n",
				              c->win, c->tag, c->class, c->isfloating, c->isfullscreen,
				              c->issticky, c == selmon->sel, c->name);
		} else if (EQ(noun, "monitors")) {
			for (m = mons; m && n < rsz - 128; m = m->next)
				n += snprintf(reply + n, rsz - n, "mon %d %dx%d+%d+%d seltag=%d sel=%d\n",
				              m->num, m->mw, m->mh, m->mx, m->my, m->seltag, m == selmon);
		} else if (EQ(noun, "layout")) {
			int t = selmon->seltag;
			snprintf(reply, rsz, "%s\n", t >= 0 ? layoutsym[tagstate[t].layout] : "-");
		} else if (EQ(noun, "state")) {
			int t = selmon->seltag;
			taglabel(t >= 0 ? t : 0, lbl, sizeof lbl);
			snprintf(reply, rsz, "mon=%d tag=%s layout=%s nmaster=%d mfact=%.2f gap=%d sel=%s\n",
			         selmon->num, t >= 0 ? lbl : "-", t >= 0 ? layoutsym[tagstate[t].layout] : "-",
			         t >= 0 ? tagstate[t].nmaster : 0, t >= 0 ? tagstate[t].mfact : 0.0,
			         gappx_run, selmon->sel ? selmon->sel->name : "");
		} else {
			snprintf(reply, rsz, "error: query tags|windows|monitors|layout|state");
		}
		return;
	} else if (EQ(verb, "help") || EQ(verb, "-h") || EQ(verb, "--help")) {
		snprintf(reply, rsz,
		    "mwm commands (window <= tag <= monitor):\n"
		    "  focus next|prev|last | focus win next|prev|last|<class>\n"
		    "  focus tag next|prev|<name> | focus mon next|prev|<n>\n"
		    "  move win next|prev|master|<tag> | move win mon next|prev\n"
		    "  move tag next|prev|<mon> | swap next|prev\n"
		    "  layout cols|stack|float|next|prev\n"
		    "  master +1|-1|<n> | master ratio +0.05|-0.05|<f>\n"
		    "  gap +5|-5|<px> | bar toggle|show|hide\n"
		    "  traybar toggle|show|hide\n"
		    "  segment list | show|hide|toggle <name>\n"
		    "  win close | win tag <name> | win toggle float|full|sticky\n"
		    "  win move <dx> <dy> | win resize <dw> <dh> | win center\n"
		    "  query tags|windows|monitors|layout|state\n"
		    "  palette | reload (config.ini + Xresources) | restart | quit | version\n"
		    "  completions bash|zsh (mwmc client-side; print shell completion)\n");
		return;
	} else {
		snprintf(reply, rsz, "error: unknown command '%s' (try: mwmc help)", verb);
		return;
	}
	drawbars();
}

/* ---- manage / unmanage ---------------------------------------------- */

void
manage(Window w, XWindowAttributes *wa)
{
	Client *c, *t = NULL;
	Window trans = None;
	XWindowChanges wc;
	Monitor *m;

	c = ecalloc(1, sizeof(Client));
	c->win = w;
	c->x = c->oldx = wa->x;
	c->y = c->oldy = wa->y;
	c->w = c->oldw = wa->width;
	c->h = c->oldh = wa->height;
	c->oldbw = wa->border_width;

	updatetitle(c);
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
		c->tag = t->tag;
		c->isfloating = 1;
	} else {
		applyrules(c);
	}

	m = clientmon(c);
	if (!m)
		m = selmon;

	if (c->x + WIDTH(c) > m->mx + m->mw)
		c->x = m->mx + m->mw - WIDTH(c);
	if (c->y + HEIGHT(c) > m->my + m->mh)
		c->y = m->my + m->mh - HEIGHT(c);
	c->x = MAX(c->x, m->mx);
	c->y = MAX(c->y, m->my);
	c->bw = settings.borderpx;

	wc.border_width = c->bw;
	XConfigureWindow(dpy, w, CWBorderWidth, &wc);
	XSetWindowBorder(dpy, w, shell_rgb(settings.palette[SchemeNorm][ColBorder]) | 0xff000000UL);
	configure(c);
	updatewindowtype(c);
	updatesizehints(c);
	updatewmhints(c);

	/* center floating windows */
	if (c->isfloating) {
		c->x = m->wx + (m->ww - WIDTH(c)) / 2;
		c->y = m->wy + (m->wh - HEIGHT(c)) / 2;
	}

	XSelectInput(dpy, w, EnterWindowMask | FocusChangeMask | PropertyChangeMask | StructureNotifyMask);
	grabbuttons(c, 0);
	XRaiseWindow(dpy, c->win);
	attach(c);
	attachstack(c);
	XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32, PropModeAppend,
	                (unsigned char *) &(c->win), 1);
	XMoveResizeWindow(dpy, c->win, c->x + 2 * sw, c->y, c->w, c->h);
	setclientstate(c, NormalState);
	if (clientmon(c) == selmon)
		unfocus(selmon->sel, 0);
	if (clientmon(c))
		clientmon(c)->sel = c;
	arrange(clientmon(c));
	XMapWindow(dpy, c->win);
	focus(NULL);
}

void
unmanage(Client *c, int destroyed)
{
	Monitor *m = clientmon(c);
	XWindowChanges wc;

	detach(c);
	detachstack(c);
	if (!destroyed) {
		wc.border_width = c->oldbw;
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSelectInput(dpy, c->win, NoEventMask);
		XConfigureWindow(dpy, c->win, CWBorderWidth, &wc);
		setclientstate(c, WithdrawnState);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
	free(c);
	focus(NULL);
	updateclientlist();
	arrange(m);
}

/* ---- properties ------------------------------------------------------ */

void
updatetitle(Client *c)
{
	if (!gettextprop(c->win, netatom[NetWMName], c->name, sizeof c->name))
		gettextprop(c->win, XA_WM_NAME, c->name, sizeof c->name);
	if (c->name[0] == '\0')
		strcpy(c->name, broken);
}

void
updatewindowtype(Client *c)
{
	Atom state = getatomprop(c, netatom[NetWMState]);
	Atom wtype = getatomprop(c, netatom[NetWMWindowType]);

	if (state == netatom[NetWMFullscreen])
		setfullscreen(c, 1);
	if (wtype == netatom[NetWMWindowTypeDialog])
		c->isfloating = 1;
}

void
updatewmhints(Client *c)
{
	XWMHints *wmh;

	if ((wmh = XGetWMHints(dpy, c->win))) {
		if (c == selmon->sel && wmh->flags & XUrgencyHint) {
			wmh->flags &= ~XUrgencyHint;
			XSetWMHints(dpy, c->win, wmh);
		} else
			c->isurgent = (wmh->flags & XUrgencyHint) ? 1 : 0;
		if (wmh->flags & InputHint)
			c->neverfocus = !wmh->input;
		else
			c->neverfocus = 0;
		XFree(wmh);
	}
}

void
updatesizehints(Client *c)
{
	long msize;
	XSizeHints size;

	if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
		size.flags = PSize;
	if (size.flags & PBaseSize) {
		c->basew = size.base_width;
		c->baseh = size.base_height;
	} else if (size.flags & PMinSize) {
		c->basew = size.min_width;
		c->baseh = size.min_height;
	} else
		c->basew = c->baseh = 0;
	if (size.flags & PResizeInc) {
		c->incw = size.width_inc;
		c->inch = size.height_inc;
	} else
		c->incw = c->inch = 0;
	if (size.flags & PMaxSize) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else
		c->maxw = c->maxh = 0;
	if (size.flags & PMinSize) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else
		c->minw = c->minh = 0;
	if (size.flags & PAspect) {
		c->mina = (float)size.min_aspect.y / size.min_aspect.x;
		c->maxa = (float)size.max_aspect.x / size.max_aspect.y;
	} else
		c->maxa = c->mina = 0.0;
	c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
	c->hintsvalid = 1;
}

void
updatestatus(void)
{
	if (!gettextprop(root, XA_WM_NAME, stext, sizeof(stext)))
		stext[0] = 0;
	drawbars();
}

void
updateclientlist(void)
{
	Client *c;

	XDeleteProperty(dpy, root, netatom[NetClientList]);
	for (c = clients; c; c = c->next)
		XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32,
		                PropModeAppend, (unsigned char *) &(c->win), 1);
}

/* ---- system tray ----------------------------------------------------- */

/* Clip the XEmbed strip to the tray segment as it opens or closes.
 * Only its container is unmapped. The icon windows stay docked and mapped. */
static void
painttray(XRectangle clip)
{
	if (!clip.width || !clip.height ||
	    (!shapeext && (clip.width != systray->width || clip.height != systray->height))) {
		if (systray->mapped)
			XUnmapWindow(dpy, systray->win);
		systray->mapped = 0;
		return;
	}
	if (shapeext && memcmp(&clip, &systray->clip, sizeof clip)) {
		if (clip.width == systray->width && clip.height == systray->height)
			XShapeCombineMask(dpy, systray->win, ShapeBounding, 0, 0, None, ShapeSet);
		else
			XShapeCombineRectangles(dpy, systray->win, ShapeBounding, 0, 0, &clip, 1, ShapeSet, Unsorted);
		systray->clip = clip;
	}
	if (!systray->mapped) {
		XMapRaised(dpy, systray->win);
		systray->mapped = 1;
	}
}

void
removesystrayicon(Client *i)
{
	Client **ii;

	if (!i)
		return;
	for (ii = &systray->icons; *ii && *ii != i; ii = &(*ii)->next)
		;
	if (*ii)
		*ii = i->next;
	free(i);
}

static void
dockicon(Window icon, const XWindowAttributes *wa, Client **link)
{
	Client *c = ecalloc(1, sizeof(Client));
	c->win = icon;
	c->depth = wa->depth;
	c->tag = -1;
	c->next = *link;
	*link = c;
	c->oldbw = wa->border_width;
	c->isfloating = 1;
	updatesizehints(c);
	updatesystrayicongeom(c, wa->width, wa->height);
	XAddToSaveSet(dpy, icon);
	XReparentWindow(dpy, icon, systray->win, 0, 0);
	XSelectInput(dpy, icon, wa->your_event_mask | StructureNotifyMask | PropertyChangeMask | ResizeRedirectMask);
	XSetWindowBorderWidth(dpy, icon, 0);
	if (c->depth == DefaultDepth(dpy, screen))
		XSetWindowBackgroundPixmap(dpy, icon, ParentRelative);
	sendevent(icon, xatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_EMBEDDED_NOTIFY, 0,
	          systray->win, XEMBED_EMBEDDED_VERSION);
	setclientstate(c, NormalState);
}

void
updatesystrayicongeom(Client *i, int w, int h)
{
	int ih = MAX(1, bh - 2 * (shell_bar_inset(&settings) + settings.systrayvertpad));

	if (!i)
		return;
	i->h = ih;
	if (w == h || h <= 0)
		i->w = ih;
	else if (h == ih)
		i->w = w;
	else
		i->w = (int)((float)ih * ((float)w / (float)h));
	if (i->w <= 0)
		i->w = ih;
}

void
updatesystrayiconstate(Client *i, XPropertyEvent *ev)
{
	long flags;
	int code = 0;
	Atom type;
	int fmt;
	unsigned long nitems, after;
	unsigned char *data = NULL;

	if (!i || ev->atom != xatom[XembedInfo] ||
	    XGetWindowProperty(dpy, i->win, xatom[XembedInfo], 0, 2, False, xatom[XembedInfo],
	                       &type, &fmt, &nitems, &after, &data) != Success)
		return;
	if (!data || nitems < 2) {
		if (data) XFree(data);
		return;
	}
	flags = ((long *)data)[1];
	if (flags & XEMBED_MAPPED) {
		code = XEMBED_WINDOW_ACTIVATE;
		XMapRaised(dpy, i->win);
		setclientstate(i, NormalState);
	} else {
		code = XEMBED_WINDOW_ACTIVATE; /* keep activated */
		XMapRaised(dpy, i->win);
	}
	sendevent(i->win, xatom[Xembed], StructureNotifyMask, CurrentTime, code, 0, systray->win, XEMBED_EMBEDDED_VERSION);
	XFree(data);
}

void
updatesystray(void)
{
	XSetWindowAttributes wa;
	Client *i;
	Monitor *m = NULL;
	int w = 0;

	if (!settings.showsystray && !systray)
		return;
	m = traymonitor();
	if (!m)
		return;
	drawbar(m);

	/* The bar paints the segment. Keep only the XEmbed icon strip in the
	 * default visual so legacy icons can use a ParentRelative background. */
	unsigned long traybg = shell_rgb(settings.palette[SchemeStatus][ColBg]);

	if (!systray) {
		systray = ecalloc(1, sizeof(Systray));
		systray->monitor = -1;
		systray->x = m->mx + m->mw;
		systray->y = m->by;
		systray->bg = traybg;
		wa.override_redirect = True;
		wa.event_mask = ButtonPressMask | SubstructureNotifyMask;
		wa.background_pixel = traybg;
		wa.border_pixel = 0;
		systray->win = XCreateWindow(dpy, root, systray->x, systray->y, 1, bh, 0,
		                             DefaultDepth(dpy, screen), InputOutput,
		                             DefaultVisual(dpy, screen),
		                             CWOverrideRedirect | CWEventMask | CWBackPixel | CWBorderPixel, &wa);
		XClassHint klass = {"mwm-tray", "mwm"};
		XSetClassHint(dpy, systray->win, &klass);
		XChangeProperty(dpy, systray->win, netatom[NetSystemTrayOrientation], XA_CARDINAL, 32,
		                PropModeReplace, (unsigned char *)&netatom[NetSystemTrayOrientationHorz], 1);
		{
			long v = XVisualIDFromVisual(DefaultVisual(dpy, screen));
			XChangeProperty(dpy, systray->win, netatom[NetSystemTrayVisual], XA_VISUALID, 32,
			                PropModeReplace, (unsigned char *)&v, 1);
		}
		XChangeProperty(dpy, systray->win, netatom[NetWMWindowType], XA_ATOM, 32,
		                PropModeReplace, (unsigned char *)&netatom[NetWMWindowTypeDock], 1);
		XSetSelectionOwner(dpy, netatom[NetSystemTray], systray->win, CurrentTime);
		if (XGetSelectionOwner(dpy, netatom[NetSystemTray]) == systray->win) {
			sendevent(root, xatom[Manager], StructureNotifyMask, CurrentTime, netatom[NetSystemTray], systray->win, 0, 0);
			XSync(dpy, False);
		} else {
			fprintf(stderr, "mwm: unable to obtain system tray selection.\n");
			XDestroyWindow(dpy, systray->win);
			free(systray);
			systray = NULL;
			return;
		}
	}
	if (systray->monitor != m->num) {
		systray->monitor = m->num;
		painttray((XRectangle){0});
		drawbars();
	}

	for (i = systray->icons; i; i = i->next) {
		XMoveResizeWindow(dpy, i->win, w, 0, i->w, i->h);
		XMapWindow(dpy, i->win);
		w += i->w + settings.systrayspacing;
	}
	w = w ? w - settings.systrayspacing : 0;
	int inset = shell_bar_inset(&settings) + settings.systrayvertpad;
	int height = MAX(1, bh - 2*inset);
	if (systray->width != w || systray->height != height)
		systray->clip = (XRectangle){0};
	systray->width = w;
	systray->height = height;
	if (systray->bg != traybg) {
		XSetWindowBackground(dpy, systray->win, traybg);
		XClearWindow(dpy, systray->win);
		systray->bg = traybg;
	}
	if (!w || (!barvisible && !bar_level) || !settings.showsystray)
		painttray((XRectangle){0});
	else {
		XResizeWindow(dpy, systray->win, w, height);
		if (systray->mapped)
			XRaiseWindow(dpy, systray->win);
	}
	XSync(dpy, False);
}

/* ---- misc X helpers -------------------------------------------------- */

void
setclientstate(Client *c, long state)
{
	long data[] = { state, None };

	XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
	                PropModeReplace, (unsigned char *)data, 2);
}

int
sendevent(Window w, Atom proto, int mask, long d0, long d1, long d2, long d3, long d4)
{
	int n;
	Atom *protocols, mt;
	int exists = 0;
	XEvent ev;

	if (proto == wmatom[WMTakeFocus] || proto == wmatom[WMDelete]) {
		mt = wmatom[WMProtocols];
		if (XGetWMProtocols(dpy, w, &protocols, &n)) {
			while (!exists && n--)
				exists = protocols[n] == proto;
			XFree(protocols);
		}
	} else {
		exists = 1;
		mt = proto;
	}
	if (exists) {
		ev.type = ClientMessage;
		ev.xclient.window = w;
		ev.xclient.message_type = mt;
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = d0;
		ev.xclient.data.l[1] = d1;
		ev.xclient.data.l[2] = d2;
		ev.xclient.data.l[3] = d3;
		ev.xclient.data.l[4] = d4;
		XSendEvent(dpy, w, False, mask, &ev);
	}
	return exists;
}

Atom
getatomprop(Client *c, Atom prop)
{
	int di;
	unsigned long dl;
	unsigned char *p = NULL;
	Atom da, atom = None;

	if (XGetWindowProperty(dpy, c->win, prop, 0L, sizeof atom, False, XA_ATOM,
	                       &da, &di, &dl, &dl, &p) == Success && p) {
		atom = *(Atom *)p;
		XFree(p);
	}
	return atom;
}

int
getrootptr(int *x, int *y)
{
	int di;
	unsigned int dui;
	Window dummy;

	return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

long
getstate(Window w)
{
	int format;
	long result = -1;
	unsigned char *p = NULL;
	unsigned long n, extra;
	Atom real;

	if (XGetWindowProperty(dpy, w, wmatom[WMState], 0L, 2L, False, wmatom[WMState],
	                       &real, &format, &n, &extra, (unsigned char **)&p) != Success)
		return -1;
	if (n != 0)
		result = *p;
	XFree(p);
	return result;
}

/* The legacy X11 STRING encoding is Latin-1. CUI text is always UTF-8. */
static size_t
latin1_utf8(char *out, size_t capacity, const char *src, size_t length)
{
	size_t n = 0;
	for (size_t i = 0; i < length && src[i]; ++i) {
		unsigned char ch = src[i];
		size_t bytes = ch < 128 ? 1 : 2;
		if (n + bytes >= capacity)
			break;
		if (bytes == 2) {
			out[n++] = 0xc0 | (ch >> 6);
			ch = 0x80 | (ch & 0x3f);
		}
		out[n++] = ch;
	}
	out[n] = 0;
	return n;
}

static int
valid_utf8(const unsigned char *s, size_t size)
{
	for (size_t i = 0; i < size && s[i];) {
		unsigned char c = s[i++];
		if (c < 128)
			continue;
		int count = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2
		            : c >= 0xc2 && c <= 0xdf ? 1 : 0;
		if (!count || i + count > size)
			return 0;
		if ((c == 0xe0 && s[i] < 0xa0) || (c == 0xed && s[i] > 0x9f)
		    || (c == 0xf0 && s[i] < 0x90) || (c == 0xf4 && s[i] > 0x8f))
			return 0;
		while (count--)
			if ((s[i++] & 0xc0) != 0x80)
				return 0;
	}
	return 1;
}

int
gettextprop(Window w, Atom atom, char *text, unsigned int size)
{
	char **list = NULL;
	int n;
	XTextProperty name = {0};

	if (!text || size == 0)
		return 0;
	text[0] = 0;
	if (XGetTextProperty(dpy, w, &name, atom) && name.nitems) {
		/* Root status tools commonly write UTF-8 bytes with XStoreName. */
		if (name.encoding == XA_STRING && !valid_utf8(name.value, name.nitems))
			latin1_utf8(text, size, (char *)name.value, name.nitems);
		else if (name.encoding == XA_STRING || name.encoding == xa_utf8) {
			size_t length = MIN(name.nitems, size - 1);
			while (length && length < name.nitems && (name.value[length] & 0xc0) == 0x80)
				--length;
			snprintf(text, size, "%.*s", (int)length, (char *)name.value);
		} else if (Xutf8TextPropertyToTextList(dpy, &name, &list, &n) >= Success && n > 0) {
			snprintf(text, size, "%s", *list);
		}
	}
	if (list)
		XFreeStringList(list);
	if (name.value)
		XFree(name.value);
	return text[0] != 0;
}

Client *
wintoclient(Window w)
{
	Client *c;

	for (c = clients; c; c = c->next)
		if (c->win == w)
			return c;
	return NULL;
}

Client *
wintosystrayicon(Window w)
{
	Client *i = NULL;

	if (!systray || !w)
		return i;
	for (i = systray->icons; i && i->win != w; i = i->next)
		;
	return i;
}

Monitor *
recttomon(int x, int y, int w, int h)
{
	Monitor *m, *r = selmon;
	int a, area = 0;

	for (m = mons; m; m = m->next)
		if ((a = INTERSECT(x, y, w, h, m)) > area) {
			area = a;
			r = m;
		}
	return r;
}

Monitor *
wintomon(Window w)
{
	int x, y;
	Client *c;
	Monitor *m;

	if (w == root && getrootptr(&x, &y))
		return recttomon(x, y, 1, 1);
	if (systray && w == systray->win)
		return traymonitor();
	for (m = mons; m; m = m->next)
		if (w == m->barwin)
			return m;
	if ((c = wintoclient(w)))
		return clientmon(c) ? clientmon(c) : selmon;
	return selmon;
}

/* ---- monitors / geometry -------------------------------------------- */

Monitor *
createmon(int num)
{
	Monitor *m = ecalloc(1, sizeof(Monitor));
	m->num = num;
	m->seltag = -1;
	m->sel = NULL;
	strncpy(m->ltsymbol, layoutsym[settings.defaultlayout], sizeof(m->ltsymbol) - 1);
	return m;
}

#ifdef XINERAMA
static int
isuniquegeom(XineramaScreenInfo *unique, size_t n, XineramaScreenInfo *info)
{
	while (n--)
		if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org
		&& unique[n].width == info->width && unique[n].height == info->height)
			return 0;
	return 1;
}
#endif /* XINERAMA */

int
updategeom(int initial)
{
	int dirty = 0;
	int oldcount = 0, nn = 1, i;
	Monitor *m;
	int oldseltag[MaxMonitors];
	struct { int x, y, w, h; } geom[MaxMonitors];

	for (m = mons; m; m = m->next)
		oldcount++;

#ifdef XINERAMA
	if (XineramaIsActive(dpy)) {
		int j, k;
		XineramaScreenInfo *info = XineramaQueryScreens(dpy, &nn);
		XineramaScreenInfo *unique = ecalloc(nn, sizeof(XineramaScreenInfo));
		for (j = 0, k = 0; j < nn; j++)
			if (isuniquegeom(unique, k, &info[j]))
				memcpy(&unique[k++], &info[j], sizeof(XineramaScreenInfo));
		XFree(info);
		nn = k;
		if (nn > MaxMonitors) nn = MaxMonitors;
		for (j = 0; j < nn; j++) {
			geom[j].x = unique[j].x_org;
			geom[j].y = unique[j].y_org;
			geom[j].w = unique[j].width;
			geom[j].h = unique[j].height;
		}
		free(unique);
	} else
#endif /* XINERAMA */
	{
		nn = 1;
		geom[0].x = 0;
		geom[0].y = 0;
		geom[0].w = sw;
		geom[0].h = sh;
	}

	if (nn != oldcount)
		dirty = 1;
	else {
		i = 0;
		for (m = mons; m && i < nn; m = m->next, i++)
			if (m->mx != geom[i].x || m->my != geom[i].y ||
			    m->mw != geom[i].w || m->mh != geom[i].h)
				dirty = 1;
	}
	if (!dirty)
		return 0;

	shell_panel_close(&shell, &panel);
	shell_tooltip_close(&shell, &tooltip);
	/* snapshot old per-monitor selected tags by index */
	for (i = 0; i < MaxMonitors; i++)
		oldseltag[i] = -1;
	for (i = 0, m = mons; m && i < MaxMonitors; m = m->next, i++)
		oldseltag[i] = m->seltag;

	/* tear down old monitors (clients are global, so they survive) */
	while (mons) {
		m = mons;
		mons = mons->next;
		if (m->barwin) {
			XUnmapWindow(dpy, m->barwin);
			XDestroyWindow(dpy, m->barwin);
		}
		shell_surface_free(&shell, &m->bar.surface);
		free(m);
	}

	/* build new monitors */
	mons = NULL;
	{
		Monitor **tail = &mons;
		for (i = 0; i < nn; i++) {
			m = createmon(i);
			m->mx = m->wx = geom[i].x;
			m->my = m->wy = geom[i].y;
			m->mw = m->ww = geom[i].w;
			m->mh = m->wh = geom[i].h;
			if (i < oldcount)
				m->seltag = oldseltag[i];
			*tail = m;
			tail = &m->next;
		}
	}

	/* reassign tags whose owner monitor disappeared */
	for (i = 0; i < MAX_TAGS; i++)
		if (tagstate[i].mon >= nn)
			tagstate[i].mon = nn - 1;

	/* apply default tag->monitor rules for newly appeared monitors */
	for (i = 0; i < MAX_TAGS; i++) {
		int mon = settings.tagmon[i];
		if (mon >= 0 && mon < nn && (mon >= oldcount || initial) && tagstate[i].mon < 0)
			tagstate[i].mon = mon;
	}

	/* ensure each monitor shows one of its owned tags if it has any */
	for (m = mons; m; m = m->next) {
		if (m->seltag >= 0 && tagstate[m->seltag].mon == m->num)
			continue;
		m->seltag = -1;
		for (i = 0; i < MAX_TAGS; i++)
			if (tagstate[i].mon == m->num) {
				m->seltag = i;
				break;
			}
	}

	updatebars();
	selmon = mons;
	if (!initial)
		updatesystray();
	return 1;
}

static void
updatebars(void)
{
	Monitor *m;
	XSetWindowAttributes wa;
	XClassHint ch = { "mwm", "mwm" };

	wa.override_redirect = True;
	wa.background_pixel = 0;
	wa.border_pixel = 0;
	wa.colormap = cmap;
	wa.event_mask = ButtonPressMask | ExposureMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask;

	for (m = mons; m; m = m->next) {
		updatebarpos(m);
		if (!m->barwin) {
			m->barwin = XCreateWindow(dpy, root, m->mx, m->by, m->mw, bh, 0, depth,
			                          InputOutput, visual,
			                          CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap | CWEventMask, &wa);
			XDefineCursor(dpy, m->barwin, cursor[CurNormal]);
			m->bar.hover_x = m->bar.hover_y = -1;
			XSetClassHint(dpy, m->barwin, &ch);
			XChangeProperty(dpy, m->barwin, netatom[NetWMWindowType], XA_ATOM, 32,
			                PropModeReplace, (unsigned char *)&netatom[NetWMWindowTypeDock], 1);
		}
		XMoveResizeWindow(dpy, m->barwin, m->mx, m->by, m->mw, bh);
		if (!bar_level && m->barmapped) {
			XUnmapWindow(dpy, m->barwin);
			m->barmapped = 0;
		}
		drawbar(m);
	}
}

/* ---- startup / shutdown --------------------------------------------- */

void
scan(void)
{
	unsigned int i, num;
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;

	if (XQueryTree(dpy, root, &d1, &d2, &wins, &num)) {
		for (i = 0; i < num; i++) {
			if (wintoclient(wins[i]) || !XGetWindowAttributes(dpy, wins[i], &wa)
			|| wa.override_redirect || XGetTransientForHint(dpy, wins[i], &d1))
				continue;
			if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
				manage(wins[i], &wa);
		}
		for (i = 0; i < num; i++) { /* now the transients */
			if (wintoclient(wins[i]) || !XGetWindowAttributes(dpy, wins[i], &wa) || wa.override_redirect)
				continue;
			if (XGetTransientForHint(dpy, wins[i], &d1)
			&& (wa.map_state == IsViewable || getstate(wins[i]) == IconicState))
				manage(wins[i], &wa);
		}
		if (wins)
			XFree(wins);
	}
}

void
sigchld(int unused)
{
	(void)unused;
	if (signal(SIGCHLD, sigchld) == SIG_ERR)
		die("can't install SIGCHLD handler:");
	while (0 < waitpid(-1, NULL, WNOHANG))
		;
}

int
xerror(Display *dpy, XErrorEvent *ee)
{
	if (ee->error_code == BadWindow
	|| (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
	|| (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable)
	|| (ee->request_code == X_PolyFillRectangle && ee->error_code == BadDrawable)
	|| (ee->request_code == X_PolySegment && ee->error_code == BadDrawable)
	|| (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)
	|| (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
	|| (ee->request_code == X_GrabKey && ee->error_code == BadAccess)
	|| (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
		return 0;
	fprintf(stderr, "mwm: fatal error: request code=%d, error code=%d\n",
	        ee->request_code, ee->error_code);
	return xerrorxlib(dpy, ee);
}

int
xerrordummy(Display *dpy, XErrorEvent *ee)
{
	(void)dpy; (void)ee;
	return 0;
}

int
xerrorstart(Display *dpy, XErrorEvent *ee)
{
	(void)dpy; (void)ee;
	die("mwm: another window manager is already running");
	return -1;
}

void
checkotherwm(void)
{
	xerrorxlib = XSetErrorHandler(xerrorstart);
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

static const char *
applystyle(const Config *next)
{
	static char error[CONFIG_ERROR];
	ShellFont font;
	if (!shell_font(&font, next, error))
		return error;

	int first = !theme_revision;
	int old_modifier = settings.movemod;
	if (first || next->showbar != settings.showbar) {
		barvisible = next->showbar;
		bar_level = shell_reveal(&whole_bar, barvisible, 0, 0);
	}
	if (first || next->showtraybar != settings.showtraybar)
		systrayvisible = next->showtraybar;
	if (!gap_overridden)
		gappx_run = next->gappx;

	shell_panel_close(&shell, &panel);
	shell_tooltip_close(&shell, &tooltip);
	for (Monitor *m = mons; m; m = m->next) {
		shell_surface_free(&shell, &m->bar.surface);
		m->bar.last_frame = 0;
	}
	unsigned hidden = 0;
	for (int i = 0; i < settings.nsegments; ++i)
		if (hidden_segments & (1u << i)) {
			int index = config_segment(next, settings.segments[i].name);
			if (index >= 0)
				hidden |= 1u << index;
		}
	hidden_segments = hidden;
	if (first)
		controls_init(&controls);
	else if (strcmp(settings.audio_sink, next->audio_sink) ||
	         strcmp(settings.battery_device, next->battery_device) ||
	         strcmp(settings.backlight_device, next->backlight_device) ||
	         settings.brightness_min != next->brightness_min)
		controls_reset(&controls);
	shell_font_free(&shellfont);
	shellfont = font;
	if (settings.cpu_interval != next->cpu_interval)
		system_next = 0;
	settings = *next;
	++theme_revision;
	lrpad = 2 * MAX(5, settings.barpadx);
	int inset = shell_bar_inset(&settings);
	bh = settings.barheight ? MAX(settings.barheight, (int)font.bar_size + 10 + 2*inset)
	                        : (int)font.bar_size + 2*(settings.barvertpad + inset) + 4;

	if (first)
		return NULL;
	for (int i = 0; i < MAX_TAGS; ++i) {
		if (tagstate[i].mon >= 0)
			continue;
		tagstate[i].layout = settings.defaultlayout;
		tagstate[i].nmaster = settings.nmaster;
		tagstate[i].mfact = settings.mfact;
	}
	for (Client *c = clients; c; c = c->next) {
		if (c->isfullscreen)
			c->oldbw = settings.borderpx;
		else {
			c->bw = settings.borderpx;
			XSetWindowBorderWidth(dpy, c->win, c->bw);
			if (settings.roundcorners)
				roundwin(c->win, c->w, c->h, settings.cornerradius, c->bw);
			else
				clearround(c->win);
		}
		int selected = clientmon(c) && c == clientmon(c)->sel;
		XSetWindowBorder(dpy, c->win, 0xff000000UL |
		                 shell_rgb(settings.palette[selected ? SchemeSel : SchemeNorm][ColBorder]));
		if (settings.movemod != old_modifier)
			grabbuttons(c, selected && clientmon(c) == selmon);
	}
	if (systray)
		for (Client *i = systray->icons; i; i = i->next)
			updatesystrayicongeom(i, i->w, i->h);
	updatebars();
	updatesystray();
	arrange(NULL);
	drawbars();
	return NULL;
}

static const char *
reloadstyle(void)
{
	static char error[CONFIG_ERROR];
	Config next;
	config_defaults(&next);
	if (!config_read(&next, config_path, required_config, error)
	    || !config_theme(&next, dpy, root, cmap, error))
		return error;
	return applystyle(&next);
}

static void
setup_display(const Config *staged)
{
	XVisualInfo vinfo;

	screen = DefaultScreen(dpy);
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
	root = RootWindow(dpy, screen);

	/* prefer a 32-bit ARGB visual so the bar can be translucent */
	XRenderPictFormat *format;
	if (XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)
	    && (format = XRenderFindVisualFormat(dpy, vinfo.visual)) && format->direct.alphaMask == 255) {
		visual = vinfo.visual;
		depth = vinfo.depth;
		cmap = XCreateColormap(dpy, root, visual, AllocNone);
	} else {
		visual = DefaultVisual(dpy, screen);
		depth = DefaultDepth(dpy, screen);
		cmap = DefaultColormap(dpy, screen);
	}

	/* shape extension for rounded corners */
	{
		int err;
		shapeext = XShapeQueryExtension(dpy, &shapeevbase, &err);
	}

	shell = (ShellHost){.dpy = dpy, .root = root, .visual = visual, .depth = depth,
	                    .cmap = cmap, .shape = shapeext};
	shell.composited = shell_composited(&shell);
	const char *error = staged ? applystyle(staged) : reloadstyle();
	if (error)
		die("mwm: %s", error);
}

void
setup(const RestartState *state)
{
	XSetWindowAttributes wa;
	Atom utf8string;

	sigchld(0);

	/* tag defaults */
	for (int i = 0; i < MAX_TAGS; i++) {
		tagstate[i].layout = settings.defaultlayout;
		tagstate[i].nmaster = settings.nmaster;
		tagstate[i].mfact = settings.mfact;
		tagstate[i].mon = -1;
	}

	/* atoms */
	utf8string = XInternAtom(dpy, "UTF8_STRING", False);
	xa_utf8 = utf8string;
	wmatom[WMProtocols] = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmatom[WMDelete] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmatom[WMState] = XInternAtom(dpy, "WM_STATE", False);
	wmatom[WMTakeFocus] = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netatom[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
	netatom[NetSystemTray] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
	netatom[NetSystemTrayOP] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	netatom[NetSystemTrayOrientation] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
	netatom[NetSystemTrayOrientationHorz] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION_HORZ", False);
	netatom[NetSystemTrayVisual] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
	netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
	netatom[NetWMStateDemandsAttention] = XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	netatom[NetWMFullscreen] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypeDock] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	netatom[NetWMWindowTypeDialog] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	netatom[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	xatom[Manager] = XInternAtom(dpy, "MANAGER", False);
	xatom[Xembed] = XInternAtom(dpy, "_XEMBED", False);
	xatom[XembedInfo] = XInternAtom(dpy, "_XEMBED_INFO", False);
	mwmatom[MwmMsg] = XInternAtom(dpy, MWM_ATOM_MSG, False);
	mwmatom[MwmCmd] = XInternAtom(dpy, MWM_ATOM_CMD, False);
	mwmatom[MwmReply] = XInternAtom(dpy, MWM_ATOM_REPLY, False);

	/* cursors */
	cursor[CurNormal] = XCreateFontCursor(dpy, XC_left_ptr);
	cursor[CurResize] = XCreateFontCursor(dpy, XC_sizing);
	cursor[CurMove] = XCreateFontCursor(dpy, XC_fleur);

	/* supporting wm check window (EWMH) */
	wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMCheck], XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *)&wmcheckwin, 1);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMName], utf8string, 8,
	                PropModeReplace, (unsigned char *)"mwm", 3);
	XChangeProperty(dpy, root, netatom[NetWMCheck], XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *)&wmcheckwin, 1);
	XChangeProperty(dpy, root, netatom[NetSupported], XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)netatom, NetLast);
	XDeleteProperty(dpy, root, netatom[NetClientList]);

	if (state) {
		barvisible = state->bar;
		bar_level = shell_reveal(&whole_bar, barvisible, 0, 0);
		systrayvisible = state->tray;
		gap_overridden = state->gap_override;
		if (gap_overridden)
			gappx_run = state->gap;
		timer = (ShellTimer){.active = state->timer.active, .at = state->timer.at,
		                     .due = state->timer.due, .wall = state->timer.wall};
	}
	updategeom(1);
	if (settings.showsystray || systray)
		updatesystray();

	wa.cursor = cursor[CurNormal];
	wa.event_mask = ROOTMASK;
	XChangeWindowAttributes(dpy, root, CWEventMask | CWCursor, &wa);

	updatestatus();
	if (!state)
		focus(NULL);
}

void
cleanup(void)
{
	Monitor *m;

	running = 0;
	controls_reset(&controls);
	shell_panel_close(&shell, &panel);
	shell_tooltip_close(&shell, &tooltip);
	while (clients)
		unmanage(clients, 0);
	if (systray) {
		XUnmapWindow(dpy, systray->win);
		while (systray->icons) {
			Client *icon = systray->icons;
			systray->icons = icon->next;
			XUnmapWindow(dpy, icon->win);
			if (icon->depth == DefaultDepth(dpy, screen))
				XSetWindowBackgroundPixmap(dpy, icon->win, None);
			XReparentWindow(dpy, icon->win, root, 0, 0);
			XRemoveFromSaveSet(dpy, icon->win);
			free(icon);
		}
		XDestroyWindow(dpy, systray->win);
		free(systray);
	}
	while (mons) {
		m = mons;
		mons = mons->next;
		if (m->barwin) {
			XUnmapWindow(dpy, m->barwin);
			XDestroyWindow(dpy, m->barwin);
		}
		shell_surface_free(&shell, &m->bar.surface);
		free(m);
	}
	for (int i = 0; i < CurLast; ++i)
		XFreeCursor(dpy, cursor[i]);
	shell_icons_free();
	shell_font_free(&shellfont);
	XDestroyWindow(dpy, wmcheckwin);
	XSync(dpy, False);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
}

/* ---- executable handoff ---------------------------------------------- */

static void
restart_args(char *args[6], const char *mode, const char *fd)
{
	int n = 0;
	args[n++] = executable;
	args[n++] = (char *)mode;
	args[n++] = (char *)fd;
	if (required_config) {
		args[n++] = "-c";
		args[n++] = (char *)config_path;
	}
	args[n] = NULL;
}

static int
check_replacement(int fd, const char *state_fd)
{
	char *args[6];
	sigset_t block, previous;
	int status;
	pid_t child, done;

	restart_args(args, RESTART_CHECK, state_fd);
	sigemptyset(&block);
	sigaddset(&block, SIGCHLD);
	/* The normal SIGCHLD handler must not reap the validation subprocess. */
	if (sigprocmask(SIG_BLOCK, &block, &previous) < 0)
		return 0;
	child = fork();
	if (!child) {
		sigprocmask(SIG_SETMASK, &previous, NULL);
		fexecve(fd, args, environ);
		_exit(127);
	}
	double deadline = shell_seconds() + 10.0;
	done = -1;
	while (child > 0) {
		done = waitpid(child, &status, WNOHANG);
		if (done > 0 || (done < 0 && errno != EINTR))
			break;
		if (shell_seconds() >= deadline) {
			kill(child, SIGKILL);
			while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
				;
			done = -1;
			break;
		}
		nanosleep(&(struct timespec){.tv_nsec = 20000000}, NULL);
	}
	sigprocmask(SIG_SETMASK, &previous, NULL);
	return done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int
save_session(FILE *file, Window requestor)
{
	RestartState state = {
		.root = root, .checkwin = wmcheckwin, .requestor = requestor,
		.display_fd = ConnectionNumber(dpy), .selected = selmon->num,
		.bar = barvisible, .tray = systrayvisible, .gap = gappx_run,
		.gap_override = gap_overridden, .timer = {timer.active, timer.at, timer.due, timer.wall},
	};
	memcpy(state.tags, tagstate, sizeof state.tags);
	for (Monitor *m = mons; m; m = m->next)
		state.monitors[state.nmon++] = (RestartMonitor){
			m->mx, m->my, m->mw, m->mh, m->seltag, m->sel ? m->sel->win : None,
		};
	for (Client *c = clients; c; c = c->next)
		++state.nclients;
	for (Client *i = systray ? systray->icons : NULL; i; i = i->next)
		++state.nicons;
	if (fwrite(&state, sizeof state, 1, file) != 1)
		return 0;
	for (Client *c = clients; c; c = c->next) {
		RestartClient saved = {
			.win = c->win, .tag = c->tag, .x = c->x, .y = c->y, .w = c->w, .h = c->h,
			.oldx = c->oldx, .oldy = c->oldy, .oldw = c->oldw, .oldh = c->oldh,
			.bw = c->bw, .oldbw = c->oldbw, .floating = c->isfloating,
			.fullscreen = c->isfullscreen, .sticky = c->issticky, .oldstate = c->oldstate,
			.urgent = c->isurgent,
		};
		if (fwrite(&saved, sizeof saved, 1, file) != 1)
			return 0;
	}
	for (Client *c = fstack; c; c = c->snext)
		if (fwrite(&c->win, sizeof c->win, 1, file) != 1)
			return 0;
	for (Client *i = systray ? systray->icons : NULL; i; i = i->next)
		if (fwrite(&i->win, sizeof i->win, 1, file) != 1)
			return 0;
	if (fwrite(&timer.duration, sizeof timer.duration, 1, file) != 1)
		return 0;
	char hidden[MAX_BAR_SEGMENTS][SEGMENT_NAME] = {{0}};
	for (int i = 0; i < settings.nsegments; ++i)
		if (hidden_segments & (1u << i))
			strcpy(hidden[i], settings.segments[i].name);
	if (fwrite(hidden, sizeof hidden, 1, file) != 1)
		return 0;
	return fflush(file) == 0 && fseek(file, 0, SEEK_SET) == 0;
}

static int
read_restart_magic(FILE *file)
{
	char magic[sizeof restart_magic];
	return fread(magic, sizeof magic, 1, file) == 1 && !memcmp(magic, restart_magic, sizeof magic);
}

static const char *
restart(Window requestor)
{
	static char error[CONFIG_ERROR];
	int fd = open(executable, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(error, sizeof error, "cannot open replacement mwm at %.480s: %s", executable, strerror(errno));
		return error;
	}
	FILE *file = tmpfile();
	if (!file) {
		close(fd);
		return "cannot create the restart state file";
	}
	char fd_arg[32];
	snprintf(fd_arg, sizeof fd_arg, "%d", fileno(file));
	/* The candidate writes its validated config before we append the session.
	 * It can then use that config even if an edit races with the handoff. */
	if (fcntl(fileno(file), F_SETFD, 0) < 0 || !check_replacement(fd, fd_arg)
	    || fseek(file, 0, SEEK_SET) != 0 || !read_restart_magic(file)
	    || fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		close(fd);
		return "replacement mwm failed the restart check (see the mwm log)";
	}
	int display_fd = ConnectionNumber(dpy);
	/* Keep the old X connection alive until the replacement connects. The new
	 * connection can then retire the old WM's resources, including its tray. */
	if (fcntl(display_fd, F_SETFD, 0) < 0) {
		fclose(file);
		close(fd);
		return "cannot pass restart file descriptors to the replacement";
	}
	XSelectInput(dpy, root, NoEventMask);
	XSync(dpy, False);
	/* Finish requests the server already redirected. Any new maps during the
	 * handoff go straight to the server and are picked up by the new scan. */
	XEvent ev;
	while (XCheckMaskEvent(dpy, SubstructureRedirectMask, &ev))
		if (handler[ev.type])
			handler[ev.type](&ev);
	const char *detail = "cannot save the restart state";
	if (save_session(file, requestor)) {
		char *args[6];
		restart_args(args, RESTART_RESUME, fd_arg);
		XFlush(dpy);
		controls_reset(&controls);
		fexecve(fd, args, environ);
		detail = "cannot execute the replacement";
	}
	snprintf(error, sizeof error, "%s: %s", detail, strerror(errno));
	fcntl(display_fd, F_SETFD, FD_CLOEXEC);
	close(fd);
	fclose(file);
	XSelectInput(dpy, root, ROOTMASK);
	scan();
	return error;
}

static void
restore_monitors(const RestartState *state, Monitor *mapping[MaxMonitors])
{
	for (int i = 0; i < state->nmon; ++i) {
		const RestartMonitor *saved = &state->monitors[i];
		Monitor *m;
		for (m = mons; m; m = m->next)
			if (m->mx == saved->x && m->my == saved->y && m->mw == saved->w && m->mh == saved->h)
				break;
		mapping[i] = m ? m : recttomon(saved->x, saved->y, saved->w, saved->h);
	}
	selmon = mapping[state->selected];
	for (int i = 0; i < MAX_TAGS; ++i) {
		int owner = state->tags[i].mon;
		if (owner >= 0) {
			tagstate[i] = state->tags[i];
			tagstate[i].mon = mapping[owner]->num;
		}
	}
	for (Monitor *m = mons; m; m = m->next) {
		m->seltag = -1;
		for (int tag = 0; tag < MAX_TAGS; ++tag)
			if (tagstate[tag].mon == m->num) {
				m->seltag = tag;
				break;
			}
	}
	for (int i = 0; i < state->nmon; ++i) {
		int tag = state->monitors[i].tag;
		if (tag >= 0 && tagstate[tag].mon == mapping[i]->num)
			mapping[i]->seltag = tag;
	}
	int tag = state->monitors[state->selected].tag;
	if (tag >= 0)
		selmon->seltag = tag;
}

static Client *
restore_client(const RestartClient *saved)
{
	XWindowAttributes wa;
	if (!XGetWindowAttributes(dpy, saved->win, &wa) || wa.override_redirect
	    || (wa.map_state != IsViewable && getstate(saved->win) != IconicState))
		return NULL;

	Client *c = ecalloc(1, sizeof *c);
	c->win = saved->win;
	c->tag = saved->tag;
	c->x = saved->x; c->y = saved->y; c->w = saved->w; c->h = saved->h;
	c->oldx = saved->oldx; c->oldy = saved->oldy; c->oldw = saved->oldw; c->oldh = saved->oldh;
	c->bw = saved->fullscreen ? 0 : settings.borderpx;
	c->oldbw = saved->fullscreen ? settings.borderpx : saved->oldbw;
	c->isfloating = saved->floating;
	c->isfullscreen = saved->fullscreen;
	c->issticky = saved->sticky;
	c->oldstate = saved->oldstate;
	updatetitle(c);
	XClassHint ch = {0};
	updateclass(c, &ch);
	if (ch.res_class) XFree(ch.res_class);
	if (ch.res_name) XFree(ch.res_name);
	updatesizehints(c);
	updatewmhints(c);
	c->isurgent = saved->urgent;

	if (c->isfullscreen) {
		Monitor *m = clientmon(c);
		c->x = m->mx; c->y = m->my; c->w = m->mw; c->h = m->mh;
	}
	XSelectInput(dpy, c->win, EnterWindowMask | FocusChangeMask | PropertyChangeMask | StructureNotifyMask);
	unfocus(c, 0);
	XSetWindowBorderWidth(dpy, c->win, c->bw);
	XMoveResizeWindow(dpy, c->win, ISVISIBLE(c) ? c->x : -2 * WIDTH(c), c->y, c->w, c->h);
	if (settings.roundcorners && !c->isfullscreen)
		roundwin(c->win, c->w, c->h, settings.cornerradius, c->bw);
	else
		clearround(c->win);
	XMapWindow(dpy, c->win);
	return c;
}

static void
resume_session(FILE *file, const RestartState *state)
{
	RestartClient *saved = ecalloc(MAX(1, state->nclients), sizeof *saved);
	Window *order = ecalloc(MAX(1, state->nclients), sizeof *order);
	Window *icons = ecalloc(MAX(1, state->nicons), sizeof *icons);
	if (fread(saved, sizeof *saved, state->nclients, file) != state->nclients
	    || fread(order, sizeof *order, state->nclients, file) != state->nclients
	    || fread(icons, sizeof *icons, state->nicons, file) != state->nicons)
		die("mwm: incomplete restart state");
	double timer_duration = 0;
	/* v0.1.1 and earlier end the handoff after the tray icons. */
	size_t bytes = fread(&timer_duration, 1, sizeof timer_duration, file);
	if (ferror(file) || (bytes && bytes != sizeof timer_duration))
		die("mwm: incomplete timer state");
	char hidden[MAX_BAR_SEGMENTS][SEGMENT_NAME] = {{0}};
	bytes = fread(hidden, 1, sizeof hidden, file);
	if (ferror(file) || (bytes && bytes != sizeof hidden))
		die("mwm: incomplete segment state");
	fclose(file);

	/* Retire the previous resources under one grab. The server reparents and
	 * maps the old tray's save-set windows before we redirect new map requests. */
	XGrabServer(dpy);
	XKillClient(dpy, state->checkwin);
	XSync(dpy, False);
	close(state->display_fd);
	checkotherwm();
	setup(state);
	for (int i = 0; i < MAX_BAR_SEGMENTS; ++i) {
		hidden[i][SEGMENT_NAME-1] = 0;
		int index = config_segment(&settings, hidden[i]);
		if (index >= 0)
			hidden_segments |= 1u << index;
	}
	if (timer.active)
		timer.duration = isfinite(timer_duration) && timer_duration > 0 ? timer_duration
		                 : fmax(1, shell_timer_remaining(&timer, time(NULL), shell_seconds()));
	Monitor *mapping[MaxMonitors];
	restore_monitors(state, mapping);

	Client **link = &clients;
	for (size_t i = 0; i < state->nclients; ++i) {
		Client *c = restore_client(&saved[i]);
		if (c) {
			*link = c;
			link = &c->next;
		}
	}
	link = &fstack;
	for (size_t i = 0; i < state->nclients; ++i) {
		Client *c = wintoclient(order[i]);
		if (c) {
			*link = c;
			link = &c->snext;
		}
	}
	Client **dock = systray ? &systray->icons : NULL;
	for (size_t i = 0; i < state->nicons; ++i) {
		XWindowAttributes wa;
		if (!XGetWindowAttributes(dpy, icons[i], &wa))
			continue;
		if (dock) {
			dockicon(icons[i], &wa, dock);
			dock = &(*dock)->next;
		} else {
			XUnmapWindow(dpy, icons[i]);
			XDeleteProperty(dpy, icons[i], wmatom[WMState]);
		}
	}
	scan();
	updateclientlist();
	for (int i = 0; i < state->nmon; ++i) {
		Monitor *m = mapping[i];
		Client *c = wintoclient(state->monitors[i].focus);
		if (c && ISVISIBLE(c) && clientmon(c) == m) {
			m->sel = c;
			XSetWindowBorder(dpy, c->win, 0xff000000UL | shell_rgb(settings.palette[SchemeSel][ColBorder]));
		}
	}
	selmon = mapping[state->selected];
	focus(selmon->sel);
	arrange(NULL);
	updatesystray();
	XUngrabServer(dpy);
	spawnreply(state->requestor, "mwm restarted");
	free(icons);
	free(order);
	free(saved);
}

enum { PaletteLimit = 256 };
static int
palette_entries(ShellEntry entries[PaletteLimit])
{
	int count = 0;
	const char *query = panel.input;
	/* Always reserve capacity for tags and layouts when many windows are open. */
	for (Client *c = fstack; c && count < PaletteLimit - MAX_TAGS - 3; c = c->snext) {
		if (!ci_strstr(c->name, query) && !ci_strstr(c->class, query) && !ci_strstr(c->instance, query))
			continue;
		ShellEntry *entry = &entries[count++];
		char tag[64];
		taglabel(c->tag, tag, sizeof tag);
		snprintf(entry->label, sizeof entry->label, "%s", c->name);
		snprintf(entry->detail, sizeof entry->detail, "%.46s · %.42s", c->class, tag);
		entry->type = EntryWindow;
		entry->value = c->win;
	}
	for (int t = 0; t < MAX_TAGS && count < PaletteLimit - 3; ++t) {
		if (t >= tagcount() && tagstate[t].mon < 0)
			continue;
		char tag[64];
		taglabel(t, tag, sizeof tag);
		if (!ci_strstr(tag, query) && !ci_strstr("tag", query))
			continue;
		ShellEntry *entry = &entries[count++];
		snprintf(entry->label, sizeof entry->label, "Go to %s", tag);
		strcpy(entry->detail, "Tag");
		entry->type = EntryTag;
		entry->value = t;
	}
	const char *layouts[] = {"Columns", "Master stack", "Floating"};
	for (int i = 0; i < 3; ++i) {
		if (!ci_strstr(layouts[i], query) && !ci_strstr("layout", query))
			continue;
		ShellEntry *entry = &entries[count++];
		snprintf(entry->label, sizeof entry->label, "%s", layouts[i]);
		strcpy(entry->detail, "Layout");
		entry->type = EntryLayout;
		entry->value = i;
	}
	return count;
}

static void
palette_choose(int row)
{
	ShellEntry entries[PaletteLimit];
	int count = palette_entries(entries);
	if (row < 0 || row >= count)
		return;
	ShellEntry *entry = &entries[row];
	shell_panel_close(&shell, &panel);
	if (entry->type == EntryLayout) {
		static const char *names[] = {"cols", "stack", "float"};
		setlayout(names[entry->value]);
		return;
	}
	Client *client = entry->type == EntryWindow ? wintoclient(entry->value) : NULL;
	if (entry->type == EntryWindow && !client)
		return;
	char tag[16];
	snprintf(tag, sizeof tag, "%u", client ? (unsigned)client->tag + 1 : (unsigned)entry->value + 1);
	viewtag(tag);
	if (client) {
		focus(client);
		restack(selmon);
		warptoclient(client);
	}
}

static void
openpanel(Monitor *m, enum ShellAction action, float anchor, Time event_time)
{
	if (!m)
		return;
	if (panel.win && panel.kind == action && panel.monitor == m->num) {
		shell_panel_close(&shell, &panel);
		return;
	}
	if (shell_panel_open(&shell, &settings, &panel, action, m->num,
	                     m->mx, m->my, m->mw, m->mh, m->by, bh, anchor, event_time)) {
		panel_dirty = 1;
		m->bar.hover_x = m->bar.hover_y = -1;
		drawbar(m);
	}
}

static void
timer_start(enum ShellAction action, time_t value)
{
	if (timer.active)
		return;
	int started = action == ActTimerSet ? shell_timer_set(&timer, panel.at, panel.input, panel.message)
	              : shell_timer_preset(&timer, action == ActTimerAtPreset, value, panel.message);
	if (started) {
		panel.input[0] = panel.message[0] = 0;
		panel.cursor = 0;
		drawbars();
	}
}

static void
panel_activate(enum ShellAction action, time_t value)
{
	switch (action) {
	case ActPast: shell_calendar_shift(&panel, -1); break;
	case ActFuture: shell_calendar_shift(&panel, 1); break;
	case ActToday: {
		time_t now = time(NULL);
		struct tm local;
		if (localtime_r(&now, &local)) {
			panel.month = local.tm_mon;
			panel.year = local.tm_year + 1900;
		}
		break;
	}
	case ActAt:
	case ActIn:
		if (panel.at != (action == ActAt)) {
			panel.input[0] = panel.message[0] = 0;
			panel.cursor = 0;
		}
		panel.at = action == ActAt;
		break;
	case ActTimerInput: panel.cursor = strlen(panel.input); break;
	case ActTimerSet:
	case ActTimerInPreset:
	case ActTimerAtPreset: timer_start(action, value); break;
	case ActTimerCancel:
		timer = (ShellTimer){0};
		drawbars();
		break;
	case ActZonePrevious:
		panel.zone_start = MAX(0, panel.zone_start - 1);
		break;
	case ActZoneNext:
		panel.zone_start = MIN(MAX(0, settings.nzones + 1 - panel.zone_rows), panel.zone_start + 1);
		break;
	case ActChoose: palette_choose(value); break;
	default: break;
	}
	panel_dirty = 1;
}

static void
timer_key(KeySym key, int ch)
{
	int length = strlen(panel.input), cursor = panel.cursor;
	if (timer.active)
		return;
	switch (key) {
	case XK_Return: case XK_KP_Enter: panel_activate(ActTimerSet, 0); break;
	case XK_Tab: case XK_ISO_Left_Tab:
		panel_activate(panel.at ? ActIn : ActAt, 0);
		break;
	case XK_Left: case XK_KP_Left: panel.cursor = MAX(0, cursor-1); break;
	case XK_Right: case XK_KP_Right: panel.cursor = MIN(length, cursor+1); break;
	case XK_Home: case XK_KP_Home: panel.cursor = 0; break;
	case XK_End: case XK_KP_End: panel.cursor = length; break;
	case XK_BackSpace:
		if (cursor > 0) {
			memmove(panel.input+cursor-1, panel.input+cursor, length-cursor+1);
			--panel.cursor;
			panel.message[0] = 0;
		}
		break;
	case XK_Delete: case XK_KP_Delete:
		if (cursor < length) {
			memmove(panel.input+cursor, panel.input+cursor+1, length-cursor);
			panel.message[0] = 0;
		}
		break;
	default:
		if (key >= XK_KP_0 && key <= XK_KP_9)
			ch = '0' + key - XK_KP_0;
		else if (key == XK_colon || key == XK_KP_Separator || key == XK_KP_Decimal)
			ch = ':';
		if ((ch == ':' || (ch >= '0' && ch <= '9')) && length < (panel.at ? 5 : 8)) {
			memmove(panel.input+cursor+1, panel.input+cursor, length-cursor+1);
			panel.input[cursor] = ch;
			++panel.cursor;
			panel.message[0] = 0;
		}
		break;
	}
	panel_dirty = 1;
}

static void
set_control(int kind, int value)
{
	controls_set(&controls, kind, value);
	drawbars();
	panel_dirty |= panel.win && shell_panel_control(panel.kind) == kind;
}

static void
adjust_control(int kind, int direction)
{
	int value = controls_value(&controls, kind);
	set_control(kind, value + direction * controls_step(&settings, kind));
}

static void
panel_key(XKeyEvent *event)
{
	char latin1[32], bytes[2 * sizeof latin1 + 1];
	KeySym key;
	int raw = XLookupString(event, latin1, sizeof latin1, &key, NULL);
	size_t n = latin1_utf8(bytes, sizeof bytes, latin1, raw > 0 ? raw : 0);
	if (key == XK_Escape) {
		shell_panel_close(&shell, &panel);
		return;
	}
	int control = shell_panel_control(panel.kind);
	if (control >= 0) {
		if (key == XK_Left || key == XK_Down || key == XK_KP_Left || key == XK_KP_Down)
			adjust_control(control, -1);
		if (key == XK_Right || key == XK_Up || key == XK_KP_Right || key == XK_KP_Up)
			adjust_control(control, 1);
		if (key == XK_Home || key == XK_KP_Home)
			set_control(control, 0);
		if (key == XK_End || key == XK_KP_End)
			set_control(control, 100);
		return;
	}
	if (panel.kind == ActCalendar) {
		if (key == XK_Left) panel_activate(ActPast, 0);
		if (key == XK_Right) panel_activate(ActFuture, 0);
		if (key == XK_Home) panel_activate(ActToday, 0);
		return;
	}
	if (panel.kind == ActClock) {
		if (!(event->state & ControlMask))
			timer_key(key, raw == 1 ? (unsigned char)latin1[0] : 0);
		return;
	}
	if (panel.kind == ActPalette) {
		if (key == XK_Return || key == XK_KP_Enter) {
			palette_choose(panel.selected);
			return;
		}
		if (key == XK_Up || key == XK_Down) {
			ShellEntry entries[PaletteLimit];
			int count = palette_entries(entries);
			if (count)
				panel.selected = (panel.selected + count + (key == XK_Up ? -1 : 1)) % count;
			panel_dirty = 1;
			return;
		}
	}
	size_t length = strlen(panel.input);
	if (key == XK_BackSpace) {
		if (length) {
			do {
				--length;
			} while (((unsigned char)panel.input[length] & 0xc0) == 0x80);
			panel.input[length] = 0;
		}
	} else {
		if (!n || length + n >= sizeof panel.input || (event->state & ControlMask))
			return;
		for (size_t i = 0; i < n; ++i) {
			if ((unsigned char)bytes[i] < 32 || (unsigned char)bytes[i] == 127)
				return;
		}
		memcpy(panel.input + length, bytes, n);
		panel.input[length + n] = 0;
	}
	panel.message[0] = 0;
	panel.selected = 0;
	panel_dirty = 1;
}

static void
panel_event(XEvent *e)
{
	int control = shell_panel_control(panel.kind);
	if (e->type == Expose) {
		panel_dirty = 1;
		return;
	}
	if (e->type == KeyPress) {
		panel_key(&e->xkey);
		return;
	}
	if (e->type == MotionNotify) {
		panel.hover_x = e->xmotion.x;
		panel.hover_y = e->xmotion.y;
		if (control >= 0)
			shell_panel_pointer(&panel, CUI_EVENT_POINTER_MOVE, panel.hover_x, panel.hover_y);
		panel_dirty = 1;
		return;
	}
	XButtonEvent *click = &e->xbutton;
	if (e->type == ButtonRelease) {
		if (control >= 0 && click->button == Button1) {
			shell_panel_pointer(&panel, CUI_EVENT_POINTER_UP, click->x, click->y);
			panel_dirty = 1;
		}
		return;
	}
	if (click->x < panel.pad || click->x >= panel.pad + panel.width
	    || click->y < panel.pad || click->y >= panel.pad + panel.height) {
		enum ShellAction was = panel.kind;
		shell_panel_close(&shell, &panel);
		for (Monitor *m = mons; m; m = m->next) {
			if (!barvisible || click->y_root < m->by || click->y_root >= m->by + bh)
				continue;
			ShellHit hit = shell_hit(m->bar.hits, m->bar.nhits, click->x_root-m->mx, click->y_root-m->by);
			int hit_control = shell_panel_control(hit.action);
			if (hit_control >= 0 && (click->button == Button4 || click->button == Button5))
				adjust_control(hit_control, click->button == Button4 ? 1 : -1);
			if (click->button == Button1 && hit.action != was && panel_action(hit.action))
				openpanel(m, hit.action, m->mx + hit.rect.x + hit.rect.w / 2, click->time);
		}
		return;
	}
	if (click->button == Button4 || click->button == Button5) {
		if (control >= 0)
			adjust_control(control, click->button == Button4 ? 1 : -1);
		if (panel.kind == ActCalendar)
			panel_activate(click->button == Button4 ? ActPast : ActFuture, 0);
		if (panel.kind == ActClock)
			panel_activate(click->button == Button4 ? ActZonePrevious : ActZoneNext, 0);
		return;
	}
	if (click->button == Button1) {
		if (control >= 0) {
			shell_panel_pointer(&panel, CUI_EVENT_POINTER_DOWN, click->x, click->y);
			panel_dirty = 1;
			return;
		}
		ShellHit hit = shell_hit(panel.hits, panel.nhits, click->x, click->y);
		panel_activate(hit.action, hit.value);
	}
}

static unsigned
active_bar_items(void)
{
	unsigned items = 0;
	if (barvisible)
		for (int i = 0; i < settings.nsegments; ++i)
			if (!(hidden_segments & (1u << i)))
				for (int j = 0; j < settings.segments[i].count; ++j)
					items |= 1u << settings.segments[i].items[j];
	return items;
}

static double
sample_cpu(int reset)
{
	static unsigned long long last_total, last_idle;
	static int have_baseline;
	if (reset)
		have_baseline = 0;

	unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
	char line[512];
	FILE *file = fopen("/proc/stat", "r");
	int ok = file && fgets(line, sizeof line, file) && !strncmp(line, "cpu ", 4) &&
	         sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
	                &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) == 8;
	if (file)
		fclose(file);
	if (!ok) {
		have_baseline = 0;
		return -1;
	}
	idle += iowait;
	unsigned long long total = user + nice + system + idle + irq + softirq + steal;
	double usage = -1;
	if (have_baseline && total > last_total && idle >= last_idle &&
	    idle - last_idle <= total - last_total)
		usage = 100.0 * (1.0 - (double)(idle - last_idle) / (total - last_total));
	last_total = total;
	last_idle = idle;
	have_baseline = 1;
	return usage;
}

static double
sample_ram(void)
{
	FILE *file = fopen("/proc/meminfo", "r");
	if (!file)
		return -1;
	unsigned long long total = 0, available = 0;
	int have_available = 0;
	char line[256];
	while (fgets(line, sizeof line, file)) {
		if (!strncmp(line, "MemTotal:", 9))
			sscanf(line + 9, "%llu", &total);
		if (!strncmp(line, "MemAvailable:", 13))
			have_available = sscanf(line + 13, "%llu", &available) == 1;
		if (total && have_available)
			break;
	}
	fclose(file);
	return total && have_available ? 100.0 * (total - MIN(total, available)) / total : -1;
}

static void
sample_system(double tick)
{
	int reset = !system_next;
	system_next = tick + settings.cpu_interval / 1000.0;
	double values[] = {[UsageCpu] = sample_cpu(reset), [UsageRam] = sample_ram()};
	double load[LoadPeriods];
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	int valid = cpus > 0 && getloadavg(load, LoadPeriods) == LoadPeriods;
	int changed = valid != system_stats.load_valid ||
	              (valid && (cpus != system_stats.cpus || memcmp(load, system_stats.load, sizeof load)));
	system_stats.load_valid = valid;
	system_stats.cpus = cpus;
	if (valid)
		memcpy(system_stats.load, load, sizeof load);
	for (int i = 0; i < UsageCount; ++i) {
		ShellUsage *usage = &system_stats.usage[i];
		if (reset || values[i] < 0) {
			changed |= usage->count > 0;
			usage->count = 0;
		}
		if (values[i] < 0)
			continue;
		if (usage->count == MAX_USAGE_SAMPLES) {
			--usage->count;
			memmove(usage->history, usage->history + 1, usage->count * sizeof usage->history[0]);
		}
		usage->history[usage->count++] = (cui_chart_datum){.value = values[i]};
		++usage->revision;
		changed = 1;
	}
	if (changed)
		drawbars();
}

static double
system_tooltip_tick(double tick)
{
	static Window hovered_bar;
	static int hovered_metric = MetricNone;
	static double due;
	Monitor *m = NULL;
	int metric = MetricNone;
	if (barvisible && bar_level == 1 && !panel.win && settings.tooltip_delay >= 0)
		for (m = mons; m; m = m->next) {
			metric = shell_bar_metric(&m->bar);
			if (metric != MetricNone)
				break;
		}
	if (!m) {
		hovered_bar = None;
		shell_tooltip_hide(&shell, &tooltip);
		return 0;
	}
	if (m->barwin != hovered_bar || metric != hovered_metric) {
		due = tick + (tooltip.mapped ? 0 : settings.tooltip_delay / 1000.0);
		hovered_bar = m->barwin;
		hovered_metric = metric;
		tooltip.dirty = 1;
	}
	if (tick < due)
		return due;
	if (tooltip.dirty || !tooltip.mapped) {
		cui_rect anchor = m->bar.metrics[metric];
		anchor.x += m->mx;
		anchor.y += m->by;
		shell_tooltip_draw(&shell, &shellfont, theme_revision, &settings, &tooltip, &system_stats,
		                   &controls, metric, (cui_rect){m->mx, m->my, m->mw, m->mh}, anchor);
	}
	return 0;
}

static int
ui_tick(void)
{
	static time_t minute = -1, second = -1, checked_compositor = -1;
	static double frame;
	time_t now = time(NULL);
	double tick = shell_seconds();
	Monitor *m = traymonitor();
	if (systray && m && systray->monitor != m->num)
		updatesystray();
	if (!panel.win)
		panel_dirty = 0;
	if (now != checked_compositor) {
		checked_compositor = now;
		int composited = shell_composited(&shell);
		if (composited != shell.composited) {
			shell.composited = composited;
			shell_panel_close(&shell, &panel);
			shell_tooltip_close(&shell, &tooltip);
			updatesystray();
			drawbars();
		}
	}
	if (timer.active && shell_timer_remaining(&timer, now, tick) <= 0) {
		timer.active = 0;
		shell_timer_notify(&settings, ConnectionNumber(dpy));
		drawbars();
		panel_dirty = panel.win != None;
	}
	if (now / 60 != minute) {
		minute = now / 60;
		for (Monitor *m = mons; m; m = m->next)
			drawbar(m);
		panel_dirty |= panel.win && panel.kind != ActPalette;
	}
	if (timer.active && second != now) {
		if (settings.timerprogressheight)
			drawbars();
		panel_dirty |= panel.win && panel.kind == ActClock;
	}
	second = now;
	unsigned items = active_bar_items();
	if (!(items & ((1u << BarCpu) | (1u << BarRam) | (1u << BarLoad))))
		system_next = 0;
	else if (tick >= system_next)
		sample_system(tick);

	int can_animate = tick >= frame;
	if (can_animate)
		animate_bar(tick);
	int controls_changed = 0;
	unsigned enabled = (items >> BarBattery) & ((1u << ControlCount) - 1);
	int control = shell_panel_control(panel.kind);
	if (panel.win && control >= 0)
		enabled |= 1u << control;
	int controls_timeout = controls_tick(&controls, &settings, enabled, ConnectionNumber(dpy), tick, &controls_changed);
	if (controls_changed) {
		drawbars();
		panel_dirty |= panel.win && control >= 0;
	}
	int animating = whole_bar.progress != barvisible;
	for (Monitor *m = mons; m; m = m->next) {
		if (m->bardirty || (bar_level > 0 && m->bar.animating && can_animate))
			paintbar(m, now, tick);
		animating |= bar_level > 0 && m->bar.animating;
	}
	int panel_animating = panel.win && shell.composited && !settings.reduced_motion
	                      && (tick - panel.opened) * 1000 < settings.duration + 16;
	if (panel.win && (panel_dirty || (panel_animating && can_animate))) {
		ShellEntry entries[PaletteLimit];
		int count = panel.kind == ActPalette ? palette_entries(entries) : 0;
		if (count)
			panel.selected = MIN(panel.selected, count - 1);
		shell_panel_draw(&shell, &shellfont, theme_revision, &settings, &panel,
		                 &timer, &controls, entries, count, now, tick);
		panel_dirty = 0;
		if (control >= 0 && panel.control_request >= 0) {
			set_control(control, panel.control_request);
			panel.control_request = -1;
			controls_timeout = 1;
		}
	}
	if (can_animate)
		frame = tick + 0.016;
	double tooltip_next = system_tooltip_tick(tick);
	int timeout = animating || panel_animating ? MAX(1, (int)ceil((frame-tick)*1000)) : 1000;
	timeout = MIN(timeout, controls_timeout);
	if (timer.active)
		timeout = MIN(timeout, MAX(1, (int)ceil(1000*shell_timer_remaining(&timer, now, tick))));
	if (system_next)
		timeout = MIN(timeout, MAX(1, (int)ceil((system_next-tick)*1000)));
	if (tooltip_next)
		timeout = MIN(timeout, MAX(1, (int)ceil((tooltip_next-tick)*1000)));
	return timeout;
}

void
run(void)
{
	struct pollfd display = {.fd = ConnectionNumber(dpy), .events = POLLIN};
	while (running) {
		/* Yield to timers and animation even when an X client is flooding the queue. */
		for (int n = 0; n < 128 && running && XPending(dpy); ++n) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (panel.win && ev.xany.window == panel.win &&
			    (ev.type == KeyPress || ev.type == ButtonPress || ev.type == ButtonRelease ||
			     ev.type == Expose || ev.type == MotionNotify))
				panel_event(&ev);
			else if (ev.type >= 0 && ev.type < LASTEvent && handler[ev.type])
				handler[ev.type](&ev);
		}
		if (!running)
			break;
		int timeout = ui_tick();
		XFlush(dpy);
		if (!XPending(dpy) && poll(&display, 1, timeout) < 0 && errno != EINTR)
			die("mwm: poll:");
	}
}

int
main(int argc, char *argv[])
{
	if (argc == 2 && !strcmp("-v", argv[1])) {
		fputs("mwm-"VERSION"\n", stderr);
		return EXIT_SUCCESS;
	}
	if (argc == 2 && !strcmp("--font-license", argv[1])) {
		shell_font_license();
		return EXIT_SUCCESS;
	}
	int check = argc >= 3 && !strcmp(argv[1], RESTART_CHECK);
	int resume = argc >= 3 && !strcmp(argv[1], RESTART_RESUME);
	int first = 1;
	FILE *handoff = NULL;
	RestartState state;
	Config staged;
	if (check || resume) {
		char *end;
		long fd = strtol(argv[2], &end, 10);
		if (end == argv[2] || *end || fd < 3 || fd > INT_MAX
		    || !(handoff = fdopen((int)fd, check ? "wb" : "rb")))
			die("mwm: invalid restart state");
		if (resume) {
			if (!read_restart_magic(handoff)
			    || fread(&staged, sizeof staged, 1, handoff) != 1
			    || fread(&state, sizeof state, 1, handoff) != 1
			    || state.checkwin == None
			    || state.display_fd < 3 || state.display_fd == fd
			    || state.nmon < 1 || state.nmon > MaxMonitors
			    || state.selected < 0 || state.selected >= state.nmon
			    || argv[0][0] != '/' || strlen(argv[0]) >= sizeof executable)
				die("mwm: invalid restart state");
			strcpy(executable, argv[0]);
		}
		first = 3;
	} else {
		ssize_t n = readlink("/proc/self/exe", executable, sizeof executable - 1);
		if (n >= 0 && (size_t)n < sizeof executable - 1)
			executable[n] = '\0';
		else
			executable[0] = '\0';
	}
	static char path[4096];
	if (argc == first + 2 && !strcmp(argv[first], "-c")) {
		config_path = argv[first + 1];
		required_config = 1;
	} else if (argc != first) {
		die("usage: mwm [-v] [-c config.ini]");
	} else {
		const char *xdg = getenv("XDG_CONFIG_HOME");
		const char *home = getenv("HOME");
		int n = xdg && xdg[0] == '/' ? snprintf(path, sizeof path, "%s/mwm/config.ini", xdg)
		        : home && *home ? snprintf(path, sizeof path, "%s/.config/mwm/config.ini", home)
		        : snprintf(path, sizeof path, "/etc/mwm/config.ini");
		if (n < 0 || (size_t)n >= sizeof path)
			die("mwm: config path is too long");
		config_path = path;
	}
	setlocale(LC_TIME, "");
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("mwm: cannot open display");
	if (fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC) < 0)
		die("mwm: cannot set close-on-exec for X11:");
	if (resume && state.root != DefaultRootWindow(dpy))
		die("mwm: restart state belongs to a different X screen");
	if (!resume && !check)
		checkotherwm();
	setup_display(resume ? &staged : NULL);
	if (check) {
		if (fwrite(restart_magic, sizeof restart_magic, 1, handoff) != 1
		    || fwrite(&settings, sizeof settings, 1, handoff) != 1 || fclose(handoff) != 0)
			die("mwm: cannot pass the checked configuration to the replacement:");
		shell_font_free(&shellfont);
		XCloseDisplay(dpy);
		return EXIT_SUCCESS;
	}
	if (resume) {
		resume_session(handoff, &state);
	} else {
		setup(NULL);
		scan();
	}
	run();
	cleanup();
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}
