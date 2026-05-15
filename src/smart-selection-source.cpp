/*
Smart Selection — source implementation.

Wrapper source: owns a private monitor_capture + crop_filter. Properties:
  monitor (int), x, y, cx, cy, "Drag to select region" button.
*/

#include <obs-module.h>
#include <util/dstr.h>
#include <graphics/graphics.h>

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
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#define SETTING_MONITOR_IDX "monitor"
#define SETTING_X           "x"
#define SETTING_Y           "y"
#define SETTING_CX          "cx"
#define SETTING_CY          "cy"

#define DEFAULT_CX 800
#define DEFAULT_CY 600

namespace {

struct SmartSelectionSource {
	obs_source_t *self          = nullptr;
	obs_source_t *internal_mc   = nullptr;
	obs_source_t *internal_crop = nullptr;

	int monitor_idx = 0;
	int x  = 0;
	int y  = 0;
	int cx = DEFAULT_CX;
	int cy = DEFAULT_CY;
};

const char *kSourceId = "smart_selection_source";

#ifdef _WIN32
struct MonitorEnumCtx {
	int  target_idx;
	int  current_idx;
	std::string device_name; // e.g. "\\\\.\\DISPLAY1"
};

BOOL CALLBACK monitor_enum_cb(HMONITOR mon, HDC, LPRECT, LPARAM lparam)
{
	auto *c = reinterpret_cast<MonitorEnumCtx *>(lparam);
	if (c->current_idx == c->target_idx) {
		MONITORINFOEXA mi;
		mi.cbSize = sizeof(mi);
		if (GetMonitorInfoA(mon, &mi))
			c->device_name = mi.szDevice;
		return FALSE; // stop
	}
	c->current_idx++;
	return TRUE;
}

std::string get_monitor_device_name(int idx)
{
	MonitorEnumCtx c{idx, 0, ""};
	EnumDisplayMonitors(nullptr, nullptr, monitor_enum_cb,
			    reinterpret_cast<LPARAM>(&c));
	return c.device_name;
}

struct MonitorIndexCtx {
	std::string target_device;
	int  found_index = -1;
	int  current_index = 0;
};

BOOL CALLBACK monitor_index_cb(HMONITOR mon, HDC, LPRECT, LPARAM lparam)
{
	auto *c = reinterpret_cast<MonitorIndexCtx *>(lparam);
	MONITORINFOEXA mi;
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoA(mon, &mi)) {
		if (c->target_device == mi.szDevice) {
			c->found_index = c->current_index;
			return FALSE;
		}
	}
	c->current_index++;
	return TRUE;
}

int find_monitor_index_by_device(const std::string &device_name)
{
	MonitorIndexCtx c;
	c.target_device = device_name;
	EnumDisplayMonitors(nullptr, nullptr, monitor_index_cb,
			    reinterpret_cast<LPARAM>(&c));
	return c.found_index;
}

// HMONITOR 기준으로 EnumDisplayMonitors 순서의 인덱스를 찾는다
struct HmonIndexCtx {
	HMONITOR target = nullptr;
	int found_index = -1;
	int current_index = 0;
};

BOOL CALLBACK hmon_index_cb(HMONITOR mon, HDC, LPRECT, LPARAM lparam)
{
	auto *c = reinterpret_cast<HmonIndexCtx *>(lparam);
	if (mon == c->target) {
		c->found_index = c->current_index;
		return FALSE;
	}
	c->current_index++;
	return TRUE;
}

// Qt 의 QScreen 한 개를 좌표 기반으로 Win32 모니터 인덱스에 매칭
int find_monitor_index_for_qscreen(QScreen *screen)
{
	if (!screen) return -1;
	const QRect g = screen->geometry();
	const qreal dpr = screen->devicePixelRatio();
	POINT pt;
	pt.x = (LONG)((g.x() + g.width()  / 2) * dpr);
	pt.y = (LONG)((g.y() + g.height() / 2) * dpr);
	HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	HmonIndexCtx c;
	c.target = hmon;
	EnumDisplayMonitors(nullptr, nullptr, hmon_index_cb,
			    reinterpret_cast<LPARAM>(&c));
	return c.found_index;
}
#else
std::string get_monitor_device_name(int) { return ""; }
#endif

void apply_monitor_to_child(SmartSelectionSource *ctx)
{
	if (!ctx->internal_mc) return;
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "monitor", ctx->monitor_idx);
	obs_data_set_int(s, "method",  0); // 0=Auto, 1=DXGI, 2=WGC
	std::string dev = get_monitor_device_name(ctx->monitor_idx);
	if (!dev.empty()) {
		obs_data_set_string(s, "monitor_id", dev.c_str());
		obs_log(LOG_INFO, "applied monitor_id='%s' (idx=%d)",
			dev.c_str(), ctx->monitor_idx);
	} else {
		obs_log(LOG_WARNING, "monitor_id empty for idx=%d (Win32 enum failed)",
			ctx->monitor_idx);
	}
	obs_source_update(ctx->internal_mc, s);
	obs_data_release(s);
}

void apply_crop_to_child(SmartSelectionSource *)
{
	// 매트릭스 변환 방식으로 바뀌어 더이상 필터를 갱신할 필요가 없음.
	// 크롭 좌표(ctx->x, y, cx, cy)는 ss_video_render에서 매번 사용된다.
}

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

	{
		obs_data_t *s = obs_data_create();
		obs_data_set_int(s, "monitor", ctx->monitor_idx);
		obs_data_set_int(s, "method",  0);
		std::string dev = get_monitor_device_name(ctx->monitor_idx);
		if (!dev.empty())
			obs_data_set_string(s, "monitor_id", dev.c_str());
		ctx->internal_mc = obs_source_create_private(
			"monitor_capture", "smartsel_internal_capture", s);
		obs_data_release(s);
	}

	// crop_filter는 사용하지 않는다 (매트릭스 변환으로 직접 크롭).
	ctx->internal_crop = nullptr;

	const uint32_t mc_w = ctx->internal_mc ? obs_source_get_width(ctx->internal_mc) : 0;
	const uint32_t mc_h = ctx->internal_mc ? obs_source_get_height(ctx->internal_mc) : 0;
	obs_log(LOG_INFO,
		"created src=%p internal_mc=%p (w=%u h=%u) internal_crop=%p "
		"monitor=%d x=%d y=%d cx=%d cy=%d",
		ctx->self, ctx->internal_mc, mc_w, mc_h, ctx->internal_crop,
		ctx->monitor_idx, ctx->x, ctx->y, ctx->cx, ctx->cy);
	return ctx;
}

void ss_destroy(void *data)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (!ctx) return;
	if (ctx->internal_crop) obs_source_release(ctx->internal_crop);
	if (ctx->internal_mc)   obs_source_release(ctx->internal_mc);
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
	if (!ctx || !ctx->internal_mc) return;

	// Manual crop: translate the child render so that (ctx->x, ctx->y) of the
	// monitor appears at (0,0) of our render target. Our wrapper reports
	// (cx, cy) as its dimensions, so only that visible window is shown —
	// pixels outside are clipped by the render target boundary.
	gs_matrix_push();
	gs_matrix_translate3f((float)(-ctx->x), (float)(-ctx->y), 0.0f);
	obs_source_video_render(ctx->internal_mc);
	gs_matrix_pop();
}

void ss_show(void *data)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (ctx && ctx->internal_mc) {
		obs_source_inc_showing(ctx->internal_mc);
		obs_log(LOG_INFO, "show -> inc_showing on child (mc_w=%u)",
			obs_source_get_width(ctx->internal_mc));
	}
}
void ss_hide(void *data)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (ctx && ctx->internal_mc) obs_source_dec_showing(ctx->internal_mc);
}
void ss_activate(void *data)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (ctx && ctx->internal_mc) obs_source_inc_active(ctx->internal_mc);
}
void ss_deactivate(void *data)
{
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (ctx && ctx->internal_mc) obs_source_dec_active(ctx->internal_mc);
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

	if (monitor_changed) apply_monitor_to_child(ctx);
	apply_crop_to_child(ctx);

	obs_log(LOG_INFO, "update m=%d x=%d y=%d cx=%d cy=%d (mc=%ux%u)",
		ctx->monitor_idx, ctx->x, ctx->y, ctx->cx, ctx->cy,
		ctx->internal_mc ? obs_source_get_width(ctx->internal_mc) : 0,
		ctx->internal_mc ? obs_source_get_height(ctx->internal_mc) : 0);
}

bool on_select_region_clicked(obs_properties_t *props, obs_property_t *prop,
			      void *data)
{
	UNUSED_PARAMETER(prop);
	auto *ctx = static_cast<SmartSelectionSource *>(data);
	if (!ctx) return false;

	if (!QApplication::instance()) {
		obs_log(LOG_WARNING, "no QApplication available; cannot show overlay");
		return false;
	}

	SelectionOverlay overlay;
	if (!overlay.runModal()) return false;

	const QRect virt = overlay.resultRect();
	if (virt.isEmpty()) return false;

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

	// HighDPI: Qt geometry/local coords are logical pixels; monitor_capture
	// works in physical pixels. Scale by the target monitor's DPR so the
	// crop region maps to the actual captured frame.
	if (found_idx >= 0 && found_idx < screens.size()) {
		const qreal dpr = screens[found_idx]->devicePixelRatio();
		if (dpr > 0.0) {
			local_x  = (int)(local_x  * dpr);
			local_y  = (int)(local_y  * dpr);
			local_cx = (int)(local_cx * dpr);
			local_cy = (int)(local_cy * dpr);
			obs_log(LOG_INFO, "DPR scaling: ratio=%.2f -> x=%d y=%d cx=%d cy=%d",
				(double)dpr, local_x, local_y, local_cx, local_cy);
		}
	}

	// Qt와 Win32의 모니터 enumeration 순서가 다를 수 있다.
	// QScreen::name()은 Windows에서 GDI device name(\\.\DISPLAY1)을 돌려주므로
	// 그것으로 Win32 인덱스를 다시 찾아 OBS에 정확한 monitor 값을 전달한다.
	int win32_monitor_idx = found_idx;
	// ALWAYS log so we can verify which build is running and what mapping happens.
	blog(LOG_INFO, "[smart_selection] === select_region START ===");
	blog(LOG_INFO, "[smart_selection] virtual rect = (%d,%d)-(%dx%d)",
	     virt.x(), virt.y(), virt.width(), virt.height());
	blog(LOG_INFO, "[smart_selection] Qt screens count = %d, found_idx = %d",
	     (int)screens.size(), found_idx);
	for (int i = 0; i < screens.size(); ++i) {
		const QRect g = screens[i]->geometry();
		blog(LOG_INFO, "[smart_selection]   Qt[%d] name='%s' geom=(%d,%d)-(%dx%d) dpr=%.2f",
		     i,
		     screens[i]->name().toUtf8().constData(),
		     g.x(), g.y(), g.width(), g.height(),
		     screens[i]->devicePixelRatio());
	}
#ifdef _WIN32
	if (found_idx >= 0 && found_idx < screens.size()) {
		QScreen *qs = screens[found_idx];
		int resolved = find_monitor_index_for_qscreen(qs);
		blog(LOG_INFO,
		     "[smart_selection] mapping Qt idx=%d ('%s', dpr=%.2f) "
		     "via MonitorFromPoint -> Win32 idx=%d",
		     found_idx, qs->name().toUtf8().constData(),
		     qs->devicePixelRatio(), resolved);
		if (resolved >= 0) {
			win32_monitor_idx = resolved;
		}
	} else {
		blog(LOG_WARNING, "[smart_selection] found_idx out of range or no screens");
	}
#endif
	blog(LOG_INFO, "[smart_selection] FINAL: monitor=%d local=(%d,%d) size=(%dx%d)",
	     win32_monitor_idx, local_x, local_y, local_cx, local_cy);

	obs_data_t *s = obs_source_get_settings(ctx->self);
	obs_data_set_int(s, SETTING_MONITOR_IDX, win32_monitor_idx);
	obs_data_set_int(s, SETTING_X,  local_x);
	obs_data_set_int(s, SETTING_Y,  local_y);
	obs_data_set_int(s, SETTING_CX, local_cx);
	obs_data_set_int(s, SETTING_CY, local_cy);
	obs_source_update(ctx->self, s);
	obs_data_release(s);

	obs_log(LOG_INFO, "region on monitor #%d -> x=%d y=%d cx=%d cy=%d",
		found_idx, local_x, local_y, local_cx, local_cy);
	UNUSED_PARAMETER(props);
	return true;
}

obs_properties_t *ss_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_button(p, "select_region",
		obs_module_text("SmartSelection.SelectRegion"),
		on_select_region_clicked);
	obs_properties_add_int(p, SETTING_MONITOR_IDX,
		obs_module_text("SmartSelection.Monitor"), 0, 16, 1);
	obs_properties_add_int(p, SETTING_X,
		obs_module_text("SmartSelection.X"), 0, 16384, 1);
	obs_properties_add_int(p, SETTING_Y,
		obs_module_text("SmartSelection.Y"), 0, 16384, 1);
	obs_properties_add_int(p, SETTING_CX,
		obs_module_text("SmartSelection.Width"), 1, 16384, 1);
	obs_properties_add_int(p, SETTING_CY,
		obs_module_text("SmartSelection.Height"), 1, 16384, 1);
	return p;
}

obs_source_info make_source_info()
{
	obs_source_info info = {};
	info.id           = kSourceId;
	info.type         = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.icon_type    = OBS_ICON_TYPE_DESKTOP_CAPTURE;

	info.get_name            = ss_get_name;
	info.create              = ss_create;
	info.destroy             = ss_destroy;
	info.get_width           = ss_get_width;
	info.get_height          = ss_get_height;
	info.get_defaults        = ss_get_defaults;
	info.get_properties      = ss_properties;
	info.update              = ss_update;
	info.video_render        = ss_video_render;
	info.enum_active_sources = ss_enum_active_sources;
	info.show                = ss_show;
	info.hide                = ss_hide;
	info.activate            = ss_activate;
	info.deactivate          = ss_deactivate;
	return info;
}

} // namespace

extern "C" struct obs_source_info smart_selection_source_info = make_source_info();
