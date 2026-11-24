#ifndef MWM_SHELL_H
#define MWM_SHELL_H

#include "mwm_config.h"
#include "cui/cui_software.h"

typedef struct {
	Display *dpy;
	Window root;
	Visual *visual;
	Colormap cmap;
	int depth, shape, composited;
} ShellHost;

typedef struct {
	cui_font *font;
	float size, bar_size;
	cui_font *medium, *mono;
} ShellFont;

typedef struct {
	cui_context *ui;
	cui_software *renderer;
	XImage *image;
	GC gc;
	unsigned revision;
	cui_rect shapes[5];
	int shape_count, shape_radius, shape_outset;
} ShellSurface;

enum ShellAction {
	ActNone, ActTag, ActLayout, ActCalendar, ActClock, ActPalette,
	ActPast, ActToday, ActFuture, ActAt, ActIn, ActTimerInput, ActTimerSet,
	ActTimerCancel, ActZonePrevious, ActZoneNext, ActChoose
};

typedef struct {
	cui_rect rect;
	enum ShellAction action;
	int value;
} ShellHit;

typedef struct {
	ShellSurface surface;
	ShellHit hits[MAX_TAGS + 5];
	int nhits;
	int hover_x, hover_y;
	int hovered;
	double hover_since;
	int tag;
	float selected_x, selected_w, from_x, from_w;
	double transition, last_frame;
	int animating;
} ShellBar;

typedef struct {
	int id, urgent;
	char label[64];
} ShellTag;

typedef struct {
	int w, h, pad, tray_width;
	int tag, ntags, layout;
	ShellTag tags[MAX_TAGS];
	const char *title, *status;
} ShellBarModel;

typedef struct {
	char label[256], detail[96];
	unsigned long value;
	int type;
} ShellEntry;

enum { EntryWindow, EntryTag, EntryLayout };

typedef struct {
	ShellSurface surface;
	Window win;
	enum ShellAction kind;
	int monitor, width, height, pad, year, month;
	int zone_start, zone_rows, at, selected, cursor;
	int hover_x, hover_y;
	ShellHit hits[24];
	int nhits;
	char input[128], message[96];
	double opened;
} ShellPanel;

typedef struct {
	int active, at;
	double due;
	time_t wall;
} ShellTimer;

int shell_font(ShellFont *, const Config *, char error[CONFIG_ERROR]);
void shell_font_free(ShellFont *);
void shell_font_license(void);
void shell_surface_free(const ShellHost *, ShellSurface *);
void shell_icons_free(void);
double shell_seconds(void);
int shell_composited(const ShellHost *);
int shell_bar_inset(const Config *);
ShellHit shell_hit(const ShellHit *, int count, int x, int y);
void shell_bar_draw(const ShellHost *, const ShellFont *, unsigned revision,
                    const Config *, ShellBar *, Window, const ShellBarModel *,
                    const ShellTimer *, time_t now, double tick);
int shell_panel_open(const ShellHost *, const Config *, ShellPanel *, enum ShellAction,
                     int monitor, int x, int y, int w, int h, int by, int bh,
                     float anchor, Time time);
void shell_panel_close(const ShellHost *, ShellPanel *);
int shell_panel_draw(const ShellHost *, const ShellFont *, unsigned revision,
                     const Config *, ShellPanel *, const ShellTimer *,
                     const ShellEntry *, int count, time_t now, double tick);
void shell_calendar_shift(ShellPanel *, int months);
int shell_timer_set(ShellTimer *, int at, const char *input, char error[96]);
double shell_timer_remaining(const ShellTimer *, time_t now, double tick);
void shell_timer_notify(const Config *, int xfd);
unsigned long shell_rgb(cui_color);

#endif
