#ifndef MWM_CONFIG_H
#define MWM_CONFIG_H

#include "cui/cui.h"
#include <X11/Xlib.h>
#include <time.h>

enum { MAX_TAGS = 100, MAX_RULES = 64, MAX_ZONES = 8, MAX_TIMER_PRESETS = 24, CONFIG_ERROR = 768 };
enum { MAX_USAGE_SAMPLES = 120, MAX_USAGE_WIDTH = 200 };
enum { SYSTEM_INTERVAL_MIN = 125, SYSTEM_INTERVAL_MAX = 5000 };
enum { UsageWarning, UsageDanger, UsageColorCount };
enum { DisplaysExternal, DisplaysAlways, DisplaysNever };
enum { DisplayRow = 1, DisplayColumn = 2, DisplayTwoAbove = 4 };
enum { SchemeNorm, SchemeSel, SchemeUrg, SchemeTitle, SchemeStatus, SchemeBar, SchemeLast };
enum { ColFg, ColBg, ColBorder };
enum BarItem { BarTags, BarLayout, BarTitle, BarStatus, BarCpu, BarGpu, BarRam, BarLoad, BarNetwork, BarDisk, BarDate, BarTime,
               BarTray, BarBattery, BarVolume, BarBrightness, BarItemCount };
enum { MAX_BAR_SEGMENTS = BarItemCount, SEGMENT_NAME = 32 };
enum BarAlign { BarLeft, BarCenter, BarRight, BarAlignCount };

typedef struct {
	char name[SEGMENT_NAME];
	enum BarItem items[BarItemCount];
	int count;
	enum BarAlign align;
} BarPlacement;

typedef struct {
	char class[64], instance[64], title[128], tag[64], type[64];
	int floating;
} Rule;

typedef struct {
	char label[80], name[128];
} TimeZone;

typedef struct {
	int borderpx, gappx, roundcorners, cornerradius, resizehints, focusfollowsmouse;
	int warpcursor, movemod, snap, defaultlayout, nmaster;
	float mfact;
	int topbar, showbar, barheight, barvertpad, barpadx, barmargin, barvmargin;
	int barseggap, segradius, tagradius, titlealign, barfontsize, separators;
	BarPlacement segments[MAX_BAR_SEGMENTS];
	int nsegments, custom_segments;
	unsigned hidden_segments;
	int timer_indicator;
	int cpu_interval, cpu_history, cpu_width, gpu_history, gpu_width, ram_history, ram_width;
	int show_temperature, cpu_temp_critical, gpu_temp_critical;
	int load_width, network_width, disk_width, tooltip_delay;
	char gpu_device[64], network_interface[64], disk_device[64];
	int controls_interval, battery_low, battery_critical, battery_step, volume_step, brightness_step;
	int brightness_min, controls_width;
	char battery_device[64], backlight_device[64], audio_sink[64];
	int showsystray, showtraybar, systraymon, systrayspacing, systrayvertpad, notificationslast;
	int applet_width, applet_rows, wifi_secret_agent, bluetooth_default_agent;
	char wifi_interface[64], bluetooth_adapter[64];
	int display_visibility, display_quick_presets, display_saved_presets, display_mirror, display_identify;
	char undocked_command[512];
	int panelradius, panel_opacity, calendar_width, clock_width, palette_width;
	int duration, reduced_motion, shadows, shadow_opacity, bar_shadow_blur, panel_shadow_blur;
	int popup_enter, popup_exit, popup_distance;
	char font[128], monofont[128];
	char colors[SchemeLast][3][64];
	int alpha[SchemeLast];
	cui_color palette[SchemeLast][3];
	char usage_colors[UsageColorCount][64];
	cui_color usage_palette[UsageColorCount];
	char tray_success_color[64];
	cui_color tray_success;
	char tags[MAX_TAGS][64];
	int tagmon[MAX_TAGS], ntags;
	Rule rules[MAX_RULES];
	int nrules;
	TimeZone zones[MAX_ZONES];
	int nzones;
	int in_presets[MAX_TIMER_PRESETS], nin_presets, at_presets;
	char hook[512];
} Config;

void config_defaults(Config *config);
int config_default_path(char *path, size_t length);
int config_read(Config *config, const char *path, int required, char error[CONFIG_ERROR]);
int config_theme(Config *config, Display *dpy, Window root, Colormap cmap,
                 char error[CONFIG_ERROR]);
int config_tag(const Config *config, const char *value);
int config_segment(const Config *config, const char *name);
int config_zone_time(const char *zone, time_t now, struct tm *out);

#endif
