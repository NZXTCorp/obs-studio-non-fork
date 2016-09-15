#include <windows.h>

#include <obs-module.h>
#include <util/dstr.h>

#include "cursor-capture.h"
#include "window-helpers.h"

#define do_log(level, format, ...) \
	blog(level, "[duplicator-window-capture: '%s'] " format, \
			obs_source_get_name(capture->source), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define TEXT_WINDOW_CAPTURE  obs_module_text("DisplayWindowCapture")
#define TEXT_CAPTURE_CURSOR  obs_module_text("CaptureCursor")
#define TEXT_COMPATIBILITY   obs_module_text("Compatibility")
#define TEXT_WINDOW          obs_module_text("WindowCapture.Window")
#define TEXT_MATCH_PRIORITY  obs_module_text("WindowCapture.Priority")
#define TEXT_MATCH_TITLE     obs_module_text("WindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS     obs_module_text("WindowCapture.Priority.Class")
#define TEXT_MATCH_EXE       obs_module_text("WindowCapture.Priority.Exe")

#define RESET_INTERVAL_SEC 3.0f

struct duplicator_window_capture {
	obs_source_t                   *source;

	int                            monitor;

	char                           *title;
	char                           *class;
	char                           *executable;
	enum window_priority           priority;

	bool                           capture_cursor;

	long                           x;
	long                           y;
	int                            rot;
	uint32_t                       width;
	uint32_t                       height;
	gs_duplicator_t                *duplicator;
	float                          reset_timeout;
	struct cursor_data             cursor_data;

	HWND                           window;
	RECT                           last_rect;
	bool                           overlapped;
};

/* ------------------------------------------------------------------------- */

static inline void update_settings(struct duplicator_window_capture *capture,
		obs_data_t *settings)
{
	const char *window      = obs_data_get_string(settings, "window");
	capture->monitor        = (int)obs_data_get_int(settings, "monitor");
	capture->capture_cursor = obs_data_get_bool(settings, "cursor");

	bfree(capture->title);
	bfree(capture->class);
	bfree(capture->executable);

	build_window_strings(window, &capture->class, &capture->title, &capture->executable);

	obs_enter_graphics();

	gs_duplicator_destroy(capture->duplicator);
	capture->duplicator = NULL;
	capture->monitor = -1;
	capture->width = 0;
	capture->height = 0;
	capture->x = 0;
	capture->y = 0;
	capture->rot = 0;
	capture->reset_timeout = 0.0f;

	obs_leave_graphics();
}

/* ------------------------------------------------------------------------- */

static const char *duplicator_capture_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_WINDOW_CAPTURE;
}

static void duplicator_capture_destroy(void *data)
{
	struct duplicator_window_capture *capture = data;

	obs_enter_graphics();

	gs_duplicator_destroy(capture->duplicator);
	cursor_data_free(&capture->cursor_data);

	obs_leave_graphics();

	bfree(capture->title);
	bfree(capture->class);
	bfree(capture->executable);

	bfree(capture);
}

static void duplicator_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "cursor", true);
	obs_data_set_default_bool(settings, "compatibility", false);
}

static void duplicator_capture_update(void *data, obs_data_t *settings)
{
	struct duplicator_window_capture *mc = data;
	update_settings(mc, settings);

	/* forces a reset */
	mc->window = NULL;
}

static void *duplicator_capture_create(obs_data_t *settings,
		obs_source_t *source)
{
	struct duplicator_window_capture *capture;

	capture = bzalloc(sizeof(struct duplicator_window_capture));
	capture->source = source;

	update_settings(capture, settings);

	return capture;
}

static bool find_monitor(struct duplicator_window_capture *capture, RECT *rect)
{
	struct gs_monitor_info info;
	int candidate = 0;

	for (;; candidate++) {
		if (!gs_get_duplicator_monitor_info(candidate, &info))
			return false;

		RECT screen_rect = {
			info.x, info.y, info.x + info.cx, info.y + info.cy
		};
		if (!PtInRect(&screen_rect, *(POINT*)rect))
			continue;

		capture->monitor = candidate;
		return true;
	}

	return false;
}

static void reset_capture_data(struct duplicator_window_capture *capture)
{
	struct gs_monitor_info monitor_info = {0};
	gs_texture_t *texture = gs_duplicator_get_texture(capture->duplicator);

	gs_get_duplicator_monitor_info(capture->monitor, &monitor_info);
	capture->width = gs_texture_get_width(texture);
	capture->height = gs_texture_get_height(texture);
	capture->x = monitor_info.x;
	capture->y = monitor_info.y;
	capture->rot = monitor_info.rotation_degrees;
}

static bool is_overlapped(struct duplicator_window_capture *capture)
{
	RECT rect, intersect;
	DWORD styles, ex_styles;

	for (HWND wnd = GetWindow(capture->window, GW_HWNDPREV);
			wnd != NULL && wnd != capture->window;
			wnd = GetWindow(wnd, GW_HWNDPREV)) {
		if (!IsWindowVisible(wnd) || IsIconic(wnd))
			continue;

		GetClientRect(wnd, &rect);
		styles = (DWORD)GetWindowLongPtr(wnd, GWL_STYLE);
		ex_styles = (DWORD)GetWindowLongPtr(wnd, GWL_EXSTYLE);

		if (ex_styles & WS_EX_TOOLWINDOW)
			continue;
		if (styles & WS_CHILD)
			continue;
		if (rect.bottom == 0 || rect.right == 0)
			continue;

		MapWindowPoints(wnd, HWND_DESKTOP, (POINT*)&rect, 2);
		if (IntersectRect(&intersect, &rect, &capture->last_rect))
			return true;
	}

	return false;
}

static void duplicator_capture_tick(void *data, float seconds)
{
	struct duplicator_window_capture *capture = data;
	RECT rect;
	bool reset_capture = false;

	if (!obs_source_showing(capture->source))
		return;

	if (!capture->window || !IsWindow(capture->window)) {
		if (!capture->title && !capture->class)
			return;

		capture->window = find_window(EXCLUDE_MINIMIZED, capture->priority,
				capture->class, capture->title, capture->executable);
		if (!capture->window)
			return;

		reset_capture = true;

	} else if (IsIconic(capture->window)) {
		return;
	}

	GetClientRect(capture->window, &rect);
	MapWindowPoints(capture->window, HWND_DESKTOP, (POINT*)&rect, 2);

	bool position_changed = rect.top != capture->last_rect.top || rect.left != capture->last_rect.left;

	obs_enter_graphics();

	if (!capture->duplicator || position_changed) {
		capture->reset_timeout += seconds;

		if (position_changed || capture->reset_timeout >= RESET_INTERVAL_SEC) {
			int old_monitor = capture->monitor;
			if (!find_monitor(capture, &rect))
				goto leave;

			if (old_monitor != capture->monitor) {
				gs_duplicator_destroy(capture->duplicator);
				capture->duplicator =
					gs_duplicator_create(capture->monitor);
				reset_capture_data(capture);
			}

			capture->reset_timeout = 0.0f;
		}
	}

	capture->last_rect = rect;

	if (!!capture->duplicator) {
		if (capture->capture_cursor)
			cursor_capture(&capture->cursor_data);

		if (!gs_duplicator_update_frame(capture->duplicator)) {
			gs_duplicator_destroy(capture->duplicator);
			capture->duplicator = NULL;
			capture->width = 0;
			capture->height = 0;
			capture->x = 0;
			capture->y = 0;
			capture->rot = 0;
			capture->reset_timeout = 0.0f;

		} else if (capture->width == 0) {
			reset_capture_data(capture);
		}
	}

leave:
	obs_leave_graphics();

	GetClientRect(capture->window, &rect);
	MapWindowPoints(capture->window, HWND_DESKTOP, (POINT*)&rect, 2);

	capture->overlapped = is_overlapped(capture);

	UNUSED_PARAMETER(seconds);
}

static uint32_t duplicator_capture_width(void *data)
{
	struct duplicator_window_capture *capture = data;
	RECT res, screen = {
		capture->x, capture->y, capture->x + capture->width, capture->y + capture->height
	};
	if (!IntersectRect(&res, &capture->last_rect, &screen))
		return 0;
	return (capture->rot % 180 == 0) ? (res.right - res.left) : (res.bottom - res.top);
}

static uint32_t duplicator_capture_height(void *data)
{
	struct duplicator_window_capture *capture = data;
	RECT res, screen = {
		capture->x, capture->y, capture->x + capture->width, capture->y + capture->height
	};
	if (!IntersectRect(&res, &capture->last_rect, &screen))
		return 0;
	return (capture->rot % 180 == 0) ? (res.bottom - res.top) : (res.right - res.left);
}

static void draw_cursor(struct duplicator_window_capture *capture, RECT *rect)
{
	long width = rect->right - rect->left;
	long height = rect->bottom - rect->top;
	cursor_draw(&capture->cursor_data, -rect->left, -rect->top,
		1.0f, 1.0f,
		capture->rot % 180 == 0 ? width : height,
		capture->rot % 180 == 0 ? height : width);
}

static void duplicator_capture_render(void *data, gs_effect_t *effect)
{
	struct duplicator_window_capture *capture = data;
	gs_texture_t *texture;
	int rot;

	if (!capture->duplicator || capture->overlapped)
		return;

	texture = gs_duplicator_get_texture(capture->duplicator);
	if (!texture)
		return;

	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	rot = capture->rot;

	RECT res, screen = {
		capture->x, capture->y, capture->x + capture->width, capture->y + capture->height
	};
	if (!IntersectRect(&res, &capture->last_rect, &screen))
		return;

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	while (gs_effect_loop(effect, "Draw")) {
		if (rot != 0) {
			float x = 0.0f;
			float y = 0.0f;

			switch (rot) {
			case 90:
				x = (float)res.right - res.left;
				break;
			case 180:
				x = (float)res.bottom - res.top;
				y = (float)res.right - res.left;
				break;
			case 270:
				y = (float)res.bottom - res.top;
				break;
			}

			gs_matrix_push();
			gs_matrix_translate3f(x, y, 0.0f);
			gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD((float)rot));
		}

		gs_draw_sprite_cropped(texture, 0, 0, 0,
			(float)res.left - capture->x, (float)res.top - capture->y,
			(float)capture->width - (res.right - capture->x),
			(float)capture->height - (res.bottom - capture->y));

		if (rot != 0)
			gs_matrix_pop();
	}

	if (capture->capture_cursor) {
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		while (gs_effect_loop(effect, "Draw")) {
			draw_cursor(capture, &res);
		}
	}
}

static obs_properties_t *duplicator_capture_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(ppts, "window", TEXT_WINDOW,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_window_list(p, EXCLUDE_MINIMIZED);

	p = obs_properties_add_list(ppts, "priority", TEXT_MATCH_PRIORITY,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_MATCH_TITLE, WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(p, TEXT_MATCH_CLASS, WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(p, TEXT_MATCH_EXE, WINDOW_PRIORITY_EXE);

	obs_properties_add_bool(ppts, "cursor", TEXT_CAPTURE_CURSOR);

	obs_properties_add_bool(ppts, "compatibility", TEXT_COMPATIBILITY);

	return ppts;
}

struct obs_source_info duplicator_window_info = {
	.id             = "display_window_capture",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name       = duplicator_capture_getname,
	.create         = duplicator_capture_create,
	.destroy        = duplicator_capture_destroy,
	.video_render   = duplicator_capture_render,
	.video_tick     = duplicator_capture_tick,
	.update         = duplicator_capture_update,
	.get_width      = duplicator_capture_width,
	.get_height     = duplicator_capture_height,
	.get_defaults   = duplicator_capture_defaults,
	.get_properties = duplicator_capture_properties
};
