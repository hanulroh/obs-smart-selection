/*
Smart Selection — source implementation.

This source is a "wrapper": it owns a private monitor_capture child source and
a crop_filter attached to that child. The user-facing properties are:

  • Monitor index (which monitor to capture from)
  • Region X, Y, Width, Height (crop applied to that monitor's frame)
  • "🎯 Drag to select region" button (opens fullscreen Qt overlay)

The Qt overlay must be invoked on OBS's UI (main) thread because it touches
Qt widgets. OBS calls obs_properties_t button callbacks on the UI thread
already, so we can call SelectionOverlay::runModal() directly from there.
*/

#include <obs-module.h>
#include <util/dstr.h>

extern "C" {
#include <plugin-support.h>
}

#include "selection-overlay.hpp"

#include <QApplication>
#include <QScreen>
#include <QRect>
#include <QPoint>

#include <algorithm>
#include <cstring>

#define SETTING_MONITOR_IDX  "monitor"
#define SETTING_X            "x"
#define SETTING_Y            "y"
#define SETTING_CX           "cx"
#define SETTING_CY           "cy"

#define DEFAULT_CX 800
#define DEFAULT_CY 600

namespace {

struct SmartSelectionSource {
	obs_source_t *self            = nullptr;
	obs_source_t *internal_mc     = nullptr; // monitor_capture child
	obs_source_t *internal_crop   = nullptr; // crop_filter on the child

	int monitor_idx = 0;
	int x  = 0;
	int y  = 0;
	int cx = DEFAULT_CX;
	int cy = DEFAULT_CY;
};

const char *kSourceId = "smart_selection_source";

// ----- Helpers -----

void apply_monitor_to_child(SmartSelectionSource *ctx)
{
	if (!ctx->internal_mc)
		return;
	obs_data_t *s = obs_data_create();
	// Set both deprecated int and modern string in case downstream picks
	// one or the other; harmless if a key is ignored.
	obs_data_set_int(s, "monitor", ctx->monitor_idx);
	obs_data_set_int(s, "method", 2); // DXGI desktop duplication where supported
	obs_source_update(ctx->internal_mc, s);
	obs_data_release(s);
}

void apply_crop_to_child(SmartSelectionSource *ctx)
{
	if (!ctx->internal_crop)
		return;
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "relative", false);
	obs_data_set_int(s, "x",  ctx->x);
	obs_data_set_int(s, "y",  ctx->y);
	obs_data_set_int(s, "cx", std::max(1, ctx->cx));
	obs_data_set_int(s, "cy", std::max(1, ctx->cy));
	obs_source_update(ctx->internal_crop, s);
	obs_data_release(s);
}

// ----- OBS source vtable -----

const char *ss_get_name(void *)
{
	return obs_module_text("SmartSelection.Name");
}

void *ss_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new SmartSelectionSource();
	ctx->self        = source;
	ctx->monitor_idx = (int)obs_data_get_int(settings, SETTING_MONITOR_IDX);
	ctx->x           = (int)obs_data_get_int(settings, SETTING_X);
	ctx->y           = (int)obs_data_get_int(settings, SETTING_Y);
	ctx->cx          = (int)obs_data_get_int(settings, SETTING_CX);
	ctx->cy          = (int)obs_data_get_int(settings, SETTING_CY);

	// Build child monitor_capture.
	{
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "monitor", ctx->monitor_idx);
		obs_data_set_int(s, "method", 2);
		ctx->internal_mc = obs_source_create_private(
			"monitor_capture", "smartsel_internal_capture", s);
		obs_data_release(s);
	}

	// Build crop_filter and attach it.
	{
		obs_data_t *s = obs_data_create();
		obs_data_set_bool(s, "relative", false);
		obs_data_set_int(s, "x",  ctx->x);
		obs_data_set_int(s, "y",  ctx->y);
		obs_data_set_int(s, "cx", std::max(1, ctx->cx));
		obs_data_set_int(s, "cy", std::max(1, ctx->cy));
		ctx->internal_crop = obs_source_create_private(
			"crop_filter", "smartsel_internal_crop", s);
		obs_data_release(s);
	}

	if (ctx->internal_mc && ctx->internal_crop)
		obs_source_filter_add(ctx->internal_mc, ctx->internal_crop);

	obs_log(LOG_INFO,
		"created (monitor=%d x=%d y=%d cx=%d cy=%d)",
		ctx->monitor_idx, ctx->x, ctx->y, ctx->cx, ctx->cy);

	return ctx;
}

void ss_destroy(void *data)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (!ctx)
		return;
	if (ctx->internal_crop)
		obs_source_release(ctx->internal_crop);
	if (ctx->internal_mc)
		obs_source_release(ctx->internal_mc);
	delete ctx;
}

uint32_t ss_get_width(void *data)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	return (uint32_t)std::max(1, ctx->cx);
}

uint32_t ss_get_height(void *data)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	return (uint32_t)std::max(1, ctx->cy);
}

void ss_video_render(void *data, gs_effect_t *)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (!ctx || !ctx->internal_mc)
		return;
	obs_source_video_render(ctx->internal_mc);
}

void ss_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (!ctx || !ctx->internal_mc)
		return;
	// Forward tick so the child runs at the right cadence.
	obs_source_video_tick(ctx->internal_mc, seconds);
}

void ss_enum_active_sources(void *data, obs_source_enum_proc_t enum_callback,
			    void *param)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (ctx && ctx->internal_mc)
		enum_callback(ctx->self, ctx->internal_mc, param);
}

void ss_get_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, SETTING_MONITOR_IDX, 0);
	obs_data_set_default_int(s, SETTING_X,  0);
	obs_data_set_default_int(s, SETTING_Y,  0);
	obs_data_set_default_int(s, SETTING_CX, DEFAULT_CX);
	obs_data_set_default_int(s, SETTING_CY, DEFAULT_CY);
}

void ss_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	const int new_monitor = (int)obs_data_get_int(settings, SETTING_MONITOR_IDX);
	const int new_x  = (int)obs_data_get_int(settings, SETTING_X);
	const int new_y  = (int)obs_data_get_int(settings, SETTING_Y);
	const int new_cx = (int)obs_data_get_int(settings, SETTING_CX);
	const int new_cy = (int)obs_data_get_int(settings, SETTING_CY);

	const bool monitor_changed = new_monitor != ctx->monitor_idx;
	ctx->monitor_idx = new_monitor;
	ctx->x  = new_x;
	ctx->y  = new_y;
	ctx->cx = std::max(1, new_cx);
	ctx->cy = std::max(1, new_cy);

	if (monitor_changed)
		apply_monitor_to_child(ctx);
	apply_crop_to_child(ctx);
}

// ----- Properties -----

bool on_select_region_clicked(obs_properties_t *props, obs_property_t *prop,
			      void *data)
{
	UNUSED_PARAMETER(prop);
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (!ctx)
		return false;

	// Must run on UI thread; OBS button callbacks are already on UI thread.
	if (!QApplication::instance()) {
		obs_log(LOG_WARNING,
			"no QApplication available; cannot show overlay");
		return false;
	}

	SelectionOverlay overlay;
	if (!overlay.runModal())
		return false; // user cancelled

	const QRect virt = overlay.resultRect();
	if (virt.isEmpty())
		return false;

	// Find which monitor contains the rect's center, in virtual coords.
	const QPoint center = virt.center();
	const auto screens = QApplication::screens();
	int found_idx = -1;
	QRect target_geom;
	for (int i = 0; i < screens.size(); ++i) {
		const QRect g = screens[i]->geometry();
		if (g.contains(center)) {
			found_idx = i;
			target_geom = g;
			break;
		}
	}
	if (found_idx < 0 && !screens.isEmpty()) {
		found_idx = 0;
		target_geom = screens[0]->geometry();
	}

	// Translate virtual-desktop coords → monitor-local coords, clamp.
	int local_x  = virt.x() - target_geom.x();
	int local_y  = virt.y() - target_geom.y();
	int local_cx = virt.width();
	int local_cy = virt.height();
	if (local_x < 0) { local_cx += local_x; local_x = 0; }
	if (local_y < 0) { local_cy += local_y; local_y = 0; }
	if (local_x + local_cx > target_geom.width())
		local_cx = target_geom.width() - local_x;
	if (local_y + local_cy > target_geom.height())
		local_cy = target_geom.height() - local_y;
	local_cx = std::max(1, local_cx);
	local_cy = std::max(1, local_cy);

	// Persist into source settings; OBS will call ss_update for us.
	obs_data_t *s = obs_source_get_settings(ctx->self);
	obs_data_set_int(s, SETTING_MONITOR_IDX, found_idx);
	obs_data_set_int(s, SETTING_X,  local_x);
	obs_data_set_int(s, SETTING_Y,  local_y);
	obs_data_set_int(s, SETTING_CX, local_cx);
	obs_data_set_int(s, SETTING_CY, local_cy);
	obs_source_update(ctx->self, s);
	obs_data_release(s);

	obs_log(LOG_INFO,
		"region selected on monitor #%d -> x=%d y=%d cx=%d cy=%d "
		"(virtual rect %d,%d %dx%d)",
		found_idx, local_x, local_y, local_cx, local_cy,
		virt.x(), virt.y(), virt.width(), virt.height());

	UNUSED_PARAMETER(props);
	return true; // refresh property UI to show new x/y/cx/cy
}

obs_properties_t *ss_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();

	obs_properties_add_button(p, "select_region",
				  obs_module_text("SmartSelection.SelectRegion"),
				  on_select_region_clicked);

	obs_properties_add_int(p, SETTING_MONITOR_IDX,
			       obs_module_text("SmartSelection.Monitor"),
			       0, 16, 1);
	obs_properties_add_int(p, SETTING_X,
			       obs_module_text("SmartSelection.X"),
			       0, 16384, 1);
	obs_properties_add_int(p, SETTING_Y,
			       obs_module_text("SmartSelection.Y"),
			       0, 16384, 1);
	obs_properties_add_int(p, SETTING_CX,
			       obs_module_text("SmartSelection.Width"),
			       1, 16384, 1);
	obs_properties_add_int(p, SETTING_CY,
			       obs_module_text("SmartSelection.Height"),
			       1, 16384, 1);
	return p;
}

// ----- Build the obs_source_info struct -----

obs_source_info make_source_info()
{
	obs_source_info info = {};
	info.id           = kSourceId;
	info.type         = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.icon_type    = OBS_ICON_TYPE_DESKTOP_CAPTURE;

	info.get_name           = ss_get_name;
	info.create             = ss_create;
	info.destroy            = ss_destroy;
	info.get_width          = ss_get_width;
	info.get_height         = ss_get_height;
	info.get_defaults       = ss_get_defaults;
	info.get_properties     = ss_properties;
	info.update             = ss_update;
	info.video_render       = ss_video_render;
	info.video_tick         = ss_video_tick;
	info.enum_active_sources = ss_enum_active_sources;
	return info;
}

} // namespace

extern "C" struct obs_source_info smart_selection_source_info = make_source_info();
