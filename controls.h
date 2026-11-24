#ifndef MWM_CONTROLS_H
#define MWM_CONTROLS_H

#include "mwm_config.h"
#include <sys/types.h>

enum Control { ControlBattery, ControlVolume, ControlBrightness, ControlCount };

typedef struct {
	int available, adjustable, level, minimum, limit, charging, muted, failed;
	int brightness_restore, battery_seconds;
	char detail[32];
} ControlReading;

typedef struct {
	int fd, queued, applying, toggle;
	pid_t pid;
	double due, deadline;
} ControlJob;

typedef struct {
	ControlReading reading[ControlCount];
	ControlJob jobs[ControlCount];
} Controls;

void controls_init(Controls *);
void controls_reset(Controls *);
int controls_tick(Controls *, const Config *, unsigned enabled, int xfd, double tick, int *changed);
int controls_value(const Controls *, enum Control);
void controls_set(Controls *, enum Control, int value);
void controls_toggle(Controls *, enum Control);
int controls_step(const Config *, enum Control);

#endif
