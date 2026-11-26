#ifndef MWM_ACTIVITY_H
#define MWM_ACTIVITY_H

#include "mwm_config.h"
#include <stdint.h>

enum { TRAFFIC_SAMPLES = 12 };
enum { GpuUnavailable, GpuBusy, GpuAwake };

typedef struct {
	double history[TRAFFIC_SAMPLES][2];
	int count;
	double tick;
	uint64_t previous[2], devices;
} Traffic;

typedef struct {
	char path[256];
	int millidegrees;
	double probe;
} Temperature;

typedef struct {
	Traffic network, disk;
	Temperature cpu_temperature, gpu_temperature;
	char gpu_path[256], gpu_card[32];
	int gpu_kind;
	uint64_t gpu_previous;
	double gpu_tick, gpu_probe;
} Activity;

double activity_sample(Activity *, const Config *, unsigned items, double tick);

#endif
