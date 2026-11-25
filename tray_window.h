#ifndef MWM_TRAY_WINDOW_H
#define MWM_TRAY_WINDOW_H

#include "tray.h"
#include "cui/cui_icon.h"

enum TrayHitKind { HitRadio, HitScan, HitRow, HitForget, HitField, HitAccept, HitCancel, HitWifi,
                 HitVpn, HitHidden, HitVisible, HitClear, HitBack };
typedef struct {
	cui_rect rect;
	enum TrayHitKind kind;
	int value;
	char *row;
} TrayHit;

enum {
	GlyphWifi0, GlyphWifi1, GlyphWifi2, GlyphWifi3, GlyphWifi4, GlyphWifiOff,
	GlyphBluetooth, GlyphBluetoothOff, GlyphDevice, GlyphVpn, GlyphLock, GlyphRefresh,
	GlyphForget, GlyphHeadphones, GlyphKeyboard, GlyphMouse, GlyphComputer, GlyphGamepad,
	GlyphBell, GlyphBellOff, GlyphClose, GlyphCount,
};

typedef struct {
	ShellHost host;
	ShellFont font;
	ShellSurface icon_surface, panel_surface;
	ShellPopupExit exit;
	GMainLoop *loop;
	Window icon, panel, manager, instance_window;
	Atom tray_selection, tray_opcode, embed_info, manager_atom, colors, config_serial;
	Atom instance, nod_client, clipboard, utf8, paste;
	XIM im;
	XIC ic;
	const char *config_path, *title;
	gboolean explicit_config;
	guint frame_id, redraw_id;
	unsigned revision;
	int screen, icon_w, icon_h, pad, width, height, x, y, offset_y, direction;
	int hover_x, hover_y, scroll, visible_rows, selected, tab;
	char *expanded;
	int detail_scroll, detail_viewport;
	cui_rect detail_bounds;
	int field, cursor, field_scroll, fields_visible;
	gboolean select_all, paste_pending, icon_hover;
	double dock_sent, opened;
	cui_color accent;
	TrayHit hits[64];
	int count;
	cui_icon_state glyphs[GlyphCount];
	unsigned form_generation;
} TrayWindow;

int tray_ui_height(Tray *, int available);
void tray_ui_panel(Tray *);
void tray_ui_icon(Tray *);
void tray_ui_glyphs_free(TrayWindow *);
int tray_row_count(Tray *);
TrayEntry *tray_row(Tray *, int index);
TrayEntry *tray_expanded(Tray *);

#endif
