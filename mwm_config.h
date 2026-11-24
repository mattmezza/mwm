#ifndef MWM_CONFIG_H
#define MWM_CONFIG_H

#include "cui/cui.h"
#include <X11/Xlib.h>
#include <time.h>

enum { MAX_TAGS = 100, MAX_RULES = 64, MAX_ZONES = 8, CONFIG_ERROR = 768 };
enum { SchemeNorm, SchemeSel, SchemeUrg, SchemeTitle, SchemeStatus, SchemeBar, SchemeLast };
enum { ColFg, ColBg, ColBorder };

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
	int barseggap, segradius, titlealign, barfontsize;
	int showsystray, showtraybar, systraymon, systrayspacing, systrayvertpad;
	int panelradius, panel_opacity, calendar_width, clock_width, palette_width;
	int duration, reduced_motion, shadows, shadow_opacity, bar_shadow_blur, panel_shadow_blur;
	char font[128], monofont[128];
	char colors[SchemeLast][3][64];
	int alpha[SchemeLast];
	cui_color palette[SchemeLast][3];
	char tags[MAX_TAGS][64];
	int tagmon[MAX_TAGS], ntags;
	Rule rules[MAX_RULES];
	int nrules;
	TimeZone zones[MAX_ZONES];
	int nzones;
	char hook[512];
} Config;

void config_defaults(Config *config);
int config_read(Config *config, const char *path, int required, char error[CONFIG_ERROR]);
int config_theme(Config *config, Display *dpy, Window root, Colormap cmap,
                 char error[CONFIG_ERROR]);
int config_tag(const Config *config, const char *value);
int config_zone_time(const char *zone, time_t now, struct tm *out);

#endif
