#include "mwm_config.h"

#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

void
config_defaults(Config *c)
{
	static const char *tags[] = {"w1", "w2", "w3", "w4", "m1", "m2", "p4", "p3", "p2", "p1"};
	static const Rule rules[] = {
		{.class = "pavucontrol", .floating = 1}, {.class = "arandr", .floating = 1},
		{.class = "st-256color", .title = "termfloat", .floating = 1},
		{.class = "st-256color", .title = "calcfloat", .floating = 1},
		{.title = "is sharing your screen", .floating = 1},
		{.class = "chromium-personal", .tag = "10"}, {.class = "chromium-work", .tag = "1"},
		{.class = "Pinentry", .floating = 1},
		{.type = "DIALOG", .floating = 1}, {.type = "UTILITY", .floating = 1},
		{.type = "TOOLBAR", .floating = 1}, {.type = "SPLASH", .floating = 1},
	};
	*c = (Config){
		.borderpx = 2, .gappx = 8, .cornerradius = 3, .focusfollowsmouse = 1,
		.warpcursor = 1, .movemod = Mod4Mask, .snap = 8,
		.defaultlayout = 1, .nmaster = 1, .mfact = 0.5f,
		.topbar = 1, .showbar = 1, .barvertpad = 8, .barpadx = 12,
		.barmargin = 10, .barvmargin = 7, .barseggap = 10, .segradius = 3, .titlealign = 1,
		.showsystray = 1, .showtraybar = 1, .systraymon = -1, .systrayspacing = 7, .systrayvertpad = 6,
		.panelradius = 4, .panel_opacity = 255, .calendar_width = 394, .clock_width = 448, .palette_width = 530,
		.duration = 150, .shadows = 1, .shadow_opacity = 112, .bar_shadow_blur = 2, .panel_shadow_blur = 6,
		.font = "Geist:pixelsize=18", .monofont = "Geist Mono",
		.colors = {
			[SchemeNorm]   = {"#c0caf5", "#1a1b26", "#3b4261"},
			[SchemeSel]    = {"#1a1b26", "#7aa2f7", "#7aa2f7"},
			[SchemeUrg]    = {"#1a1b26", "#f7768e", "#f7768e"},
			[SchemeTitle]  = {"#c0caf5", "#1a1b26", "#3b4261"},
			[SchemeStatus] = {"#c0caf5", "#1a1b26", "#3b4261"},
			[SchemeBar]    = {"#c0caf5", "#16161e", "#3b4261"},
		},
		.alpha = {255, 255, 255, 255, 255, 0},
		.ntags = COUNT(tags), .nrules = COUNT(rules),
	};
	for (int i = 0; i < MAX_TAGS; ++i) {
		if (i < c->ntags)
			strcpy(c->tags[i], tags[i]);
		c->tagmon[i] = i == 0 ? 0 : -1;
	}
	memcpy(c->rules, rules, sizeof rules);
}

typedef struct {
	const char *section, *key;
	size_t offset, length;
	float min, max;
} Setting;

#define INT(s, k, f, lo, hi) {s, k, offsetof(Config, f), 0, lo, hi}
#define STR(s, k, f) {s, k, offsetof(Config, f), sizeof(((Config *)0)->f), 0, 0}
static const Setting settings[] = {
	INT("layout", "borderpx", borderpx, 0, 20), INT("layout", "gappx", gappx, 0, 100),
	INT("layout", "roundcorners", roundcorners, 0, 1), INT("layout", "cornerradius", cornerradius, 0, 48),
	INT("layout", "resizehints", resizehints, 0, 1), INT("layout", "focusfollowsmouse", focusfollowsmouse, 0, 1),
	INT("layout", "warpcursor", warpcursor, 0, 1), INT("layout", "movemod", movemod, 1, 255),
	INT("layout", "snap", snap, 0, 100), INT("layout", "nmaster", nmaster, 0, 100),
	INT("layout", "mfact", mfact, 0.05f, 0.95f),
	INT("bar", "topbar", topbar, 0, 1), INT("bar", "showbar", showbar, 0, 1),
	INT("bar", "barheight", barheight, 0, 100), INT("bar", "barvertpad", barvertpad, 0, 24),
	INT("bar", "barpadx", barpadx, 0, 32), INT("bar", "barmargin", barmargin, 0, 48),
	INT("bar", "barvmargin", barvmargin, 0, 48), INT("bar", "barseggap", barseggap, 0, 32),
	INT("bar", "segradius", segradius, 0, 32), INT("bar", "titlealign", titlealign, 0, 2),
	INT("tray", "showsystray", showsystray, 0, 1), INT("tray", "showtraybar", showtraybar, 0, 1),
	INT("tray", "systraymon", systraymon, -1, 99), INT("tray", "systrayspacing", systrayspacing, 0, 24),
	INT("tray", "systrayvertpad", systrayvertpad, 0, 24),
	INT("panels", "radius", panelradius, 0, 32),
	INT("panels", "opacity", panel_opacity, 0, 255),
	INT("panels", "calendar_width", calendar_width, 300, 900),
	INT("panels", "clock_width", clock_width, 340, 900),
	INT("panels", "palette_width", palette_width, 360, 900),
	INT("motion", "duration", duration, 0, 500), INT("motion", "reduced_motion", reduced_motion, 0, 1),
	INT("motion", "shadows", shadows, 0, 1),
	INT("motion", "shadow_opacity", shadow_opacity, 0, 255),
	INT("motion", "bar_shadow_blur", bar_shadow_blur, 1, 5),
	INT("motion", "panel_shadow_blur", panel_shadow_blur, 1, 12),
	STR("clock", "timer_hook", hook), STR("theme", "font", font), STR("theme", "monofont", monofont),
	STR("theme", "normfg", colors[SchemeNorm][ColFg]), STR("theme", "normbg", colors[SchemeNorm][ColBg]),
	STR("theme", "normborder", colors[SchemeNorm][ColBorder]),
	STR("theme", "selfg", colors[SchemeSel][ColFg]), STR("theme", "selbg", colors[SchemeSel][ColBg]),
	STR("theme", "selborder", colors[SchemeSel][ColBorder]),
	STR("theme", "urgfg", colors[SchemeUrg][ColFg]), STR("theme", "urgbg", colors[SchemeUrg][ColBg]),
	STR("theme", "urgborder", colors[SchemeUrg][ColBorder]),
	STR("theme", "titlefg", colors[SchemeTitle][ColFg]), STR("theme", "titlebg", colors[SchemeTitle][ColBg]),
	STR("theme", "statusfg", colors[SchemeStatus][ColFg]), STR("theme", "statusbg", colors[SchemeStatus][ColBg]),
	STR("theme", "barbg", colors[SchemeBar][ColBg]),
	INT("theme", "normbgalpha", alpha[SchemeNorm], 0, 255), INT("theme", "selbgalpha", alpha[SchemeSel], 0, 255),
	INT("theme", "urgbgalpha", alpha[SchemeUrg], 0, 255), INT("theme", "titlebgalpha", alpha[SchemeTitle], 0, 255),
	INT("theme", "statusbgalpha", alpha[SchemeStatus], 0, 255), INT("theme", "barbgalpha", alpha[SchemeBar], 0, 255),
};
#undef INT
#undef STR

static int
integer(const char *s, int low, int high, int *out)
{
	char *end;
	errno = 0;
	long n = strtol(s, &end, strncmp(s, "0x", 2) == 0 ? 16 : 10);
	if (errno || !s[0] || *end || n < low || n > high)
		return 0;
	*out = n;
	return 1;
}

static int
copy_value(char *dest, size_t length, const char *value)
{
	if (strlen(value) >= length)
		return 0;
	strcpy(dest, value);
	return 1;
}

static int
setting(Config *c, const Setting *s, const char *value)
{
	void *dest = (char *)c + s->offset;
	if (s->length)
		return (*value || s->offset == offsetof(Config, hook)) && copy_value(dest, s->length, value);
	if (s->offset == offsetof(Config, mfact)) {
		char *end;
		float f = strtof(value, &end);
		if (end == value || *end || !isfinite(f) || f < s->min || f > s->max)
			return 0;
		*(float *)dest = f;
		return 1;
	}
	if (s->min == 0 && s->max == 1) {
		if (!strcmp(value, "true") || !strcmp(value, "false")) {
			*(int *)dest = !strcmp(value, "true");
			return 1;
		}
	}
	return integer(value, s->min, s->max, dest);
}

static char *
trim(char *s)
{
	while (isspace((unsigned char)*s))
		++s;
	char *end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = 0;
	return s;
}

int
config_tag(const Config *c, const char *value)
{
	int id;
	if (integer(value, 1, MAX_TAGS, &id))
		return id - 1;
	for (int i = 0; i < c->ntags; ++i)
		if (c->tags[i][0] && !strcmp(value, c->tags[i]))
			return i;
	return -1;
}

static int
zone_exists(const char *name)
{
	char path[sizeof "/usr/share/zoneinfo/" + 128];
	if (!*name || *name == '/' || strstr(name, "..") || strlen(name) >= 128
	    || strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-+") != strlen(name))
		return 0;
	snprintf(path, sizeof path, "/usr/share/zoneinfo/%s", name);
	struct stat info;
	if (stat(path, &info) || !S_ISREG(info.st_mode))
		return 0;
	FILE *file = fopen(path, "rb");
	if (!file)
		return 0;
	char magic[4];
	int valid = fread(magic, 1, sizeof magic, file) == sizeof magic && !memcmp(magic, "TZif", 4);
	fclose(file);
	return valid;
}

int
config_zone_time(const char *zone, time_t now, struct tm *out)
{
	const char *tz = getenv("TZ");
	char *old = tz ? strdup(tz) : NULL;
	char path[150];
	snprintf(path, sizeof path, ":/usr/share/zoneinfo/%s", zone);
	if ((tz && !old) || setenv("TZ", path, 1)) {
		free(old);
		return 0;
	}
	tzset();
	int ok = localtime_r(&now, out) != NULL;
	if (old)
		setenv("TZ", old, 1);
	else
		unsetenv("TZ");
	tzset();
	free(old);
	return ok;
}

static int
read_rule(Rule *r, const char *key, const char *v)
{
	if (!strcmp(key, "class")) return copy_value(r->class, sizeof r->class, v);
	if (!strcmp(key, "instance")) return copy_value(r->instance, sizeof r->instance, v);
	if (!strcmp(key, "title")) return copy_value(r->title, sizeof r->title, v);
	if (!strcmp(key, "tag")) return copy_value(r->tag, sizeof r->tag, v);
	if (!strcmp(key, "type"))
		return copy_value(r->type, sizeof r->type, !strncmp(v, "_NET_WM_WINDOW_TYPE_", 20) ? v + 20 : v);
	if (!strcmp(key, "floating")) {
		if (!strcmp(v, "true") || !strcmp(v, "false")) {
			r->floating = !strcmp(v, "true");
			return 1;
		}
		return integer(v, 0, 1, &r->floating);
	}
	return 0;
}

static int
read_value(Config *c, const char *section, const char *key, const char *v, Rule *rule)
{
	int id;
	if (rule)
		return read_rule(rule, key, v);
	if (!strcmp(section, "tags") && integer(key, 1, MAX_TAGS, &id)) {
		if (id > c->ntags)
			c->ntags = id;
		return copy_value(c->tags[id - 1], sizeof c->tags[0], v);
	}
	if (!strcmp(section, "tag_monitors") && integer(key, 1, MAX_TAGS, &id))
		return integer(v, -1, 99, &c->tagmon[id - 1]);
	if (!strcmp(section, "timezones")) {
		for (id = 0; id < c->nzones; ++id)
			if (!strcmp(c->zones[id].label, key))
				break;
		if (!*key || id == MAX_ZONES || !zone_exists(v)
		    || !copy_value(c->zones[id].label, sizeof c->zones[id].label, key))
			return 0;
		strcpy(c->zones[id].name, v);
		if (id == c->nzones)
			++c->nzones;
		return 1;
	}
	if (!strcmp(section, "layout") && !strcmp(key, "default")) {
		const char *layouts[] = {"cols", "stack", "float"};
		for (id = 0; id < 3; ++id)
			if (!strcmp(v, layouts[id])) {
				c->defaultlayout = id;
				return 1;
			}
		return 0;
	}
	for (size_t i = 0; i < COUNT(settings); ++i)
		if (!strcmp(section, settings[i].section) && !strcmp(key, settings[i].key))
			return setting(c, &settings[i], v);
	return 0;
}

int
config_read(Config *c, const char *path, int required, char error[CONFIG_ERROR])
{
	FILE *file = fopen(path, "r");
	char line[1024], section[80] = "";
	int number = 0;
	int rule_lines[MAX_RULES] = {0}, tag_lines[MAX_TAGS] = {0};
	int custom_rules = 0;
	Rule *rule = NULL;
	const char *reason = NULL;
	if (!file) {
		if (!required && errno == ENOENT)
			return 1;
		snprintf(error, CONFIG_ERROR, "%s: %s", path, strerror(errno));
		return 0;
	}
	while (fgets(line, sizeof line, file)) {
		++number;
		if (!strchr(line, '\n') && !feof(file)) {
			reason = "line is too long";
			break;
		}
		char *s = trim(line);
		if (!*s || *s == '#' || *s == ';')
			continue;
		if (*s == '[') {
			char *end = strchr(++s, ']');
			if (!end || end[1]) {
				reason = "expected [section]";
				break;
			}
			*end = 0;
			if (!copy_value(section, sizeof section, trim(s))) {
				reason = "section name is too long";
				break;
			}
			rule = NULL;
			if (!strncmp(section, "rule ", 5) || !strcmp(section, "rules")) {
				if (!custom_rules++)
					c->nrules = 0;
				if (!strcmp(section, "rules"))
					continue;
				if (c->nrules == MAX_RULES || !section[5]) {
					reason = "invalid rule name or too many rules";
					break;
				}
				rule_lines[c->nrules] = number;
				rule = &c->rules[c->nrules++];
				*rule = (Rule){.floating = -1};
			} else {
				const char *sections[] = {"bar", "layout", "theme", "motion", "tray", "panels", "clock",
				                          "timezones", "tags", "tag_monitors"};
				size_t i = 0;
				while (i < COUNT(sections) && strcmp(section, sections[i]))
					++i;
				if (i == COUNT(sections)) {
					reason = "unknown section";
					break;
				}
			}
			continue;
		}
		char *equals = strchr(s, '=');
		if (!equals) {
			reason = "expected key = value";
			break;
		}
		*equals = 0;
		const char *key = trim(s);
		if (!read_value(c, section, key, trim(equals + 1), rule)) {
			reason = "unknown key or invalid value";
			break;
		}
		int tag;
		if (!strcmp(section, "tags") && integer(key, 1, MAX_TAGS, &tag))
			tag_lines[tag - 1] = number;
	}
	if (ferror(file))
		reason = "read failed";
	fclose(file);
	for (int i = 0; !reason && i < c->nrules; ++i)
		if (c->rules[i].tag[0] && config_tag(c, c->rules[i].tag) < 0) {
			number = rule_lines[i];
			reason = "rule refers to an unknown tag";
		}
	for (int i = 0; !reason && i < c->ntags; ++i) {
		if (!c->tags[i][0])
			continue;
		int id;
		if ((integer(c->tags[i], 1, MAX_TAGS, &id) && id != i + 1)
		    || config_tag(c, c->tags[i]) != i) {
			reason = "duplicate tag name or name conflicts with a numeric tag ID";
			number = tag_lines[i];
			if (!number)
				for (int j = 0; j < i; ++j)
					if (!strcmp(c->tags[j], c->tags[i]))
						number = tag_lines[j];
		}
	}
	if (reason)
		snprintf(error, CONFIG_ERROR, "%s:%d: %s", path, number, reason);
	return !reason;
}

int
config_theme(Config *c, Display *dpy, Window root, Colormap cmap, char error[CONFIG_ERROR])
{
	Atom type, resource = XInternAtom(dpy, "RESOURCE_MANAGER", False);
	int format, ok = 1;
	unsigned long length, rest;
	unsigned char *data = NULL;

	XrmInitialize();
	if (XGetWindowProperty(dpy, root, resource, 0, 262144, False, XA_STRING,
	                       &type, &format, &length, &rest, &data) == Success && data) {
		XrmDatabase db = type == XA_STRING && format == 8 && !rest
		                 ? XrmGetStringDatabase((char *)data) : NULL;
		for (size_t i = 0; db && i < COUNT(settings); ++i) {
			char name[96], klass[96], *resource_type;
			XrmValue value;
			snprintf(name, sizeof name, "mwm.%s", settings[i].key);
			snprintf(klass, sizeof klass, "Mwm.%s", settings[i].key);
			if ((!strcmp(settings[i].section, "theme") || !strcmp(settings[i].section, "bar")
			     || (settings[i].offset == offsetof(Config, borderpx)
			         || settings[i].offset == offsetof(Config, gappx)
			         || settings[i].offset == offsetof(Config, cornerradius)
			         || settings[i].offset == offsetof(Config, mfact)))
			    && XrmGetResource(db, name, klass, &resource_type, &value)
			    && !strcmp(resource_type, "String") && !setting(c, &settings[i], value.addr)) {
				snprintf(error, CONFIG_ERROR, "invalid Xresource %s", name);
				ok = 0;
				break;
			}
		}
		if (db)
			XrmDestroyDatabase(db);
		XFree(data);
	}
	for (int i = 0; ok && i < SchemeLast; ++i)
		for (int j = 0; ok && j < 3; ++j) {
			XColor color;
			if (!XParseColor(dpy, cmap, c->colors[i][j], &color)) {
				snprintf(error, CONFIG_ERROR, "invalid theme color: %s", c->colors[i][j]);
				ok = 0;
			} else
				c->palette[i][j] = (cui_color){color.red >> 8, color.green >> 8,
				                             color.blue >> 8, j == ColBg ? c->alpha[i] : 255};
		}
	return ok;
}
