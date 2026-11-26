#include "activity.h"

#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
	uint64_t value[2], devices;
} Counters;

static int
read_fields(const char *text, unsigned long long *values, int n)
{
	for (int i = 0; i < n; ++i) {
		while (isspace((unsigned char)*text))
			++text;
		if (!isdigit((unsigned char)*text))
			return 0;
		char *end;
		values[i] = strtoull(text, &end, 10);
		text = end;
	}
	return 1;
}

static int
read_counter(const char *path, uint64_t *value)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return 0;
	unsigned long long number;
	int ok = fscanf(file, "%llu", &number) == 1;
	fclose(file);
	if (ok)
		*value = number;
	return ok;
}

static void
add(Counters *c, const char *device, uint64_t first, uint64_t second)
{
	/* Keep a stable identity when interfaces or disks appear or disappear. */
	uint64_t name = 5381;
	for (const unsigned char *p = (const unsigned char *)device; *p; ++p)
		name = name*33 + *p;
	c->devices += name;
	c->value[0] += first;
	c->value[1] += second;
}

static Counters
network_counters(const char *preferred)
{
	Counters physical = {0}, fallback = {0};
	FILE *file = fopen("/proc/net/dev", "r");
	if (!file)
		return physical;
	char line[512];
	while (fgets(line, sizeof line, file)) {
		char *name = line + strspn(line, " \t"), *colon = strchr(name, ':');
		if (!colon)
			continue;
		*colon = 0;
		unsigned long long fields[9];
		if ((*preferred ? strcmp(name, preferred) : !strcmp(name, "lo")) ||
		    !read_fields(colon+1, fields, 9))
			continue;
		char path[sizeof line + 24];
		snprintf(path, sizeof path, "/sys/class/net/%s/device", name);
		add(*preferred || access(path, F_OK) == 0 ? &physical : &fallback, name, fields[0], fields[8]);
	}
	fclose(file);
	return physical.devices ? physical : fallback;
}

static Counters
disk_counters(const char *preferred)
{
	Counters counters = {0};
	FILE *file = fopen("/proc/diskstats", "r");
	if (!file)
		return counters;
	char line[512], name[64];
	while (fgets(line, sizeof line, file)) {
		int offset = 0;
		if (sscanf(line, "%*u %*u %63s%n", name, &offset) != 1 ||
		    (*preferred && strcmp(name, preferred)))
			continue;
		char path[96];
		snprintf(path, sizeof path, "/sys/block/%s/device", name);
		if (!*preferred && access(path, F_OK) != 0)
			continue;
		unsigned long long fields[7];
		if (read_fields(line+offset, fields, 7))
			add(&counters, name, fields[2], fields[6]);
	}
	fclose(file);
	return counters;
}

static void
sample_traffic(Traffic *traffic, Counters counters, double tick, double units)
{
	if (!counters.devices || counters.devices != traffic->devices ||
	    counters.value[0] < traffic->previous[0] || counters.value[1] < traffic->previous[1]) {
		traffic->count = 0;
	} else if (tick > traffic->tick) {
		if (traffic->count == TRAFFIC_SAMPLES) {
			--traffic->count;
			memmove(traffic->history, traffic->history+1, traffic->count * sizeof traffic->history[0]);
		}
		for (int i = 0; i < 2; ++i)
			traffic->history[traffic->count][i] = units * (counters.value[i] - traffic->previous[i]) / (tick - traffic->tick);
		++traffic->count;
	}
	traffic->tick = tick;
	traffic->devices = counters.devices;
	memcpy(traffic->previous, counters.value, sizeof traffic->previous);
}

static void
read_sensor_text(const char *path, char *text, size_t size)
{
	FILE *file = fopen(path, "r");
	if (file && fgets(text, size, file))
		text[strcspn(text, "\r\n")] = 0;
	else
		*text = 0;
	if (file)
		fclose(file);
}

static int
read_temperature(const char *path, int *value)
{
	char text[64], *end;
	read_sensor_text(path, text, sizeof text);
	errno = 0;
	long number = strtol(text, &end, 10);
	if (errno || end == text || *end || number < INT_MIN || number > INT_MAX)
		return 0;
	*value = number;
	return 1;
}

static int
use_temperature(Temperature *sensor, const char *path)
{
	if (strlen(path) >= sizeof sensor->path || !read_temperature(path, &sensor->millidegrees))
		return 0;
	strcpy(sensor->path, path);
	return 1;
}

static void
find_temperature(Temperature *sensor, const char *pattern)
{
	glob_t inputs = {0};
	if (glob(pattern, 0, NULL, &inputs) == 0)
		for (size_t i = 0; i < inputs.gl_pathc; ++i)
			if (use_temperature(sensor, inputs.gl_pathv[i]))
				break;
	globfree(&inputs);
}

static void
find_cpu_temperature(Temperature *sensor)
{
	glob_t devices = {0};
	if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &devices) == 0)
		for (size_t i = 0; i < devices.gl_pathc && !sensor->path[0]; ++i) {
			const char *device = devices.gl_pathv[i];
			char path[sizeof sensor->path], name[64];
			snprintf(path, sizeof path, "%s/name", device);
			read_sensor_text(path, name, sizeof name);
			int amd = !strcmp(name, "k10temp");
			if (!amd && strcmp(name, "coretemp"))
				continue;
			if (amd) {
				char label[64];
				snprintf(path, sizeof path, "%s/temp2_label", device);
				read_sensor_text(path, label, sizeof label);
				snprintf(path, sizeof path, "%s/temp2_input", device);
				if (!strcmp(label, "Tdie") && use_temperature(sensor, path))
					break;
			}
			snprintf(path, sizeof path, "%s/temp1_input", device);
			use_temperature(sensor, path);
		}
	globfree(&devices);
	if (sensor->path[0])
		return;
	if (glob("/sys/class/thermal/thermal_zone*", 0, NULL, &devices) == 0)
		for (size_t i = 0; i < devices.gl_pathc && !sensor->path[0]; ++i) {
			const char *zone = devices.gl_pathv[i];
			char path[sizeof sensor->path], type[64];
			snprintf(path, sizeof path, "%s/type", zone);
			read_sensor_text(path, type, sizeof type);
			if (!strcmp(type, "x86_pkg_temp") || !strcmp(type, "cpu-thermal") ||
			    !strcmp(type, "cpu_thermal") || !strcmp(type, "TCPU")) {
				snprintf(path, sizeof path, "%s/temp", zone);
				use_temperature(sensor, path);
			}
		}
	globfree(&devices);
}

static int
is_gpu_card(const char *name)
{
	return !strncmp(name, "card", 4) && name[4] && !name[4+strspn(name+4, "0123456789")];
}

static void
sample_temperature(Temperature *sensor, double tick, const char *card)
{
	if (sensor->path[0] && read_temperature(sensor->path, &sensor->millidegrees))
		return;
	sensor->path[0] = 0;
	if (tick < sensor->probe)
		return;
	sensor->probe = tick + 5;
	if (!card) {
		find_cpu_temperature(sensor);
	} else if (!*card || is_gpu_card(card)) {
		char pattern[sizeof sensor->path];
		snprintf(pattern, sizeof pattern, "/sys/class/drm/%s/device/hwmon/hwmon*/temp1_input",
		         *card ? card : "card[0-9]*");
		find_temperature(sensor, pattern);
	}
}

static void
find_gpu(Activity *activity, const char *preferred)
{
	glob_t cards = {0};
	static const char *suffix[] = {"device/gpu_busy_percent", "gt/gt0/rc6_residency_ms", "power/rc6_residency_ms"};
	char card[sizeof activity->gpu_card] = "";
	activity->gpu_kind = GpuUnavailable;
	activity->gpu_tick = 0;
	if (glob("/sys/class/drm/card[0-9]*", 0, NULL, &cards) == 0) {
		for (int j = 0; j < 3 && !activity->gpu_kind; ++j)
			for (size_t i = 0; i < cards.gl_pathc; ++i) {
				const char *path = cards.gl_pathv[i], *name = strrchr(path, '/') + 1;
				if (!is_gpu_card(name) || (*preferred && strcmp(name, preferred)))
					continue;
				if (j) {
					char enabled[sizeof activity->gpu_path];
					int size = snprintf(enabled, sizeof enabled, "%s/%s/rc6_enable", path,
					                    j == 1 ? "gt/gt0" : "power");
					uint64_t on;
					if (size < (int)sizeof enabled && read_counter(enabled, &on) && !on)
						continue;
				}
				int n = snprintf(activity->gpu_path, sizeof activity->gpu_path, "%s/%s", path, suffix[j]);
				uint64_t value;
				if (n < (int)sizeof activity->gpu_path && read_counter(activity->gpu_path, &value)) {
					activity->gpu_kind = j ? GpuAwake : GpuBusy;
					snprintf(card, sizeof card, "%s", name);
					break;
				}
			}
	}
	globfree(&cards);
	if (strcmp(activity->gpu_card, card)) {
		strcpy(activity->gpu_card, card);
		activity->gpu_temperature = (Temperature){0};
	}
}

double
activity_sample(Activity *activity, const Config *config, unsigned items, double tick)
{
	if (items & (1u << BarNetwork))
		sample_traffic(&activity->network, network_counters(config->network_interface), tick, 1);
	if (items & (1u << BarDisk))
		sample_traffic(&activity->disk, disk_counters(config->disk_device), tick, 512);
	if (items & (1u << BarCpu))
		sample_temperature(&activity->cpu_temperature, tick, NULL);
	if (!(items & (1u << BarGpu)))
		return -1;
	if (!activity->gpu_kind && tick >= activity->gpu_probe) {
		find_gpu(activity, config->gpu_device);
		activity->gpu_probe = tick + 5;
	}
	sample_temperature(&activity->gpu_temperature, tick,
	                   *config->gpu_device ? config->gpu_device : activity->gpu_card);
	uint64_t value;
	if (!activity->gpu_kind || !read_counter(activity->gpu_path, &value)) {
		activity->gpu_kind = GpuUnavailable;
		return -1;
	}
	if (activity->gpu_kind == GpuBusy)
		return fmin(100, value);
	double result = activity->gpu_tick && tick > activity->gpu_tick && value >= activity->gpu_previous
	                ? fmax(0, 100 - (value - activity->gpu_previous) / (10 * (tick - activity->gpu_tick))) : -1;
	activity->gpu_previous = value;
	activity->gpu_tick = tick;
	return result;
}
