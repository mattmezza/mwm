#ifndef MWM_SHELL_H
#define MWM_SHELL_H

#include "mwm_config.h"
#include "controls.h"
#include "activity.h"
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
	ActBattery, ActVolume, ActBrightness, ActMonthGrid, ActYearGrid, ActCalendarMonth,
	ActCalendarYear, ActCalendarDay, ActTimerToggle, ActPresetPrevious, ActPresetNext, ActCopyTime, ActTimerFoldOpen,
	ActTimerMessage, ActPaletteInput, ActControlTrack
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

typedef struct {
	ShellSurface surface;
	Window win;
	int x, y, distance, duration;
	double started;
} ShellPopupExit;

enum { MetricNone = -1, UsageCpu, UsageGpu, UsageRam, UsageCount, MetricLoad = UsageCount,
       MetricNetwork, MetricDisk,
       MetricControl, MetricCount = MetricControl + ControlCount };
enum { USAGE_CHART_HEIGHT = 22, LoadPeriods = 3 };

typedef struct {
	cui_chart_datum history[MAX_USAGE_SAMPLES];
	int count;
	uint64_t revision;
} ShellUsage;

typedef struct {
	ShellUsage usage[UsageCount];
	Activity activity;
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
	int hover_x, hover_y, pointer;
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
	ShellPopupExit exit;
	Window win;
	int mapped, dirty, offset;
	double opened;
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
	int clock_separator;
} ShellBarModel;

typedef struct {
	char label[256], detail[96];
	unsigned long value;
	int type;
} ShellEntry;

enum { EntryWindow, EntryTag, EntryLayout };
enum { CalendarDays, CalendarMonths, CalendarYears, CalendarYearPage = 12 };
enum { TimerMessageSize = 256 };

typedef struct {
	ShellSurface surface;
	Window win;
	XIC ic;
	enum ShellAction kind;
	int monitor, width, height, pad, year, month;
	int calendar_view, browse_year;
	int zone_start, zone_rows, timer_open, at, selected, cursor, message_cursor, message_focus;
	int clock_limit, window_x, window_y, offset_y, clock_anchor, preset_start, preset_more;
	int window_height, clock_height;
	cui_rect preset_area;
	int hover_x, hover_y, pointer;
	ShellHit hits[52];
	int nhits;
	char input[128], timer_error[96], message[TimerMessageSize];
	double opened;
	cui_slider_state slider;
	int control_request;
} ShellPanel;

typedef struct {
	int active, at;
	double due;
	time_t wall;
	double duration;
	char message[TimerMessageSize];
} ShellTimer;

int shell_font(ShellFont *, const Config *, char error[CONFIG_ERROR]);
void shell_font_free(ShellFont *);
void shell_font_license(void);
void shell_surface_free(const ShellHost *, ShellSurface *);
int shell_surface_begin(const ShellHost *, const ShellFont *, unsigned, ShellSurface *,
                        Window, int width, int height, double elapsed);
int shell_surface_draw(const ShellHost *, ShellSurface *, Window, const cui_rect *body,
                       int radius, int outset);
int shell_popup_padding(const ShellHost *, const Config *);
void shell_popup_background(cui_context *, const ShellHost *, const Config *, cui_rect, float opacity);
int shell_popup_entrance(const Config *, double started, double now, int direction, int *offset);
void shell_popup_dismiss(const ShellHost *, const Config *, ShellPopupExit *, Window,
                        ShellSurface *, int direction);
int shell_popup_exit_tick(const ShellHost *, ShellPopupExit *, double now);
void shell_popup_exit_reset(const ShellHost *, ShellPopupExit *);
void shell_text(cui_context *, const ShellFont *, int id, cui_rect, const char *,
                cui_color, float size, cui_text_style, cui_align);
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
void shell_tooltip_hide(const ShellHost *, const Config *, ShellTooltip *);
void shell_tooltip_close(const ShellHost *, ShellTooltip *);
int shell_tooltip_motion(const ShellHost *, const Config *, ShellTooltip *, double now, int frame);
void shell_tooltip_draw(const ShellHost *, const ShellFont *, unsigned revision,
                        const Config *, ShellTooltip *, const ShellSystem *,
                        const Controls *, int metric, cui_rect monitor, cui_rect anchor);
int shell_panel_open(const ShellHost *, const Config *, ShellPanel *, enum ShellAction,
                     int monitor, int x, int y, int w, int h, int by, int bh,
                     float anchor, Time time);
void shell_panel_close(const ShellHost *, ShellPanel *);
void shell_panel_end_input(ShellPanel *);
cui_rect shell_panel_body(const Config *, const ShellPanel *);
int shell_panel_draw(const ShellHost *, const ShellFont *, unsigned revision,
                     const Config *, ShellPanel *, const ShellTimer *,
                     const Controls *, const ShellEntry *, int count, time_t now, double tick);
int shell_panel_control(enum ShellAction);
void shell_panel_pointer(ShellPanel *, cui_event_type, int x, int y);
void shell_calendar_shift(ShellPanel *, int months);
void shell_clock_resize(const Config *, ShellPanel *, int open, int active);
int shell_timer_set(ShellTimer *, int at, const char *input, char error[96]);
int shell_timer_preset(ShellTimer *, int at, time_t value, char error[96]);
double shell_timer_remaining(const ShellTimer *, time_t now, double tick);
void shell_timer_notify(const Config *, const ShellTimer *, int xfd);
unsigned long shell_rgb(cui_color);

#endif
