#include "tray.h"

#include <cJSON.h>
#include <string.h>

enum DisplayRequest { ReadStatus, ReadPresets, ApplyAction };

typedef struct {
	Tray *tray;
	enum DisplayRequest request;
	GSubprocess *process;
	GCancellable *cancel;
	guint timeout;
	gboolean expired;
} DisplayJob;

typedef struct {
	DisplayJob *job;
	gint64 next_poll;
} Displays;

static const struct {
	int bit, minimum, maximum;
	const char *command, *label;
	enum TrayIcon icon;
} quick[] = {
	{DisplayRow, 2, G_MAXINT, "row", "Side by side", TrayDisplayRow},
	{DisplayColumn, 2, G_MAXINT, "column", "Stacked", TrayDisplayColumn},
	{DisplayTwoAbove, 3, 3, "y", "Two above, one below", TrayDisplayTwoAbove},
};

static gboolean run(Tray *, enum DisplayRequest, const char *, const char *);

static void
quick_rows(Tray *tray)
{
	g_ptr_array_set_size(tray->rows, 0);
	for (size_t i = 0; i < G_N_ELEMENTS(quick); ++i)
		if ((tray->config.display_quick_presets & quick[i].bit) &&
		    tray->display_arrangeable >= quick[i].minimum && tray->display_arrangeable <= quick[i].maximum)
			tray_entry(tray, quick[i].command, quick[i].label, "Quick preset", quick[i].icon, FALSE, FALSE, 0);
}

static const cJSON *
member(const cJSON *object, const char *key)
{
	return cJSON_GetObjectItemCaseSensitive(object, key);
}

static gboolean
aligned(const cJSON *first, const cJSON *output)
{
	const cJSON *position = member(first, "position"), *mode = member(first, "mode");
	const cJSON *other = member(output, "mode");
	return cJSON_Compare(position, member(output, "position"), 1) &&
	       cJSON_Compare(member(mode, "width"), member(other, "width"), 1) &&
	       cJSON_Compare(member(mode, "height"), member(other, "height"), 1) &&
	       cJSON_Compare(member(first, "rotation"), member(output, "rotation"), 1);
}

static gboolean
read_status(Tray *tray, const cJSON *root)
{
	const cJSON *outputs = member(root, "outputs"), *seconds = member(root, "confirmation_seconds");
	if (!cJSON_IsArray(outputs) || !cJSON_IsNumber(seconds) || seconds->valueint < 0)
		return FALSE;
	const cJSON *output, *first = NULL;
	int active = 0, arrangeable = 0;
	gboolean external = FALSE, mirrored = TRUE;
	cJSON_ArrayForEach(output, outputs) {
		const cJSON *internal = member(output, "internal"), *enabled = member(output, "enabled");
		if (!cJSON_IsBool(internal) || !cJSON_IsBool(enabled))
			return FALSE;
		external |= cJSON_IsFalse(internal);
		arrangeable += cJSON_GetArraySize(member(output, "modes")) > 0;
		if (cJSON_IsTrue(enabled)) {
			++active;
			if (!first)
				first = output;
			else
				mirrored &= aligned(first, output);
		}
	}
	tray->display_external = external;
	tray->display_count = cJSON_GetArraySize(outputs);
	tray->display_arrangeable = arrangeable;
	tray->display_mirrored = active >= 2 && mirrored;
	tray->display_remaining = seconds->valueint;
	tray->available = tray->enabled = TRUE;
	g_autofree char *summary = tray->display_mirrored ? g_strdup_printf("%d connected · Mirrored", tray->display_count) :
	                           g_strdup_printf("%d connected · %d active", tray->display_count, active);
	tray_model(tray, summary, "");
	return TRUE;
}

static gboolean
read_presets(Tray *tray, const cJSON *root)
{
	const cJSON *presets = member(root, "presets"), *preset;
	if (!cJSON_IsArray(presets))
		return FALSE;
	quick_rows(tray);
	cJSON_ArrayForEach(preset, presets) {
		const char *name = cJSON_GetStringValue(member(preset, "name"));
		if (!name || !*name || !cJSON_IsTrue(member(preset, "matches")))
			continue;
		g_autofree char *id = g_strconcat("preset:", name, NULL);
		tray_entry(tray, id, name, "Saved preset", TrayMonitor, FALSE, FALSE, 0);
	}
	return TRUE;
}

static void
unavailable(Tray *tray)
{
	tray->available = tray->enabled = FALSE;
	tray->display_count = tray->display_arrangeable = tray->display_remaining = 0;
	tray->display_mirrored = FALSE;
	tray_model(tray, "Undocked unavailable", "");
	quick_rows(tray);
}

static gboolean
timed_out(gpointer data)
{
	DisplayJob *job = data;
	job->expired = TRUE;
	job->timeout = 0;
	g_subprocess_force_exit(job->process);
	g_cancellable_cancel(job->cancel);
	return G_SOURCE_REMOVE;
}

static void
cancel_job(Displays *state)
{
	DisplayJob *job = g_steal_pointer(&state->job);
	if (job) {
		job->tray = NULL;
		g_subprocess_force_exit(job->process);
		g_cancellable_cancel(job->cancel);
	}
}

static void
command_failed(Tray *tray, enum DisplayRequest request, const char *message)
{
	if (request == ReadStatus)
		unavailable(tray);
	else if (request == ReadPresets)
		quick_rows(tray);
	tray->busy = FALSE;
	tray_status(tray, TRUE, message);
}

static char *
command_text(GBytes *bytes)
{
	gsize length;
	const char *text = bytes ? g_bytes_get_data(bytes, &length) : NULL;
	return text ? g_strndup(text, length) : g_strdup("");
}

static void
command_done(GObject *source, GAsyncResult *result, gpointer data)
{
	DisplayJob *job = data;
	Tray *tray = job->tray;
	enum DisplayRequest request = job->request;
	g_autoptr(GBytes) output = NULL, diagnostics = NULL;
	g_autoptr(GError) error = NULL;
	gboolean done = g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &output, &diagnostics, &error);
	gboolean success = done && g_subprocess_get_successful(job->process);
	gboolean expired = job->expired;
	if (job->timeout)
		g_source_remove(job->timeout);
	g_object_unref(job->cancel);
	g_object_unref(job->process);
	g_free(job);
	if (!tray)
		return;
	Displays *state = tray->backend;
	state->job = NULL;
	if (!success) {
		g_autofree char *message = command_text(diagnostics);
		g_strstrip(message);
		command_failed(tray, request, expired ? "Undocked did not respond" : *message ? message :
		               error ? error->message : "Undocked command failed");
	} else if (request != ApplyAction) {
		g_autofree char *text = command_text(output);
		cJSON *root = cJSON_ParseWithOpts(text, NULL, 1);
		const cJSON *version = member(root, "version");
		gboolean recovered = !tray->available && request == ReadStatus;
		if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
		    !(request == ReadStatus ? read_status(tray, root) : read_presets(tray, root))) {
			command_failed(tray, request, "Unsupported JSON response from Undocked");
		} else if (request == ReadStatus) {
			tray->busy = FALSE;
			if (recovered)
				tray_status(tray, FALSE, "");
			if (tray_panel_open(tray) && tray->config.display_saved_presets && !tray->display_remaining)
				run(tray, ReadPresets, "presets", "--json");
			else
				quick_rows(tray);
			tray_changed(tray);
		} else {
			tray_changed(tray);
		}
		cJSON_Delete(root);
	}
	if (request == ApplyAction)
		run(tray, ReadStatus, "status", "--json");
}

static gboolean
run(Tray *tray, enum DisplayRequest request, const char *verb, const char *argument)
{
	Displays *state = tray->backend;
	const char *argv[] = {tray->config.undocked_command, verb, argument, NULL};
	g_autoptr(GError) error = NULL;
	if (request == ReadStatus)
		state->next_poll = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
	GSubprocess *process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
	                                       &error);
	if (!process) {
		command_failed(tray, request, error->message);
		return FALSE;
	}
	DisplayJob *job = g_new0(DisplayJob, 1);
	*job = (DisplayJob){.tray = tray, .request = request, .process = process, .cancel = g_cancellable_new()};
	state->job = job;
	job->timeout = g_timeout_add_seconds(5, timed_out, job);
	g_subprocess_communicate_async(process, NULL, job->cancel, command_done, job);
	return TRUE;
}

void
displays_tick(Tray *tray)
{
	Displays *state = tray->backend;
	if (tray->config.display_visibility != DisplaysNever && !state->job &&
	    (tray_panel_open(tray) || g_get_monotonic_time() >= state->next_poll))
		run(tray, ReadStatus, "status", "--json");
}

void
displays_start(Tray *tray)
{
	tray->backend = g_new0(Displays, 1);
	displays_tick(tray);
}

void
displays_stop(Tray *tray)
{
	cancel_job(tray->backend);
	g_clear_pointer(&tray->backend, g_free);
}

void
displays_configure(Tray *tray, gboolean command_changed)
{
	Displays *state = tray->backend;
	if (!state)
		return;
	if (command_changed || tray->config.display_visibility == DisplaysNever) {
		cancel_job(state);
		tray->display_external = tray->busy = FALSE;
		unavailable(tray);
		tray_status(tray, FALSE, "");
	} else if (state->job && state->job->request == ReadPresets) {
		cancel_job(state);
	}
	quick_rows(tray);
	displays_show(tray);
}

void
displays_show(Tray *tray)
{
	Displays *state = tray->backend;
	state->next_poll = 0;
	displays_tick(tray);
}

void
displays_action(Tray *tray, enum TrayAction action, const char *id)
{
	if (action == TrayOpenEditor) {
		g_autoptr(GError) error = NULL;
		g_autoptr(GSubprocess) editor = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
		                                                tray->config.undocked_command, NULL);
		if (editor)
			tray_hide(tray);
		else
			tray_status(tray, TRUE, error->message);
		return;
	}
	if (!tray->available)
		return;
	const char *verb = NULL, *argument = NULL;
	if (tray->display_remaining) {
		if (action == TrayKeep)
			verb = "keep";
		else if (action == TrayRevert)
			verb = "revert";
	} else if (action == TrayIdentify && tray->config.display_identify && tray->display_count) {
		verb = "identify";
	} else if (action == TrayMirror && tray->config.display_mirror && tray->display_arrangeable > 1) {
		verb = "layout";
		argument = tray->display_mirrored ? "extend" : "mirror";
	} else if (action == TrayRow && id) {
		for (guint i = 0; i < tray->rows->len; ++i) {
			const TrayEntry *row = tray->rows->pdata[i];
			if (!strcmp(id, row->id)) {
				gboolean saved = g_str_has_prefix(id, "preset:");
				verb = saved ? "preset" : "layout";
				argument = saved ? id + strlen("preset:") : id;
				break;
			}
		}
	}
	if (verb) {
		cancel_job(tray->backend);
		tray->busy = run(tray, ApplyAction, verb, argument);
		tray_changed(tray);
	}
}
