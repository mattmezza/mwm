#include "controls.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum { ReadControl = -1, ToggleControl = -2 };

static int
attribute(const char *device, const char *key, char *out, size_t size)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", device, key);
	FILE *file = fopen(path, "r");
	if (!file)
		return 0;
	int ok = fgets(out, size, file) != NULL;
	fclose(file);
	if (ok)
		out[strcspn(out, "\r\n")] = 0;
	return ok;
}

static int
number(const char *device, const char *key)
{
	char text[48], *end;
	if (!attribute(device, key, text, sizeof text))
		return -1;
	long value = strtol(text, &end, 10);
	return !*text || *end || value < 0 || value > INT_MAX ? -1 : (int)value;
}

static int
find_device(const char *base, const char *preferred, const char *probe, char device[384])
{
	DIR *dir = opendir(base);
	if (!dir)
		return 0;
	struct dirent *entry;
	int found = 0;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.' || (*preferred && strcmp(entry->d_name, preferred)))
			continue;
		snprintf(device, 384, "%s/%s", base, entry->d_name);
		if (number(device, probe) >= 0) {
			found = 1;
			break;
		}
	}
	closedir(dir);
	return found;
}

/* Only the background worker waits for helpers. */
static int
run(char *const argv[], char *output, size_t size)
{
	int pipefd[2];
	if (pipe(pipefd) < 0)
		return 0;
	pid_t child = fork();
	if (!child) {
		int quiet = open("/dev/null", O_WRONLY);
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		if (quiet >= 0) {
			dup2(quiet, STDERR_FILENO);
			close(quiet);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	char chunk[128];
	size_t stored = 0;
	ssize_t bytes;
	while (child > 0 && (bytes = read(pipefd[0], chunk, sizeof chunk)) != 0) {
		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (size > stored + 1) {
			size_t copy = MIN((size_t)bytes, size - stored - 1);
			memcpy(output + stored, chunk, copy);
			stored += copy;
		}
	}
	close(pipefd[0]);
	if (size)
		output[stored] = 0;
	int status;
	pid_t done;
	do {
		done = child > 0 ? waitpid(child, &status, 0) : -1;
	} while (done < 0 && child > 0 && errno == EINTR);
	return child > 0 && done == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static ControlReading
read_battery(const Config *config, int request)
{
	ControlReading data = {.limit = -1};
	char device[384], path[456];
	if (!find_device("/sys/class/power_supply", config->battery_device, "capacity", device))
		return data;
	int capacity = number(device, "capacity");
	if (capacity < 0)
		return data;
	data.available = 1;
	data.level = MIN(capacity, 100);
	data.limit = number(device, "charge_control_end_threshold");
	int start = number(device, "charge_control_start_threshold");
	data.minimum = MAX(1, MIN(99, start) + 1);
	snprintf(path, sizeof path, "%s/charge_control_end_threshold", device);
	data.adjustable = data.limit >= data.minimum && data.limit <= 100 && access(path, W_OK) == 0;
	if (request >= 0) {
		FILE *file = data.adjustable ? fopen(path, "w") : NULL;
		if (file) {
			int wrote = fprintf(file, "%d\n", MAX(data.minimum, MIN(request, 100)));
			data.failed = fclose(file) != 0 || wrote < 0;
		} else {
			data.failed = 1;
		}
		data.limit = number(device, "charge_control_end_threshold");
	}
	attribute(device, "status", data.detail, sizeof data.detail);
	data.charging = !strcmp(data.detail, "Charging");
	return data;
}

static ControlReading
read_brightness(const Config *config, int request, int restore)
{
	ControlReading data = {.minimum = config->brightness_min, .brightness_restore = restore};
	char device[384];
	if (!find_device("/sys/class/backlight", config->backlight_device, "max_brightness", device))
		return data;
	int max = number(device, "max_brightness");
	if (max <= 0)
		return data;
	int current = number(device, "brightness");
	if (current > 0)
		data.brightness_restore = current;
	if (request != ReadControl) {
		char level[16];
		int raw = request == ToggleControl
		          ? (current > 0 ? 0 : restore > 0 ? MIN(restore, max) : MAX(1, max / 2))
		          : MAX(1, (int)round((double)max * MAX(config->brightness_min, request) / 100));
		snprintf(level, sizeof level, "%d", raw);
		char *args[] = {"brightnessctl", "-q", "-c", "backlight", "-d", strrchr(device, '/') + 1,
		                "-n0", "set", level, NULL};
		data.failed = current < 0 || !run(args, NULL, 0);
		current = number(device, "brightness");
		if (request == ToggleControl && current != raw)
			data.failed = 1;
	}
	if (current > 0)
		data.brightness_restore = current;
	data.available = current >= 0;
	data.adjustable = data.available;
	data.level = current < 0 ? 0 : MIN(100, (int)round(100.0 * current / max));
	return data;
}

static ControlReading
read_volume(const Config *config, int request)
{
	ControlReading data = {0};
	char *sink = (char *)config->audio_sink;
	if (request == ToggleControl) {
		char *mute[] = {"wpctl", "set-mute", "--", sink, "toggle", NULL};
		data.failed = !run(mute, NULL, 0);
	} else if (request >= 0) {
		char level[16];
		snprintf(level, sizeof level, "%d%%", request);
		char *set[] = {"wpctl", "set-volume", "--limit", "1.0", "--", sink, level, NULL};
		data.failed = !run(set, NULL, 0);
	}
	char output[128];
	char *get[] = {"wpctl", "get-volume", "--", sink, NULL};
	double volume;
	if (run(get, output, sizeof output) && sscanf(output, "Volume: %lf", &volume) == 1
	    && isfinite(volume) && volume >= 0) {
		data.available = data.adjustable = 1;
		data.level = (int)round(100 * fmin(volume, 1.0));
		data.muted = strstr(output, "[MUTED]") != NULL;
		if (data.muted && request > 0 && !data.failed) {
			char *unmute[] = {"wpctl", "set-mute", "--", sink, "0", NULL};
			data.failed = !run(unmute, NULL, 0);
			data.muted = data.failed;
		}
	}
	return data;
}

void
controls_init(Controls *controls)
{
	*controls = (Controls){0};
	for (int i = 0; i < ControlCount; ++i)
		controls->jobs[i] = (ControlJob){.fd = -1, .queued = ReadControl, .applying = ReadControl};
}

void
controls_reset(Controls *controls)
{
	for (int i = 0; i < ControlCount; ++i) {
		ControlJob *job = &controls->jobs[i];
		if (job->fd >= 0) {
			kill(-job->pid, SIGKILL);
			close(job->fd);
		}
	}
	controls_init(controls);
}

static int
start(Controls *controls, enum Control kind, const Config *config, int xfd, double tick)
{
	ControlJob *job = &controls->jobs[kind];
	int fds[2];
	if (pipe(fds) < 0)
		return 0;
	if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
	    fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
		close(fds[0]);
		close(fds[1]);
		return 0;
	}
	int request = job->queued;
	if (request == ReadControl && job->toggle)
		request = ToggleControl;
	pid_t pid = fork();
	if (!pid) {
		setpgid(0, 0);
		signal(SIGCHLD, SIG_DFL);
		close(xfd);
		close(fds[0]);
		ControlReading data = kind == ControlBattery ? read_battery(config, request)
		                      : kind == ControlVolume ? read_volume(config, request)
		                                              : read_brightness(config, request,
		                                                  controls->reading[kind].brightness_restore);
		ssize_t written;
		do {
			written = write(fds[1], &data, sizeof data);
		} while (written < 0 && errno == EINTR);
		_exit(written == sizeof data ? 0 : 1);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return 0;
	}
	setpgid(pid, pid);
	job->fd = fds[0];
	job->pid = pid;
	job->applying = request;
	if (request == ToggleControl)
		job->toggle = 0;
	job->queued = ReadControl;
	job->deadline = tick + 2;
	return 1;
}

int
controls_tick(Controls *controls, const Config *config, unsigned enabled, int xfd, double tick, int *changed)
{
	int timeout = 1000;
	for (int i = 0; i < ControlCount; ++i) {
		ControlJob *job = &controls->jobs[i];
		ControlReading *old = &controls->reading[i];
		if (job->fd >= 0) {
			ControlReading data = {0};
			ssize_t bytes = read(job->fd, &data, sizeof data);
			if (bytes < 0 && (errno == EAGAIN || errno == EINTR) && tick < job->deadline) {
				timeout = MIN(timeout, 30);
				continue;
			}
			if (bytes < 0 && tick >= job->deadline)
				kill(-job->pid, SIGKILL);
			close(job->fd);
			job->fd = -1;
			if (bytes != sizeof data) {
				data = *old;
				data.failed = job->applying != ReadControl;
				if (job->applying == ReadControl)
					data.available = data.adjustable = 0;
			}
			if (job->applying == ReadControl)
				data.failed = old->failed;
			*changed |= memcmp(old, &data, sizeof data) != 0;
			*old = data;
			job->applying = ReadControl;
		}
		int pending = job->queued != ReadControl || job->toggle;
		if (!(enabled & (1u << i)) && !pending)
			continue;
		if (pending || tick >= job->due) {
			job->due = tick + config->controls_interval / 1000.0;
			if (i != ControlVolume && !pending) {
				ControlReading data = i == ControlBattery ? read_battery(config, ReadControl)
				                      : read_brightness(config, ReadControl, old->brightness_restore);
				data.failed = old->failed;
				*changed |= memcmp(old, &data, sizeof data) != 0;
				*old = data;
			} else if (start(controls, i, config, xfd, tick))
				timeout = MIN(timeout, 30);
			else if (pending) {
				job->queued = ReadControl;
				job->toggle = 0;
				old->failed = *changed = 1;
			}
		}
		timeout = MIN(timeout, MAX(1, (int)ceil(1000 * (job->due - tick))));
	}
	return timeout;
}

int
controls_value(const Controls *controls, enum Control kind)
{
	const ControlJob *job = &controls->jobs[kind];
	const ControlReading *data = &controls->reading[kind];
	if (job->queued >= 0)
		return job->queued;
	if (job->applying >= 0)
		return job->applying;
	return kind == ControlBattery && data->limit >= 0 ? data->limit : data->level;
}

void
controls_set(Controls *controls, enum Control kind, int value)
{
	ControlReading *data = &controls->reading[kind];
	if (data->available && data->adjustable) {
		ControlJob *job = &controls->jobs[kind];
		job->queued = MAX(data->minimum, MIN(value, 100));
		if (kind == ControlBrightness || (kind == ControlVolume && job->queued > 0))
			job->toggle = 0;
	}
}

void
controls_toggle(Controls *controls, enum Control kind)
{
	if ((kind == ControlVolume || kind == ControlBrightness) && controls->reading[kind].adjustable)
		controls->jobs[kind].toggle ^= 1;
}

int
controls_step(const Config *config, enum Control kind)
{
	const int steps[] = {config->battery_step, config->volume_step, config->brightness_step};
	return steps[kind];
}
