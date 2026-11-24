#ifndef MWM_SHELL_H
#define MWM_SHELL_H

#include "mwm_config.h"
#include "controls.h"
#include "cui/cui_chart.h"
#include "cui/cui_software.h"
#include "cui/cui_slider_events.h"

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
	cui_rect shapes[MAX_BAR_SEGMENTS];
	int shape_count, shape_radius, shape_outset;
	int shape_top, shape_bottom;
} ShellSurface;

enum ShellAction {
	ActNone, ActTag, ActLayout, ActCalendar, ActClock, ActPalette,
	ActPast, ActToday, ActFuture, ActAt, ActIn, ActTimerInput, ActTimerSet,
	ActTimerCancel, ActTimerInPreset, ActTimerAtPreset, ActZonePrevious, ActZoneNext, ActChoose,
	ActBattery, ActVolume, ActBrightness
};

typedef struct {
	cui_rect rect;
	enum ShellAction action;
	time_t value;
} ShellHit;

typedef struct {
	float progress;
	int shown;
} ShellReveal;

enum { MetricNone = -1, UsageCpu, UsageRam, UsageCount, MetricLoad = UsageCount,
       MetricControl, MetricCount = MetricControl + ControlCount };
enum { USAGE_CHART_HEIGHT = 22, LoadPeriods = 3 };

typedef struct {
	cui_chart_datum history[MAX_USAGE_SAMPLES];
	int count;
	uint64_t revision;
} ShellUsage;

typedef struct {
	ShellUsage usage[UsageCount];
	double load[LoadPeriods];
	long cpus;
	int load_valid, interval;
} ShellSystem;

typedef struct {
	cui_chart_state state;
	unsigned char pixels[MAX_USAGE_WIDTH * USAGE_CHART_HEIGHT * 4];
} ShellUsageChart;

typedef struct {
	ShellSurface surface;
	ShellHit hits[MAX_TAGS + 10];
	int nhits;
	int hover_x, hover_y;
	int hovered;
	double hover_since;
	int tag;
	float selected_x, selected_w, from_x, from_w;
	double transition, last_frame;
	int animating;
	ShellReveal segment_reveal[MAX_BAR_SEGMENTS];
	int tray_x, tray_clip_x, tray_clip_width;
	ShellUsageChart usage_charts[UsageCount];
	cui_rect metrics[MetricCount];
	char status[512], title[256];
} ShellBar;

typedef struct {
	ShellSurface surface;
	Window win;
	int mapped, dirty;
	cui_rect geometry;
} ShellTooltip;

typedef struct {
	int id, urgent;
	char label[64];
} ShellTag;

typedef struct {
	int w, h, pad, slide, map, clip_top, clip_bottom, tray_width, tray_shown;
	int tag, ntags, layout;
	ShellTag tags[MAX_TAGS];
	const char *title, *status;
	const ShellSystem *system;
	const Controls *controls;
	unsigned hidden;
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
	int zone_start, zone_rows, timer_presets, at, selected, cursor;
	int hover_x, hover_y;
	ShellHit hits[24];
	int nhits;
	char input[128], message[96];
	double opened;
	cui_slider_state slider;
	int control_request;
} ShellPanel;

typedef struct {
	int active, at;
	double due;
	time_t wall;
	double duration;
} ShellTimer;

int shell_font(ShellFont *, const Config *, char error[CONFIG_ERROR]);
void shell_font_free(ShellFont *);
void shell_font_license(void);
void shell_surface_free(const ShellHost *, ShellSurface *);
void shell_icons_free(void);
double shell_seconds(void);
float shell_reveal(ShellReveal *, int shown, double elapsed, int duration);
int shell_composited(const ShellHost *);
int shell_bar_inset(const Config *);
ShellHit shell_hit(const ShellHit *, int count, int x, int y);
int shell_bar_draw(const ShellHost *, const ShellFont *, unsigned revision,
                    const Config *, ShellBar *, Window, const ShellBarModel *,
                    const ShellTimer *, time_t now, double tick);
int shell_bar_metric(const ShellBar *, int x, int y);
void shell_tooltip_hide(const ShellHost *, ShellTooltip *);
void shell_tooltip_close(const ShellHost *, ShellTooltip *);
void shell_tooltip_draw(const ShellHost *, const ShellFont *, unsigned revision,
                        const Config *, ShellTooltip *, const ShellSystem *,
                        const Controls *, int metric, cui_rect monitor, cui_rect anchor);
int shell_panel_open(const ShellHost *, const Config *, ShellPanel *, enum ShellAction,
                     int monitor, int x, int y, int w, int h, int by, int bh,
                     float anchor, Time time);
void shell_panel_close(const ShellHost *, ShellPanel *);
int shell_panel_draw(const ShellHost *, const ShellFont *, unsigned revision,
                     const Config *, ShellPanel *, const ShellTimer *,
                     const Controls *, const ShellEntry *, int count, time_t now, double tick);
int shell_panel_control(enum ShellAction);
void shell_panel_pointer(ShellPanel *, cui_event_type, int x, int y);
void shell_calendar_shift(ShellPanel *, int months);
int shell_timer_set(ShellTimer *, int at, const char *input, char error[96]);
int shell_timer_preset(ShellTimer *, int at, time_t value, char error[96]);
double shell_timer_remaining(const ShellTimer *, time_t now, double tick);
void shell_timer_notify(const Config *, int xfd);
unsigned long shell_rgb(cui_color);

#endif
