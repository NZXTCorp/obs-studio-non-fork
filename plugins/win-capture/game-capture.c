#include <inttypes.h>
#include <obs-module.h>
#include <util/platform.h>
#include <windows.h>
#include <dxgi.h>
#include <emmintrin.h>
#include <ipc-util/pipe.h>
#include "obfuscate.h"
#include "inject-library.h"
#include "graphics-hook-info.h"
#include "window-helpers.h"
#include "cursor-capture.h"

#define do_log(level, format, ...) \
	blog(level, "[game-capture: '%s'] " format, \
			obs_source_get_name(gc->source), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define SETTING_ANY_FULLSCREEN   "capture_any_fullscreen"
#define SETTING_CAPTURE_WINDOW   "window"
#define SETTING_ACTIVE_WINDOW    "active_window"
#define SETTING_WINDOW_PRIORITY  "priority"
#define SETTING_COMPATIBILITY    "sli_compatibility"
#define SETTING_FORCE_SCALING    "force_scaling"
#define SETTING_SCALE_RES        "scale_res"
#define SETTING_CURSOR           "capture_cursor"
#define SETTING_TRANSPARENCY     "allow_transparency"
#define SETTING_LIMIT_FRAMERATE  "limit_framerate"
#define SETTING_CAPTURE_OVERLAYS "capture_overlays"
#define SETTING_ANTI_CHEAT_HOOK  "anti_cheat_hook"
#define SETTING_ALLOW_IPC_INJ    "allow_ipc_injector"
#define SETTING_OVERLAY_DLL      "overlay_dll"
#define SETTING_OVERLAY_DLL64    "overlay_dll64"
#define SETTING_PROCESS_ID       "process_id"
#define SETTING_THREAD_ID        "thread_id"
#define SETTING_HWND             "hwnd"

#define TEXT_GAME_CAPTURE        obs_module_text("GameCapture")
#define TEXT_ANY_FULLSCREEN      obs_module_text("GameCapture.AnyFullscreen")
#define TEXT_SLI_COMPATIBILITY   obs_module_text("Compatibility")
#define TEXT_ALLOW_TRANSPARENCY  obs_module_text("AllowTransparency")
#define TEXT_FORCE_SCALING       obs_module_text("GameCapture.ForceScaling")
#define TEXT_SCALE_RES           obs_module_text("GameCapture.ScaleRes")
#define TEXT_WINDOW              obs_module_text("WindowCapture.Window")
#define TEXT_MATCH_PRIORITY      obs_module_text("WindowCapture.Priority")
#define TEXT_MATCH_TITLE         obs_module_text("WindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS         obs_module_text("WindowCapture.Priority.Class")
#define TEXT_MATCH_EXE           obs_module_text("WindowCapture.Priority.Exe")
#define TEXT_CAPTURE_CURSOR      obs_module_text("CaptureCursor")
#define TEXT_LIMIT_FRAMERATE     obs_module_text("GameCapture.LimitFramerate")
#define TEXT_CAPTURE_OVERLAYS    obs_module_text("GameCapture.CaptureOverlays")
#define TEXT_ANTI_CHEAT_HOOK     obs_module_text("GameCapture.AntiCheatHook")

#define DEFAULT_RETRY_INTERVAL 2.0f
#define ERROR_RETRY_INTERVAL 4.0f

struct game_capture_config {
	char                          *title;
	char                          *class;
	char                          *executable;
	enum window_priority          priority;
	uint32_t                      scale_cx;
	uint32_t                      scale_cy;
	bool                          cursor : 1;
	bool                          force_shmem : 1;
	bool                          capture_any_fullscreen : 1;
	bool                          force_scaling : 1;
	bool                          allow_transparency : 1;
	bool                          limit_framerate : 1;
	bool                          capture_overlays : 1;
	bool                          anticheat_hook : 1;
	bool                          allow_ipc_injector : 1;
	char                          *overlay_dll;
	char                          *overlay_dll64;
	DWORD                         process_id;
	DWORD                         thread_id;
	HWND                          hwnd;
};

struct game_capture {
	obs_source_t                  *source;

	signal_handler_t              *signals;
	struct calldata               start_calldata;
	struct calldata               stop_calldata;
	struct calldata               inject_fail_calldata;
	struct calldata               ipc_inject_calldata;
	struct calldata               ipc_monitor_process_calldata;

	struct cursor_data            cursor_data;
	HANDLE                        injector_process;
	uint32_t                      cx;
	uint32_t                      cy;
	uint32_t                      pitch;
	DWORD                         process_id;
	DWORD                         thread_id;
	HWND                          next_window;
	HWND                          window;
	float                         retry_time;
	float                         fps_reset_time;
	float                         retry_interval;
	int                           retries;
	bool                          wait_for_target_startup : 1;
	bool                          showing : 1;
	bool                          active : 1;
	bool                          capturing : 1;
	bool                          did_capture : 1;
	bool                          activate_hook : 1;
	bool                          process_is_64bit : 1;
	bool                          ipc_injector_active : 1;
	bool                          error_acquiring : 1;
	bool                          dwm_capture : 1;
	bool                          initial_config : 1;
	bool                          convert_16bit : 1;
	bool                          pipe_initialized : 1;

	CRITICAL_SECTION              ipc_mutex;
	DWORD                         ipc_result;
	bool                          have_ipc_result : 1;
	bool                          monitored_process_died : 1;

	struct game_capture_config    config;

	ipc_pipe_server_t             pipe;
	gs_texture_t                  *texture;
	struct hook_info              *global_hook_info;
	HANDLE                        keep_alive;
	HANDLE                        hook_restart;
	HANDLE                        hook_stop;
	HANDLE                        hook_ready;
	HANDLE                        hook_exit;
	HANDLE                        hook_data_map;
	HANDLE                        global_hook_info_map;
	HANDLE                        target_process;
	HANDLE                        texture_mutexes[2];

	uint32_t                      last_map_id;

	struct {
		gs_texrender_t            *copy_tex;
		gs_stagesurf_t            *surf;
		bool                      requested : 1;
		bool                      copied : 1;
		bool                      staged : 1;
		bool                      saved : 1;

		HANDLE                    save_thread;
		
		struct calldata           calldata;

		CRITICAL_SECTION          mutex;
		long long                 id;
		struct dstr               name;
	} screenshot;

	union {
		struct {
			struct shmem_data *shmem_data;
			uint8_t *texture_buffers[2];
		};

		struct shtex_data *shtex_data;
		void *data;
	};

	void (*copy_texture)(struct game_capture*);
};

struct graphics_offsets offsets32 = {0};
struct graphics_offsets offsets64 = {0};

static inline enum gs_color_format convert_format(uint32_t format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:     return GS_RGBA;
	case DXGI_FORMAT_B8G8R8X8_UNORM:     return GS_BGRX;
	case DXGI_FORMAT_B8G8R8A8_UNORM:     return GS_BGRA;
	case DXGI_FORMAT_R10G10B10A2_UNORM:  return GS_R10G10B10A2;
	case DXGI_FORMAT_R16G16B16A16_UNORM: return GS_RGBA16;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return GS_RGBA16F;
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return GS_RGBA32F;
	}

	return GS_UNKNOWN;
}

static void close_handle(HANDLE *p_handle)
{
	HANDLE handle = *p_handle;
	if (handle) {
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		*p_handle = NULL;
	}
}

static inline HMODULE kernel32(void)
{
	static HMODULE kernel32_handle = NULL;
	if (!kernel32_handle)
		kernel32_handle = GetModuleHandleW(L"kernel32");
	return kernel32_handle;
}

static inline HANDLE open_process(DWORD desired_access, bool inherit_handle,
		DWORD process_id)
{
	static HANDLE (WINAPI *open_process_proc)(DWORD, BOOL, DWORD) = NULL;
	if (!open_process_proc)
		open_process_proc = get_obfuscated_func(kernel32(),
				"NuagUykjcxr", 0x1B694B59451ULL);

	return open_process_proc(desired_access, inherit_handle, process_id);
}

static inline bool target_process_died(struct game_capture *gc);

static void stop_capture(struct game_capture *gc)
{
	if (gc->hook_stop) {
		SetEvent(gc->hook_stop);
	}

	if (target_process_died(gc)) {
		signal_handler_signal(gc->signals, "stop_capture", &gc->stop_calldata);
		gc->did_capture = false;
		close_handle(&gc->target_process);

		gc->last_map_id = 0;
	}

	gc->copy_texture = NULL;
	gc->wait_for_target_startup = false;
	gc->active = false;
	gc->capturing = false;
}

static void close_capture(struct game_capture *gc)
{
	stop_capture(gc);

	ipc_pipe_server_free(&gc->pipe);
	gc->pipe_initialized = false;

	if (gc->global_hook_info) {
		UnmapViewOfFile(gc->global_hook_info);
		gc->global_hook_info = NULL;
	}
	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	while (gc->screenshot.save_thread) {
		switch (WaitForSingleObject(gc->screenshot.save_thread, INFINITE)) {
		case WAIT_OBJECT_0:
		case WAIT_FAILED:
			close_handle(&gc->screenshot.save_thread);
		}
	}

	close_handle(&gc->keep_alive);
	close_handle(&gc->hook_restart);
	close_handle(&gc->hook_stop);
	close_handle(&gc->hook_ready);
	close_handle(&gc->hook_exit);
	close_handle(&gc->hook_data_map);
	close_handle(&gc->global_hook_info_map);
	close_handle(&gc->texture_mutexes[0]);
	close_handle(&gc->texture_mutexes[1]);

	if (gc->texture) {
		struct obs_graphics_defer_cleanup odgc[] = {
			{gc->texture, OBS_CLEANUP_DEFER_TEXTURE},
			{gc->screenshot.surf, OBS_CLEANUP_DEFER_STAGESURF}
		};
		obs_defer_graphics_cleanup(sizeof(odgc)/sizeof(odgc[0]), odgc);
		gc->texture = NULL;
		gc->screenshot.surf = NULL;
	}
}

static inline void free_config(struct game_capture_config *config)
{
	bfree(config->title);
	bfree(config->class);
	bfree(config->executable);
	bfree(config->overlay_dll);
	bfree(config->overlay_dll64);
	memset(config, 0, sizeof(*config));
}

static void game_capture_destroy(void *data)
{
	struct game_capture *gc = data;
	close_capture(gc);
	close_handle(&gc->target_process);

	cursor_data_free_deferred(&gc->cursor_data);

	struct obs_graphics_defer_cleanup cleanup = {
		gc->screenshot.copy_tex, OBS_CLEANUP_DEFER_TEXRENDER
	};
	obs_defer_graphics_cleanup(1, &cleanup);

	free_config(&gc->config);

	DeleteCriticalSection(&gc->screenshot.mutex);
	DeleteCriticalSection(&gc->ipc_mutex);

	calldata_free(&gc->screenshot.calldata);
	calldata_free(&gc->ipc_monitor_process_calldata);
	calldata_free(&gc->ipc_inject_calldata);
	calldata_free(&gc->inject_fail_calldata);
	calldata_free(&gc->stop_calldata);
	calldata_free(&gc->start_calldata);

	bfree(gc);
}

static inline void get_config(struct game_capture_config *cfg,
		obs_data_t *settings, const char *window)
{
	int ret;
	const char *scale_str;

	build_window_strings(window, &cfg->class, &cfg->title,
			&cfg->executable);

	cfg->capture_any_fullscreen = obs_data_get_bool(settings,
			SETTING_ANY_FULLSCREEN);
	cfg->priority = (enum window_priority)obs_data_get_int(settings,
			SETTING_WINDOW_PRIORITY);
	cfg->force_shmem = obs_data_get_bool(settings,
			SETTING_COMPATIBILITY);
	cfg->cursor = obs_data_get_bool(settings, SETTING_CURSOR);
	cfg->allow_transparency = obs_data_get_bool(settings,
			SETTING_TRANSPARENCY);
	cfg->force_scaling = obs_data_get_bool(settings,
			SETTING_FORCE_SCALING);
	cfg->limit_framerate = obs_data_get_bool(settings,
			SETTING_LIMIT_FRAMERATE);
	cfg->capture_overlays = obs_data_get_bool(settings,
			SETTING_CAPTURE_OVERLAYS);
	cfg->anticheat_hook = obs_data_get_bool(settings,
			SETTING_ANTI_CHEAT_HOOK);
	cfg->allow_ipc_injector = obs_data_get_bool(settings,
			SETTING_ALLOW_IPC_INJ);

	scale_str = obs_data_get_string(settings, SETTING_SCALE_RES);
	ret = sscanf(scale_str, "%"PRIu32"x%"PRIu32,
			&cfg->scale_cx, &cfg->scale_cy);

	cfg->scale_cx &= ~2;
	cfg->scale_cy &= ~2;

	if (cfg->force_scaling) {
		if (ret != 2 || cfg->scale_cx == 0 || cfg->scale_cy == 0) {
			cfg->scale_cx = 0;
			cfg->scale_cy = 0;
		}
	}

	cfg->overlay_dll = bstrdup(
			obs_data_get_string(settings, SETTING_OVERLAY_DLL));
	cfg->overlay_dll64 = bstrdup(
			obs_data_get_string(settings, SETTING_OVERLAY_DLL64));

	cfg->process_id = (DWORD)obs_data_get_int(settings, SETTING_PROCESS_ID);
	cfg->thread_id = (DWORD)obs_data_get_int(settings, SETTING_THREAD_ID);
	cfg->hwnd = (HWND)obs_data_get_int(settings, SETTING_HWND);
}

static inline int s_cmp(const char *str1, const char *str2)
{
	if (!str1 || !str2)
		return -1;

	return strcmp(str1, str2);
}

static inline bool capture_needs_reset(struct game_capture_config *cfg1,
		struct game_capture_config *cfg2)
{
	if (cfg1->capture_any_fullscreen != cfg2->capture_any_fullscreen) {
		return true;

	} else if (!cfg1->capture_any_fullscreen &&
			(s_cmp(cfg1->class, cfg2->class) != 0 ||
			 s_cmp(cfg1->title, cfg2->title) != 0 ||
			 s_cmp(cfg1->executable, cfg2->executable) != 0 ||
			 cfg1->priority != cfg2->priority)) {
		return true;

	} else if (cfg1->force_scaling != cfg2->force_scaling) {
		return true;

	} else if (cfg1->force_scaling &&
			(cfg1->scale_cx != cfg2->scale_cx ||
			 cfg1->scale_cy != cfg2->scale_cy)) {
		return true;

	} else if (cfg1->force_shmem != cfg2->force_shmem) {
		return true;

	} else if (cfg1->limit_framerate != cfg2->limit_framerate) {
		return true;

	} else if (cfg1->capture_overlays != cfg2->capture_overlays) {
		return true;

	} else if (cfg1->overlay_dll != cfg2->overlay_dll ||
			strcmp(cfg1->overlay_dll, cfg2->overlay_dll)) {
		return true;
	} else if (cfg1->overlay_dll64 != cfg2->overlay_dll64 ||
			strcmp(cfg1->overlay_dll64, cfg2->overlay_dll64)) {
		return true;
	}

	return false;
}

static void game_capture_update(void *data, obs_data_t *settings)
{
	struct game_capture *gc = data;
	struct game_capture_config cfg;
	bool reset_capture = false;
	const char *window = obs_data_get_string(settings,
			SETTING_CAPTURE_WINDOW);

	get_config(&cfg, settings, window);
	reset_capture = (cfg.process_id && cfg.process_id != gc->process_id)
		|| (cfg.hwnd && cfg.hwnd != gc->window)
		|| capture_needs_reset(&cfg, &gc->config);

	if (cfg.force_scaling && (cfg.scale_cx == 0 || cfg.scale_cy == 0)) {
		gc->error_acquiring = true;
		warn("error acquiring, scale is bad");
	} else {
		gc->error_acquiring = false;
	}

	if (!cfg.process_id || cfg.process_id != gc->process_id)
		gc->monitored_process_died = false;

	free_config(&gc->config);
	gc->config = cfg;
	gc->activate_hook = cfg.process_id ||
		(!!window && !!*window);
	gc->retry_interval = DEFAULT_RETRY_INTERVAL;
	gc->wait_for_target_startup = false;
	gc->have_ipc_result = false;
	gc->ipc_injector_active = false;

	if (reset_capture || !cfg.process_id)
		close_handle(&gc->target_process);

	if (!gc->initial_config) {
		if (reset_capture) {
			close_capture(gc);
		}
	} else {
		gc->initial_config = false;
	}
}

static void update_ipc_injector_calldata(struct game_capture *gc,
	bool process_is_64bit, bool anti_cheat, DWORD process_thread_id)
{
	calldata_set_bool(&gc->ipc_inject_calldata, "process_is_64bit",
		process_is_64bit);
	calldata_set_bool(&gc->ipc_inject_calldata, "anti_cheat", anti_cheat);
	calldata_set_int(&gc->ipc_inject_calldata, "process_thread_id",
		process_thread_id);
	calldata_set_string(&gc->ipc_inject_calldata, "hook_dir", obs_module_file(""));
}

void injector_result(void *context, calldata_t *data)
{
	struct game_capture *gc = context;
	DWORD code = (DWORD)calldata_int(data, "code");

	EnterCriticalSection(&gc->ipc_mutex);
	gc->have_ipc_result = true;
	gc->ipc_result = code;
	LeaveCriticalSection(&gc->ipc_mutex);
}

void monitored_process_exit(void *context, calldata_t *data)
{
	struct game_capture *gc = context;
	DWORD process_id = (DWORD)calldata_int(data, "process_id");

	EnterCriticalSection(&gc->ipc_mutex);
	if (gc->process_id == process_id)
		gc->monitored_process_died = true;
	LeaveCriticalSection(&gc->ipc_mutex);
}

void screenshot_requested(void *context, calldata_t *data)
{
	struct game_capture *gc = context;
	const char *filename = calldata_string(data, "filename");
	bool filename_used = false;
	long long id = 0;

	EnterCriticalSection(&gc->screenshot.mutex);
	if ((filename_used = !gc->screenshot.name.len)) {
		dstr_copy(&gc->screenshot.name, filename);
		id = ++gc->screenshot.id;
	} else {
		id = gc->screenshot.id;
	}
	LeaveCriticalSection(&gc->screenshot.mutex);

	calldata_set_bool(data, "filename_used", filename_used);
	calldata_set_int(data, "screenshot_id", id);
}

static const char *capture_signals[] = {
	"void start_capture(ptr source, int width, int height)",
	"void stop_capture(ptr source)",
	"void inject_failed(ptr source, ptr injector_exit_code)",
	"void inject_request(ptr source, bool process_is_64bit, "
	                    "bool anti_cheat, int process_thread_id, "
	                    "string hook_dir)",
	"void monitor_process(ptr source, int process_id)",
	"void screenshot_saved(ptr source, string filename, int screenshot_id)",
	"void process_inaccessible(ptr source, int process_id)",
	NULL
};

static void *game_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct game_capture *gc = bzalloc(sizeof(*gc));
	gc->source = source;
	gc->initial_config = true;
	gc->retry_interval = DEFAULT_RETRY_INTERVAL;

	gc->signals = obs_source_get_signal_handler(source);
	signal_handler_add_array(gc->signals, capture_signals);

	calldata_init(&gc->start_calldata);
	calldata_set_int(&gc->start_calldata, "width", 0);
	calldata_set_int(&gc->start_calldata, "height", 0);
	calldata_set_ptr(&gc->start_calldata, "source", source);

	calldata_init(&gc->stop_calldata);
	calldata_set_ptr(&gc->stop_calldata, "source", source);

	calldata_init(&gc->inject_fail_calldata);
	calldata_set_ptr(&gc->inject_fail_calldata, "source", source);

	calldata_init(&gc->ipc_inject_calldata);
	calldata_set_ptr(&gc->ipc_inject_calldata, "source", source);
	update_ipc_injector_calldata(gc, false, false, 0);

	calldata_init(&gc->ipc_monitor_process_calldata);
	calldata_set_int(&gc->ipc_monitor_process_calldata, "process_id", 0);
	calldata_set_ptr(&gc->ipc_monitor_process_calldata, "source", source);

	calldata_init(&gc->screenshot.calldata);
	calldata_set_ptr(&gc->screenshot.calldata, "source", source);
	calldata_set_int(&gc->screenshot.calldata, "screenshot_id", 0);

	InitializeCriticalSection(&gc->ipc_mutex);
	InitializeCriticalSection(&gc->screenshot.mutex);

	proc_handler_t *proc = obs_source_get_proc_handler(source);
	proc_handler_add(proc, "void injector_result(int code)",
			injector_result, gc);

	proc_handler_add(proc, "void monitored_process_exit(int process_id, "
			                                           "int code)",
			monitored_process_exit, gc);

	proc_handler_add(proc, "void save_screenshot(string filename, "
			                                    "out int screenshot_id, "
			                                    "out bool filename_used)",
			screenshot_requested, gc);

	game_capture_update(gc, settings);
	return gc;
}

static inline HANDLE create_event_id(bool manual_reset, bool initial_state,
		const char *name, DWORD process_id)
{
	char new_name[128];
	sprintf(new_name, "%s%lu", name, process_id);
	return CreateEventA(NULL, manual_reset, initial_state, new_name);
}

static inline HANDLE open_event_id(const char *name, DWORD process_id)
{
	char new_name[128];
	sprintf(new_name, "%s%lu", name, process_id);
	return OpenEventA(EVENT_ALL_ACCESS, false, new_name);
}

#define STOP_BEING_BAD \
	"  This is most likely due to security software. Please make sure " \
        "that the OBS installation folder is excluded/ignored in the "      \
        "settings of the security software you are using."

static bool check_file_integrity(struct game_capture *gc, const char *file,
		const char *name)
{
	DWORD error;
	HANDLE handle;
	wchar_t *w_file = NULL;

	if (!file || !*file) {
		warn("Game capture %s not found." STOP_BEING_BAD, name);
		return false;
	}

	if (!os_utf8_to_wcs_ptr(file, 0, &w_file)) {
		warn("Could not convert file name to wide string");
		return false;
	}

	handle = CreateFileW(w_file, GENERIC_READ | GENERIC_EXECUTE,
			FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	bfree(w_file);

	if (handle != INVALID_HANDLE_VALUE) {
		CloseHandle(handle);
		return true;
	}

	error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND) {
		warn("Game capture file '%s' not found."
				STOP_BEING_BAD, file);
	} else if (error == ERROR_ACCESS_DENIED) {
		warn("Game capture file '%s' could not be loaded."
				STOP_BEING_BAD, file);
	} else {
		warn("Game capture file '%s' could not be loaded: %lu."
				STOP_BEING_BAD, file, error);
	}

	return false;
}

static inline bool is_64bit_windows(void)
{
#ifdef _WIN64
	return true;
#else
	BOOL x86 = false;
	bool success = !!IsWow64Process(GetCurrentProcess(), &x86);
	return success && !!x86;
#endif
}

static inline bool is_64bit_process(HANDLE process)
{
	BOOL x86 = true;
	if (is_64bit_windows()) {
		bool success = !!IsWow64Process(process, &x86);
		if (!success) {
			return false;
		}
	}

	return !x86;
}

static void signal_process_inaccessible(struct game_capture *gc)
{
	uint8_t stack[128];

	calldata_t data;
	calldata_init_fixed(&data, stack, sizeof(stack)/sizeof(uint8_t));
	calldata_set_ptr(&data, "source", &gc->source);
	calldata_set_int(&data, "process_id", gc->process_id);
	signal_handler_signal(gc->signals, "process_inaccessible", &data);
}

static inline bool open_target_process(struct game_capture *gc)
{
	if (gc->target_process)
		goto check_alive;

	gc->target_process = open_process(
			PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
			false, gc->process_id);
	if (!gc->target_process) {
		warn("process '%ld' inaccessible, giving up", (long)gc->process_id);
		gc->error_acquiring = true;

		signal_process_inaccessible(gc);

		return false;
	}

	gc->process_is_64bit = is_64bit_process(gc->target_process);

check_alive:
	return !target_process_died(gc);
}

static inline bool init_keepalive(struct game_capture *gc)
{
	if (gc->keep_alive)
		return true;

	gc->keep_alive = create_event_id(false, false, EVENT_HOOK_KEEPALIVE,
			gc->process_id);
	if (!gc->keep_alive) {
		warn("failed to create keepalive event");
		return false;
	}

	return true;
}

static inline bool init_texture_mutexes(struct game_capture *gc)
{
	if (gc->texture_mutexes[0] && gc->texture_mutexes[1])
		return true;

	gc->texture_mutexes[0] = get_mutex_plus_id(MUTEX_TEXTURE1,
			gc->process_id);
	gc->texture_mutexes[1] = get_mutex_plus_id(MUTEX_TEXTURE2,
			gc->process_id);

	if (!gc->texture_mutexes[0] || !gc->texture_mutexes[1]) {
		warn("failed to create texture mutexes: %lu", GetLastError());
		return false;
	}

	return true;
}

/* if there's already a hook in the process, then signal and start */
static inline bool attempt_existing_hook(struct game_capture *gc)
{
	gc->hook_restart = open_event_id(EVENT_CAPTURE_RESTART, gc->process_id);
	if (gc->hook_restart) {
		if (gc->config.executable)
			debug("existing hook found, signaling process: %s",
					gc->config.executable);
		else
			debug("existing hook found, signaling process id: %d",
					gc->process_id);
		SetEvent(gc->hook_restart);
		return true;
	}

	return false;
}

static inline void reset_frame_interval(struct game_capture *gc)
{
	struct obs_video_info ovi;
	uint64_t interval = 0;

	if (obs_get_video_info(&ovi)) {
		interval = ovi.fps_den * 1000000000ULL / ovi.fps_num;

		/* Always limit capture framerate to some extent.  If a game
		 * running at 900 FPS is being captured without some sort of
		 * limited capture interval, it will dramatically reduce
		 * performance. */
		if (!gc->config.limit_framerate)
			interval /= 2;
	}

	gc->global_hook_info->frame_interval = interval;
}

static inline bool init_hook_info(struct game_capture *gc)
{
	if (gc->global_hook_info_map && gc->global_hook_info)
		goto copy_config;

	gc->global_hook_info_map = get_hook_info(gc->process_id);
	if (!gc->global_hook_info_map) {
		warn("init_hook_info: get_hook_info failed: %lu",
				GetLastError());
		return false;
	}

	gc->global_hook_info = MapViewOfFile(gc->global_hook_info_map,
			FILE_MAP_ALL_ACCESS, 0, 0,
			sizeof(*gc->global_hook_info));
	if (!gc->global_hook_info) {
		warn("init_hook_info: failed to map data view: %lu",
				GetLastError());
		return false;
	}

copy_config:
	gc->global_hook_info->offsets = gc->process_is_64bit ?
		offsets64 : offsets32;
	gc->global_hook_info->capture_overlay = gc->config.capture_overlays;
	gc->global_hook_info->force_shmem = gc->config.force_shmem;
	gc->global_hook_info->use_scale = gc->config.force_scaling;
	if (gc->config.scale_cx)
		gc->global_hook_info->cx = gc->config.scale_cx;
	if (gc->config.scale_cy)
		gc->global_hook_info->cy = gc->config.scale_cy;
	reset_frame_interval(gc);

	const char *path = gc->process_is_64bit ?
		(gc->config.overlay_dll64 ? gc->config.overlay_dll64 : "") :
		(gc->config.overlay_dll ? gc->config.overlay_dll : "");
	strncpy(gc->global_hook_info->overlay_dll_path, path, MAX_PATH);

	obs_enter_graphics();
	LUID *luid = (LUID*)gs_get_device_luid();
	gc->global_hook_info->luid_valid = !!luid;
	if (luid)
		gc->global_hook_info->luid = *luid;

	if (!gs_shared_texture_available())
		gc->global_hook_info->force_shmem = true;
	obs_leave_graphics();

	obs_enter_graphics();
	if (!gs_shared_texture_available())
		gc->global_hook_info->force_shmem = true;
	obs_leave_graphics();

	return true;
}

static void pipe_log(void *param, uint8_t *data, size_t size)
{
	struct game_capture *gc = param;
	if (data && size)
		info("%s", data);
}

static inline bool init_pipe(struct game_capture *gc)
{
	if (gc->pipe_initialized)
		return true;

	char name[64];
	sprintf(name, "%s%lu", PIPE_NAME, gc->process_id);

	if (!ipc_pipe_server_start(&gc->pipe, name, pipe_log, gc)) {
		warn("init_pipe: failed to start pipe");
		return false;
	}

	gc->pipe_initialized = true;

	return true;
}

static inline int inject_library(HANDLE process, const wchar_t *dll)
{
	return inject_library_obf(process, dll,
			"D|hkqkW`kl{k\\osofj", 0xa178ef3655e5ade7,
			"[uawaRzbhh{tIdkj~~", 0x561478dbd824387c,
			"[fr}pboIe`dlN}", 0x395bfbc9833590fd,
			"\\`zs}gmOzhhBq", 0x12897dd89168789a,
			"GbfkDaezbp~X", 0x76aff7238788f7db);
}

static inline bool hook_direct(struct game_capture *gc,
		const char *hook_path_rel, int *ret)
{
	wchar_t hook_path_abs_w[MAX_PATH];
	wchar_t *hook_path_rel_w;
	wchar_t *path_ret;
	HANDLE process;

	os_utf8_to_wcs_ptr(hook_path_rel, 0, &hook_path_rel_w);
	if (!hook_path_rel_w) {
		warn("hook_direct: could not convert string");
		return false;
	}

	path_ret = _wfullpath(hook_path_abs_w, hook_path_rel_w, MAX_PATH);
	bfree(hook_path_rel_w);

	if (path_ret == NULL) {
		warn("hook_direct: could not make absolute path");
		return false;
	}

	process = open_process(PROCESS_ALL_ACCESS, false, gc->process_id);
	if (!process) {
		if (gc->config.executable)
			warn("hook_direct: could not open process: %s (%lu)",
					gc->config.executable, GetLastError());
		else {
			warn("hook_direct: could not open process id: %d (%lu)",
				gc->process_id, GetLastError());
			gc->error_acquiring = true;
		}
		return false;
	}

	*ret = inject_library(process, hook_path_abs_w);
	CloseHandle(process);

	if (*ret != 0) {
		warn("hook_direct: inject failed: %d", *ret);
		calldata_set_ptr(&gc->inject_fail_calldata, "injector_exit_code", ret);
		return false;
	}

	return true;
}

static inline bool create_inject_process(struct game_capture *gc,
		const char *inject_path, const char *hook_dll)
{
	wchar_t *command_line_w = malloc(4096 * sizeof(wchar_t));
	wchar_t *inject_path_w;
	wchar_t *hook_dll_w;
	bool anti_cheat = gc->config.anticheat_hook;
	PROCESS_INFORMATION pi = {0};
	STARTUPINFO si = {0};
	bool success = false;

	os_utf8_to_wcs_ptr(inject_path, 0, &inject_path_w);
	os_utf8_to_wcs_ptr(hook_dll, 0, &hook_dll_w);

	si.cb = sizeof(si);

	swprintf(command_line_w, 4096, L"\"%s\" \"%s\" %lu %lu",
			inject_path_w, hook_dll_w,
			(unsigned long)anti_cheat, gc->process_id);

	success = !!CreateProcessW(inject_path_w, command_line_w, NULL, NULL,
			false, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	if (success) {
		CloseHandle(pi.hThread);
		gc->injector_process = pi.hProcess;
	} else {
		warn("Failed to create inject helper process: %lu",
				GetLastError());
	}

	free(command_line_w);
	bfree(inject_path_w);
	bfree(hook_dll_w);
	return success;
}

static inline bool inject_hook(struct game_capture *gc)
{
	bool matching_architecture;
	bool success = false;
	const char *hook_dll;
	char *inject_path;
	char *hook_path;
	int inject_result;

	if (gc->config.allow_ipc_injector) {
		bool anti_cheat = gc->config.anticheat_hook;

		update_ipc_injector_calldata(gc, gc->process_is_64bit,
				anti_cheat, gc->process_id);

		signal_handler_signal(gc->signals, "inject_request",
				&gc->ipc_inject_calldata);

		gc->ipc_injector_active = true;
		return true;
	}

	if (gc->process_is_64bit) {
		hook_dll = "graphics-hook64.dll";
		inject_path = obs_module_file("inject-helper64.exe");
	} else {
		hook_dll = "graphics-hook32.dll";
		inject_path = obs_module_file("inject-helper32.exe");
	}

	hook_path = obs_module_file(hook_dll);

	if (!check_file_integrity(gc, inject_path, "inject helper")) {
		goto cleanup;
	}
	if (!check_file_integrity(gc, hook_path, "graphics hook")) {
		goto cleanup;
	}

#ifdef _WIN64
	matching_architecture = gc->process_is_64bit;
#else
	matching_architecture = !gc->process_is_64bit;
#endif

	if (matching_architecture && !gc->config.anticheat_hook) {
		info("using direct hook");
		success = hook_direct(gc, hook_path, &inject_result);
	} else {
		info("using helper (%s hook)", gc->config.anticheat_hook ?
				"compatibility" : "direct");
		success = create_inject_process(gc, inject_path, hook_dll);
	}

cleanup:
	bfree(inject_path);
	bfree(hook_path);

	if (!success) {
		signal_handler_signal(gc->signals, "inject_failed", &gc->inject_fail_calldata);
		calldata_set_ptr(&gc->inject_fail_calldata, "injector_exit_code", NULL);
	}

	return success;
}

static bool init_capture(struct game_capture *gc)
{
	if (!open_target_process(gc)) {
		return false;
	}
	if (!init_keepalive(gc)) {
		return false;
	}
	if (!init_texture_mutexes(gc)) {
		return false;
	}
	if (!init_hook_info(gc)) {
		return false;
	}
	if (!init_pipe(gc)) {
		return false;
	}

	return true;
}

static bool init_hook(struct game_capture *gc)
{
	if (gc->config.capture_any_fullscreen) {
		struct dstr name = {0};
		if (get_window_exe(&name, gc->next_window)) {
			info("attempting to hook fullscreen process: %s",
					name.array);
			dstr_free(&name);
		}
	} else if (gc->config.thread_id || gc->config.process_id) {
		info("attempting to hook process id %lu (thread id %lu)",
				gc->config.process_id, gc->config.thread_id);
	} else {
		info("attempting to hook process: %s", gc->config.executable);
	}

	if (!attempt_existing_hook(gc)) {
		if (!inject_hook(gc)) {
			return false;
		}
	}

	gc->window = gc->next_window;
	gc->next_window = NULL;
	gc->active = true;
	return true;
}

static void setup_window(struct game_capture *gc, HWND window)
{
	DWORD process_id = 0;
	HANDLE hook_restart;

	GetWindowThreadProcessId(window, &process_id);

	/* do not wait if we're re-hooking a process */
	hook_restart = open_event_id(EVENT_CAPTURE_RESTART, process_id);
	if (hook_restart) {
		gc->wait_for_target_startup = false;
		CloseHandle(hook_restart);
	}

	/* otherwise if it's an unhooked process, always wait a bit for the
	 * target process to start up before starting the hook process;
	 * sometimes they have important modules to load first or other hooks
	 * (such as steam) need a little bit of time to load.  ultimately this
	 * helps prevent crashes */
	if (gc->wait_for_target_startup) {
		gc->retry_interval = 3.0f;
		gc->wait_for_target_startup = false;
	} else {
		gc->next_window = window;
	}
}

static void get_fullscreen_window(struct game_capture *gc)
{
	HWND window = GetForegroundWindow();
	MONITORINFO mi = {0};
	HMONITOR monitor;
	DWORD styles;
	RECT rect;

	gc->next_window = NULL;

	if (!window) {
		return;
	}
	if (!GetWindowRect(window, &rect)) {
		return;
	}

	/* ignore regular maximized windows */
	styles = (DWORD)GetWindowLongPtr(window, GWL_STYLE);
	if ((styles & WS_MAXIMIZE) != 0 && (styles & WS_BORDER) != 0) {
		return;
	}

	monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
	if (!monitor) {
		return;
	}

	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfo(monitor, &mi)) {
		return;
	}

	if (rect.left   == mi.rcMonitor.left   &&
	    rect.right  == mi.rcMonitor.right  &&
	    rect.bottom == mi.rcMonitor.bottom &&
	    rect.top    == mi.rcMonitor.top) {
		setup_window(gc, window);
	} else {
		gc->wait_for_target_startup = true;
	}
}

static void get_selected_window(struct game_capture *gc)
{
	HWND window;

	if (strcmpi(gc->config.class, "dwm") == 0) {
		wchar_t class_w[512];
		os_utf8_to_wcs(gc->config.class, 0, class_w, 512);
		window = FindWindowW(class_w, NULL);
	} else {
		window = find_window(INCLUDE_MINIMIZED,
				gc->config.priority,
				gc->config.class,
				gc->config.title,
				gc->config.executable);
	}

	if (window) {
		setup_window(gc, window);
	} else {
		gc->wait_for_target_startup = true;
	}
}

static void try_hook(struct game_capture *gc)
{
	if (gc->config.process_id) {
		gc->process_id = gc->config.process_id;
		gc->next_window = gc->config.hwnd;

		if (!init_capture(gc))
			close_capture(gc);

		else if (!init_hook(gc))
			stop_capture(gc);

		return;
	}

	if (gc->config.capture_any_fullscreen) {
		get_fullscreen_window(gc);
	} else {
		get_selected_window(gc);
	}

	if (gc->next_window) {
		gc->thread_id = GetWindowThreadProcessId(gc->next_window,
				&gc->process_id);

		// Make sure we never try to hook ourselves (projector)
		if (gc->process_id == GetCurrentProcessId())
			return;

		if (!gc->thread_id || !gc->process_id) {
			warn("error acquiring, failed to get window "
					"thread/process ids: %lu",
					GetLastError());
			gc->error_acquiring = true;
			return;
		}

		if (!init_capture(gc))
			close_capture(gc);
		else if (!init_hook(gc))
			stop_capture(gc);

	} else {
		gc->active = false;
	}
}

static inline bool init_events(struct game_capture *gc)
{
	if (!gc->hook_restart) {
		gc->hook_restart = get_event_plus_id(EVENT_CAPTURE_RESTART,
				gc->process_id);
		if (!gc->hook_restart) {
			warn("init_events: failed to get hook_restart "
			     "event: %lu", GetLastError());
			return false;
		}
	}

	if (!gc->hook_stop) {
		gc->hook_stop = get_event_plus_id(EVENT_CAPTURE_STOP,
				gc->process_id);
		if (!gc->hook_stop) {
			warn("init_events: failed to get hook_stop event: %lu",
					GetLastError());
			return false;
		}
	}

	if (!gc->hook_ready) {
		gc->hook_ready = get_event_plus_id(EVENT_HOOK_READY,
				gc->process_id);
		if (!gc->hook_ready) {
			warn("init_events: failed to get hook_ready event: %lu",
					GetLastError());
			return false;
		}
	}

	if (!gc->hook_exit) {
		gc->hook_exit = get_event_plus_id(EVENT_HOOK_EXIT,
				gc->process_id);
		if (!gc->hook_exit) {
			warn("init_events: failed to get hook_exit event: %lu",
					GetLastError());
			return false;
		}
	}

	return true;
}

enum capture_result {
	CAPTURE_FAIL,
	CAPTURE_RETRY,
	CAPTURE_SUCCESS
};

static inline enum capture_result init_capture_data(struct game_capture *gc)
{
	char name[64];
	sprintf(name, "%s%u", SHMEM_TEXTURE, gc->global_hook_info->map_id);

	gc->cx = gc->global_hook_info->cx;
	gc->cy = gc->global_hook_info->cy;
	gc->pitch = gc->global_hook_info->pitch;

	if (gc->data) {
		UnmapViewOfFile(gc->data);
		gc->data = NULL;
	}

	CloseHandle(gc->hook_data_map);

	gc->hook_data_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, name);
	if (!gc->hook_data_map) {
		DWORD error = GetLastError();
		if (error == 2) {
			if (gc->global_hook_info->map_id != gc->last_map_id) {
				gc->last_map_id = gc->global_hook_info->map_id;
				warn("init_capture_data: couldn't open hook_data_map %lu",
					gc->last_map_id);
			}
			return CAPTURE_RETRY;
		} else {
			warn("init_capture_data: failed to open file "
			     "mapping: %lu", error);
		}
		return CAPTURE_FAIL;
	}

	gc->data = MapViewOfFile(gc->hook_data_map, FILE_MAP_ALL_ACCESS, 0, 0,
			gc->global_hook_info->map_size);
	if (!gc->data) {
		warn("init_capture_data: failed to map data view: %lu",
				GetLastError());
		return CAPTURE_FAIL;
	}

	return CAPTURE_SUCCESS;
}

#define PIXEL_16BIT_SIZE 2
#define PIXEL_32BIT_SIZE 4

static inline uint32_t convert_5_to_8bit(uint16_t val)
{
	return (uint32_t)((double)(val & 0x1F) * (255.0/31.0));
}

static inline uint32_t convert_6_to_8bit(uint16_t val)
{
	return (uint32_t)((double)(val & 0x3F) * (255.0/63.0));
}

static void copy_b5g6r5_tex(struct game_capture *gc, int cur_texture,
		uint8_t *data, uint32_t pitch)
{
	uint8_t *input = gc->texture_buffers[cur_texture];
	uint32_t gc_cx = gc->cx;
	uint32_t gc_cy = gc->cy;
	uint32_t gc_pitch = gc->pitch;

	for (uint32_t y = 0; y < gc_cy; y++) {
		uint8_t *row = input + (gc_pitch * y);
		uint8_t *out = data + (pitch * y);

		for (uint32_t x = 0; x < gc_cx; x += 8) {
			__m128i pixels_blue, pixels_green, pixels_red;
			__m128i pixels_result;
			__m128i *pixels_dest;

			__m128i *pixels_src = (__m128i*)(row + x * sizeof(uint16_t));
			__m128i pixels = _mm_load_si128(pixels_src);

			__m128i zero = _mm_setzero_si128();
			__m128i pixels_low = _mm_unpacklo_epi16(pixels, zero);
			__m128i pixels_high = _mm_unpackhi_epi16(pixels, zero);

			__m128i blue_channel_mask = _mm_set1_epi32(0x0000001F);
			__m128i blue_offset = _mm_set1_epi32(0x00000003);
			__m128i green_channel_mask = _mm_set1_epi32(0x000007E0);
			__m128i green_offset = _mm_set1_epi32(0x00000008);
			__m128i red_channel_mask = _mm_set1_epi32(0x0000F800);
			__m128i red_offset = _mm_set1_epi32(0x00000300);

			pixels_blue = _mm_and_si128(pixels_low, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_low, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 5);

			pixels_red = _mm_and_si128(pixels_low, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 8);

			pixels_result = _mm_set1_epi32(0xFF000000);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);
			pixels_result = _mm_or_si128(pixels_result, pixels_red);

			pixels_dest = (__m128i*)(out + x * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);

			pixels_blue = _mm_and_si128(pixels_high, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_high, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 5);

			pixels_red = _mm_and_si128(pixels_high, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 8);

			pixels_result = _mm_set1_epi32(0xFF000000);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);
			pixels_result = _mm_or_si128(pixels_result, pixels_red);

			pixels_dest = (__m128i*)(out + (x + 4) * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);
		}
	}
}

static void copy_b5g5r5a1_tex(struct game_capture *gc, int cur_texture,
		uint8_t *data, uint32_t pitch)
{
	uint8_t *input = gc->texture_buffers[cur_texture];
	uint32_t gc_cx = gc->cx;
	uint32_t gc_cy = gc->cy;
	uint32_t gc_pitch = gc->pitch;

	for (uint32_t y = 0; y < gc_cy; y++) {
		uint8_t *row = input + (gc_pitch * y);
		uint8_t *out = data + (pitch * y);

		for (uint32_t x = 0; x < gc_cx; x += 8) {
			__m128i pixels_blue, pixels_green, pixels_red, pixels_alpha;
			__m128i pixels_result;
			__m128i *pixels_dest;

			__m128i *pixels_src = (__m128i*)(row + x * sizeof(uint16_t));
			__m128i pixels = _mm_load_si128(pixels_src);

			__m128i zero = _mm_setzero_si128();
			__m128i pixels_low = _mm_unpacklo_epi16(pixels, zero);
			__m128i pixels_high = _mm_unpackhi_epi16(pixels, zero);

			__m128i blue_channel_mask = _mm_set1_epi32(0x0000001F);
			__m128i blue_offset = _mm_set1_epi32(0x00000003);
			__m128i green_channel_mask = _mm_set1_epi32(0x000003E0);
			__m128i green_offset = _mm_set1_epi32(0x000000C);
			__m128i red_channel_mask = _mm_set1_epi32(0x00007C00);
			__m128i red_offset = _mm_set1_epi32(0x00000180);
			__m128i alpha_channel_mask = _mm_set1_epi32(0x00008000);
			__m128i alpha_offset = _mm_set1_epi32(0x00000001);
			__m128i alpha_mask32 = _mm_set1_epi32(0xFF000000);

			pixels_blue = _mm_and_si128(pixels_low, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_low, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 6);

			pixels_red = _mm_and_si128(pixels_low, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 9);

			pixels_alpha = _mm_and_si128(pixels_low, alpha_channel_mask);
			pixels_alpha = _mm_srli_epi32(pixels_alpha, 15);
			pixels_alpha = _mm_sub_epi32(pixels_alpha, alpha_offset);
			pixels_alpha = _mm_andnot_si128(pixels_alpha, alpha_mask32);

			pixels_result = pixels_red;
			pixels_result = _mm_or_si128(pixels_result, pixels_alpha);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);

			pixels_dest = (__m128i*)(out + x * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);

			pixels_blue = _mm_and_si128(pixels_high, blue_channel_mask);
			pixels_blue = _mm_slli_epi32(pixels_blue, 3);
			pixels_blue = _mm_add_epi32(pixels_blue, blue_offset);

			pixels_green = _mm_and_si128(pixels_high, green_channel_mask);
			pixels_green = _mm_add_epi32(pixels_green, green_offset);
			pixels_green = _mm_slli_epi32(pixels_green, 6);

			pixels_red = _mm_and_si128(pixels_high, red_channel_mask);
			pixels_red = _mm_add_epi32(pixels_red, red_offset);
			pixels_red = _mm_slli_epi32(pixels_red, 9);

			pixels_alpha = _mm_and_si128(pixels_high, alpha_channel_mask);
			pixels_alpha = _mm_srli_epi32(pixels_alpha, 15);
			pixels_alpha = _mm_sub_epi32(pixels_alpha, alpha_offset);
			pixels_alpha = _mm_andnot_si128(pixels_alpha, alpha_mask32);

			pixels_result = pixels_red;
			pixels_result = _mm_or_si128(pixels_result, pixels_alpha);
			pixels_result = _mm_or_si128(pixels_result, pixels_blue);
			pixels_result = _mm_or_si128(pixels_result, pixels_green);

			pixels_dest = (__m128i*)(out + (x + 4) * sizeof(uint32_t));
			_mm_store_si128(pixels_dest, pixels_result);
		}
	}
}

static inline void copy_16bit_tex(struct game_capture *gc, int cur_texture,
		uint8_t *data, uint32_t pitch)
{
	if (gc->global_hook_info->format == DXGI_FORMAT_B5G5R5A1_UNORM) {
		copy_b5g5r5a1_tex(gc, cur_texture, data, pitch);

	} else if (gc->global_hook_info->format == DXGI_FORMAT_B5G6R5_UNORM) {
		copy_b5g6r5_tex(gc, cur_texture, data, pitch);
	}
}

static void copy_shmem_tex(struct game_capture *gc)
{
	int cur_texture;
	HANDLE mutex = NULL;
	uint32_t pitch;
	int next_texture;
	uint8_t *data;

	if (!gc->shmem_data)
		return;

	cur_texture = gc->shmem_data->last_tex;

	if (cur_texture < 0 || cur_texture > 1)
		return;

	next_texture = cur_texture == 1 ? 0 : 1;

	if (object_signalled(gc->texture_mutexes[cur_texture])) {
		mutex = gc->texture_mutexes[cur_texture];

	} else if (object_signalled(gc->texture_mutexes[next_texture])) {
		mutex = gc->texture_mutexes[next_texture];
		cur_texture = next_texture;

	} else {
		return;
	}

	if (gs_texture_map(gc->texture, &data, &pitch)) {
		if (gc->convert_16bit) {
			copy_16bit_tex(gc, cur_texture, data, pitch);

		} else if (pitch == gc->pitch) {
			memcpy(data, gc->texture_buffers[cur_texture],
					pitch * gc->cy);
		} else {
			uint8_t *input = gc->texture_buffers[cur_texture];
			uint32_t best_pitch =
				pitch < gc->pitch ? pitch : gc->pitch;

			for (uint32_t y = 0; y < gc->cy; y++) {
				uint8_t *line_in = input + gc->pitch * y;
				uint8_t *line_out = data + pitch * y;
				memcpy(line_out, line_in, best_pitch);
			}
		}

		gs_texture_unmap(gc->texture);
	}

	ReleaseMutex(mutex);
}

static inline bool is_16bit_format(uint32_t format)
{
	return format == DXGI_FORMAT_B5G5R5A1_UNORM ||
	       format == DXGI_FORMAT_B5G6R5_UNORM;
}

static inline bool init_shmem_capture(struct game_capture *gc)
{
	enum gs_color_format format;

	gc->texture_buffers[0] =
		(uint8_t*)gc->data + gc->shmem_data->tex1_offset;
	gc->texture_buffers[1] =
		(uint8_t*)gc->data + gc->shmem_data->tex2_offset;

	gc->convert_16bit = is_16bit_format(gc->global_hook_info->format);
	format = gc->convert_16bit ?
		GS_BGRA : convert_format(gc->global_hook_info->format);

	obs_enter_graphics();
	gs_texture_destroy(gc->texture);
	gc->texture = gs_texture_create(gc->cx, gc->cy, format, 1, NULL,
			GS_DYNAMIC);

	gs_stagesurface_destroy(gc->screenshot.surf);
	gc->screenshot.surf = gs_stagesurface_create(gc->cx, gc->cy, GS_RGBA);
	obs_leave_graphics();

	if (!gc->texture) {
		warn("init_shmem_capture: failed to create texture");
		return false;
	}

	gc->copy_texture = copy_shmem_tex;
	return true;
}

static inline bool init_shtex_capture(struct game_capture *gc)
{
	obs_enter_graphics();
	gs_texture_destroy(gc->texture);
	gc->texture = gs_texture_open_shared(gc->shtex_data->tex_handle);

	gs_stagesurface_destroy(gc->screenshot.surf);
	if (gc->texture)
		gc->screenshot.surf = gs_stagesurface_create(
				gs_texture_get_width(gc->texture),
				gs_texture_get_height(gc->texture), GS_RGBA);
	else
		gc->screenshot.surf = NULL;
	obs_leave_graphics();

	if (!gc->texture) {
		warn("init_shtex_capture: failed to open shared handle");
		return false;
	}

	return true;
}

static bool start_capture(struct game_capture *gc)
{
	if (!init_events(gc)) {
		return false;
	}
	if (gc->global_hook_info->type == CAPTURE_TYPE_MEMORY) {
		if (!init_shmem_capture(gc)) {
			return false;
		}
	} else {
		if (!init_shtex_capture(gc)) {
			return false;
		}
	}

	calldata_set_int(&gc->start_calldata, "width", gc->global_hook_info->base_cx);
	calldata_set_int(&gc->start_calldata, "height", gc->global_hook_info->base_cy);

	signal_handler_signal(gc->signals, "start_capture", &gc->start_calldata);

	return true;
}

static inline bool target_process_died(struct game_capture *gc)
{
	if (gc->target_process || !gc->config.allow_ipc_injector)
		return object_signalled(gc->target_process);

	bool res = false;

	EnterCriticalSection(&gc->ipc_mutex);
	res = gc->monitored_process_died;
	LeaveCriticalSection(&gc->ipc_mutex);

	return res;
}

static inline bool capture_valid(struct game_capture *gc)
{
	if (!gc->dwm_capture && gc->window && !IsWindow(gc->window))
	       return false;

	if (object_signalled(gc->hook_exit))
		return false;

	return !target_process_died(gc);
}

static void send_inject_failed(struct game_capture *gc, long exit_code)
{
	calldata_set_ptr(&gc->inject_fail_calldata, "injector_exit_code", &exit_code);
	signal_handler_signal(gc->signals, "inject_failed", &gc->inject_fail_calldata);
	calldata_set_ptr(&gc->inject_fail_calldata, "injector_exit_code", NULL);
}

static DWORD WINAPI screenshot_save_thread(LPVOID param)
{
	struct game_capture *gc = param;

	obs_enter_graphics();
	gc->screenshot.saved = gs_stagesurface_save_to_file(gc->screenshot.surf, gc->screenshot.name.array);
	obs_leave_graphics();

	return 0;
}

static void handle_screenshot(struct game_capture *gc)
{
	if (gc->screenshot.staged && !gc->screenshot.saved && !gc->screenshot.save_thread)
		gc->screenshot.save_thread = CreateThread(NULL, 0, screenshot_save_thread, gc, 0, NULL);

	bool thread_ready = gc->screenshot.save_thread && WaitForSingleObject(gc->screenshot.save_thread, 0) == WAIT_OBJECT_0;

	EnterCriticalSection(&gc->screenshot.mutex);
	if (thread_ready && gc->screenshot.name.len) {
		calldata_set_int(&gc->screenshot.calldata, "screenshot_id", gc->screenshot.id);
		calldata_set_string(&gc->screenshot.calldata, "filename", gc->screenshot.name.array);

		signal_handler_signal(gc->signals, "screenshot_saved", &gc->screenshot.calldata);

		close_handle(&gc->screenshot.save_thread);

		gc->screenshot.name.len = 0;

		gc->screenshot.requested = false;
		gc->screenshot.copied = false;
		gc->screenshot.staged = false;
		gc->screenshot.saved = false;
	} else if (gc->screenshot.name.len) {
		gc->screenshot.requested = true;
	}
	LeaveCriticalSection(&gc->screenshot.mutex);

	if (gc->screenshot.copied && !gc->screenshot.staged) {
		obs_enter_graphics();
		gs_texture_t *tex = gs_texrender_get_texture(gc->screenshot.copy_tex);
		gs_stage_texture(gc->screenshot.surf, tex);
		obs_leave_graphics();
		gc->screenshot.staged = true;
	}
	
	if (!gc->screenshot.copied && gc->screenshot.requested && gc->texture) {
		obs_enter_graphics();

		if (!gc->screenshot.copy_tex)
			gc->screenshot.copy_tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

		gs_texrender_reset(gc->screenshot.copy_tex);
		if (gs_texrender_begin(gc->screenshot.copy_tex, gc->cx, gc->cy)) {
			gs_ortho(0.f, (float)gc->cx, 0.f, (float)gc->cy, -100.f, 100.f);
			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

			while (gs_effect_loop(effect, "Draw")) {
				obs_source_draw(gc->texture, 0, 0, 0, 0,
					gc->global_hook_info->flip);
			}

			gs_texrender_end(gc->screenshot.copy_tex);
			gc->screenshot.copied = true;
		}
		obs_leave_graphics();
	}
}

static void handle_injector_exit_code(struct game_capture *gc, DWORD code, const char *ipc)
{
	if (code != 0) {
		warn("%sinject process failed: %ld", ipc, (long)code);
		send_inject_failed(gc, (long)code);
	}

	if (!gc->config.anticheat_hook && code == INJECT_ERROR_VALLOC_DENIED) {
		warn("normal hook failed with ERROR_ACCESS_DENIED, retrying with anti-cheat hook");
		code = 0;
		gc->config.anticheat_hook = true;
	}

	if (code != 0 && code != INJECT_ERROR_UNLIKELY_FAIL) {
		gc->error_acquiring = true;

	} else if (!gc->capturing) {
		gc->retry_interval = ERROR_RETRY_INTERVAL;
		stop_capture(gc);
	}
}

static void game_capture_tick(void *data, float seconds)
{
	struct game_capture *gc = data;

	handle_screenshot(gc);

	if ((gc->hook_stop && object_signalled(gc->hook_stop)) ||
		target_process_died(gc)) {
		close_capture(gc);
	}

	if (gc->active && !gc->hook_ready && gc->process_id) {
		gc->hook_ready = get_event_plus_id(EVENT_HOOK_READY,
				gc->process_id);
		gc->retry_time = 0;
		gc->retry_interval = ERROR_RETRY_INTERVAL;
	}

	if (gc->hook_ready && object_signalled(gc->hook_ready)) {
		enum capture_result result = init_capture_data(gc);

		if (result == CAPTURE_SUCCESS) {
			gc->capturing = start_capture(gc);
			gc->did_capture = gc->did_capture || gc->capturing;
		}

		if (result != CAPTURE_RETRY && !gc->capturing) {
			gc->retry_interval = ERROR_RETRY_INTERVAL;
			stop_capture(gc);
		}

	} else if (gc->active && gc->hook_ready && !gc->capturing && gc->retry_time > gc->retry_interval) {
		if (gc->retries < 10) {
			close_handle(&gc->hook_ready);
			gc->active = false;

			gc->retries += 1;
		} else if (gc->retries == 10) {
			gc->retries += 1;
			warn("giving up after retrying hook_ready signal after 10 tries");
		}
	}

	if (gc->injector_process && object_signalled(gc->injector_process)) {
		DWORD exit_code = 0;

		GetExitCodeProcess(gc->injector_process, &exit_code);
		close_handle(&gc->injector_process);

		handle_injector_exit_code(gc, exit_code, "");
	}

	if (gc->config.allow_ipc_injector && gc->ipc_injector_active) {
		bool code_valid = false;
		DWORD code = 0;

		EnterCriticalSection(&gc->ipc_mutex);
		if ((code_valid = gc->have_ipc_result)) {
			gc->have_ipc_result = false;
			code = gc->ipc_result;
		}
		LeaveCriticalSection(&gc->ipc_mutex);

		if (code_valid) {
			gc->ipc_injector_active = false;

			handle_injector_exit_code(gc, code, "ipc ");
		}
	}

	gc->retry_time += seconds;

	if (!gc->active) {
		if (!gc->error_acquiring &&
			(!gc->config.allow_ipc_injector || !gc->ipc_injector_active) &&
		    gc->retry_time > gc->retry_interval) {
			if (gc->config.capture_any_fullscreen ||
			    gc->activate_hook) {
				try_hook(gc);
				gc->retry_time = 0.0f;
			}
		}
	} else {
		if (!capture_valid(gc)) {
			info("capture window no longer exists, "
			     "terminating capture");
			close_capture(gc);
		} else {
			if (gc->copy_texture) {
				obs_enter_graphics();
				gc->copy_texture(gc);
				obs_leave_graphics();
			}

			if (gc->config.cursor) {
				obs_enter_graphics();
				cursor_capture(&gc->cursor_data);
				obs_leave_graphics();
			}

			gc->fps_reset_time += seconds;
			if (gc->fps_reset_time >= gc->retry_interval) {
				reset_frame_interval(gc);
				gc->fps_reset_time = 0.0f;
			}
		}
	}
}

static inline void game_capture_render_cursor(struct game_capture *gc)
{
	POINT p = {0};

	if (!gc->global_hook_info->window ||
	    !gc->global_hook_info->base_cx ||
	    !gc->global_hook_info->base_cy)
		return;

	ClientToScreen((HWND)(uintptr_t)gc->global_hook_info->window, &p);

	float x_scale = (float)gc->global_hook_info->cx /
		(float)gc->global_hook_info->base_cx;
	float y_scale = (float)gc->global_hook_info->cy /
		(float)gc->global_hook_info->base_cy;

	cursor_draw(&gc->cursor_data, -p.x, -p.y, x_scale, y_scale,
			gc->global_hook_info->base_cx,
			gc->global_hook_info->base_cy);
}

static void game_capture_render(void *data, gs_effect_t *effect)
{
	struct game_capture *gc = data;
	if (!gc->texture)
		return;

	effect = obs_get_base_effect(gc->config.allow_transparency ?
			OBS_EFFECT_DEFAULT : OBS_EFFECT_OPAQUE);

	while (gs_effect_loop(effect, "Draw")) {
		obs_source_draw(gc->texture, 0, 0, 0, 0,
				gc->global_hook_info->flip);

		if (gc->config.allow_transparency && gc->config.cursor) {
			game_capture_render_cursor(gc);
		}
	}

	if (!gc->config.allow_transparency && gc->config.cursor) {
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

		while (gs_effect_loop(effect, "Draw")) {
			game_capture_render_cursor(gc);
		}
	}
}

static uint32_t game_capture_width(void *data)
{
	struct game_capture *gc = data;
	return gc->active ? gc->global_hook_info->cx : 0;
}

static uint32_t game_capture_height(void *data)
{
	struct game_capture *gc = data;
	return gc->active ? gc->global_hook_info->cy : 0;
}

static const char *game_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_GAME_CAPTURE;
}

static void game_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, SETTING_ANY_FULLSCREEN, true);
	obs_data_set_default_int(settings, SETTING_WINDOW_PRIORITY,
			(int)WINDOW_PRIORITY_EXE);
	obs_data_set_default_bool(settings, SETTING_COMPATIBILITY, false);
	obs_data_set_default_bool(settings, SETTING_FORCE_SCALING, false);
	obs_data_set_default_bool(settings, SETTING_CURSOR, true);
	obs_data_set_default_bool(settings, SETTING_TRANSPARENCY, false);
	obs_data_set_default_string(settings, SETTING_SCALE_RES, "0x0");
	obs_data_set_default_bool(settings, SETTING_LIMIT_FRAMERATE, false);
	obs_data_set_default_bool(settings, SETTING_CAPTURE_OVERLAYS, false);
	obs_data_set_default_bool(settings, SETTING_ANTI_CHEAT_HOOK, false);

	obs_data_set_default_string(settings, SETTING_OVERLAY_DLL, NULL);

	obs_data_set_default_bool(settings, SETTING_ALLOW_IPC_INJ, false);

	obs_data_set_default_int(settings, SETTING_PROCESS_ID, 0);
	obs_data_set_default_int(settings, SETTING_THREAD_ID, 0);
	obs_data_set_default_int(settings, SETTING_HWND, 0);
}

static bool any_fullscreen_callback(obs_properties_t *ppts,
		obs_property_t *p, obs_data_t *settings)
{
	bool any_fullscreen = obs_data_get_bool(settings,
			SETTING_ANY_FULLSCREEN);

	p = obs_properties_get(ppts, SETTING_CAPTURE_WINDOW);
	obs_property_set_enabled(p, !any_fullscreen);

	p = obs_properties_get(ppts, SETTING_WINDOW_PRIORITY);
	obs_property_set_enabled(p, !any_fullscreen);

	return true;
}

static bool use_scaling_callback(obs_properties_t *ppts, obs_property_t *p,
		obs_data_t *settings)
{
	bool use_scale = obs_data_get_bool(settings, SETTING_FORCE_SCALING);

	p = obs_properties_get(ppts, SETTING_SCALE_RES);
	obs_property_set_enabled(p, use_scale);
	return true;
}

static void insert_preserved_val(obs_property_t *p, const char *val)
{
	char *class = NULL;
	char *title = NULL;
	char *executable = NULL;
	struct dstr desc = {0};

	build_window_strings(val, &class, &title, &executable);

	dstr_printf(&desc, "[%s]: %s", executable, title);
	obs_property_list_insert_string(p, 1, desc.array, val);
	obs_property_list_item_disable(p, 1, true);

	dstr_free(&desc);
	bfree(class);
	bfree(title);
	bfree(executable);
}

static bool window_changed_callback(obs_properties_t *ppts, obs_property_t *p,
		obs_data_t *settings)
{
	const char *cur_val;
	bool match = false;
	size_t i = 0;

	cur_val = obs_data_get_string(settings, SETTING_CAPTURE_WINDOW);
	if (!cur_val) {
		return false;
	}

	for (;;) {
		const char *val = obs_property_list_item_string(p, i++);
		if (!val)
			break;

		if (strcmp(val, cur_val) == 0) {
			match = true;
			break;
		}
	}

	if (cur_val && *cur_val && !match) {
		insert_preserved_val(p, cur_val);
		return true;
	}

	UNUSED_PARAMETER(ppts);
	return false;
}

static const double default_scale_vals[] = {
	1.25,
	1.5,
	2.0,
	2.5,
	3.0
};

#define NUM_DEFAULT_SCALE_VALS \
	(sizeof(default_scale_vals) / sizeof(default_scale_vals[0]))

static BOOL CALLBACK EnumFirstMonitor(HMONITOR monitor, HDC hdc,
		LPRECT rc, LPARAM data)
{
	*(HMONITOR*)data = monitor;

	UNUSED_PARAMETER(hdc);
	UNUSED_PARAMETER(rc);
	return false;
}

static obs_properties_t *game_capture_properties(void *data)
{
	HMONITOR monitor;
	uint32_t cx = 1920;
	uint32_t cy = 1080;

	/* scaling is free form, this is mostly just to provide some common
	 * values */
	bool success = !!EnumDisplayMonitors(NULL, NULL, EnumFirstMonitor,
			(LPARAM)&monitor);
	if (success) {
		MONITORINFO mi = {0};
		mi.cbSize = sizeof(mi);

		if (!!GetMonitorInfo(monitor, &mi)) {
			cx = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
			cy = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
		}
	}

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_bool(ppts, SETTING_ANY_FULLSCREEN,
			TEXT_ANY_FULLSCREEN);

	obs_property_set_modified_callback(p, any_fullscreen_callback);

	p = obs_properties_add_list(ppts, SETTING_CAPTURE_WINDOW, TEXT_WINDOW,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "", "");
	fill_window_list(p, INCLUDE_MINIMIZED);

	obs_property_set_modified_callback(p, window_changed_callback);

	p = obs_properties_add_list(ppts, SETTING_WINDOW_PRIORITY,
			TEXT_MATCH_PRIORITY, OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_MATCH_TITLE, WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(p, TEXT_MATCH_CLASS, WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(p, TEXT_MATCH_EXE,   WINDOW_PRIORITY_EXE);

	obs_properties_add_bool(ppts, SETTING_COMPATIBILITY,
			TEXT_SLI_COMPATIBILITY);

	p = obs_properties_add_bool(ppts, SETTING_FORCE_SCALING,
			TEXT_FORCE_SCALING);

	obs_property_set_modified_callback(p, use_scaling_callback);

	p = obs_properties_add_list(ppts, SETTING_SCALE_RES, TEXT_SCALE_RES,
			OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);

	for (size_t i = 0; i < NUM_DEFAULT_SCALE_VALS; i++) {
		char scale_str[64];
		uint32_t new_cx =
			(uint32_t)((double)cx / default_scale_vals[i]) & ~2;
		uint32_t new_cy =
			(uint32_t)((double)cy / default_scale_vals[i]) & ~2;

		sprintf(scale_str, "%"PRIu32"x%"PRIu32, new_cx, new_cy);

		obs_property_list_add_string(p, scale_str, scale_str);
	}

	obs_property_set_enabled(p, false);

	obs_properties_add_bool(ppts, SETTING_TRANSPARENCY,
			TEXT_ALLOW_TRANSPARENCY);

	obs_properties_add_bool(ppts, SETTING_LIMIT_FRAMERATE,
			TEXT_LIMIT_FRAMERATE);

	obs_properties_add_bool(ppts, SETTING_CURSOR, TEXT_CAPTURE_CURSOR);

	obs_properties_add_bool(ppts, SETTING_ANTI_CHEAT_HOOK,
			TEXT_ANTI_CHEAT_HOOK);

	obs_properties_add_bool(ppts, SETTING_CAPTURE_OVERLAYS,
			TEXT_CAPTURE_OVERLAYS);

	obs_property_t *o_dll = obs_properties_add_text(ppts,
			SETTING_OVERLAY_DLL, "overlay_dll (invisible)",
			OBS_TEXT_DEFAULT);
	obs_property_set_visible(o_dll, false);
	o_dll = obs_properties_add_text(ppts,
			SETTING_OVERLAY_DLL64, "overlay_dll64 (invisible)",
			OBS_TEXT_DEFAULT);
	obs_property_set_visible(o_dll, false);

	obs_property_t *pid = obs_properties_add_int(ppts,
			SETTING_PROCESS_ID, "process_id (invisible)",
			0, ULONG_MAX, 1);
	obs_property_set_visible(pid, false);

	obs_property_t *tid = obs_properties_add_int(ppts,
			SETTING_THREAD_ID, "thread_id (invisible)",
			0, ULONG_MAX, 1);
	obs_property_set_visible(tid, false);

	UNUSED_PARAMETER(data);
	return ppts;
}


struct obs_source_info game_capture_info = {
	.id = "game_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = game_capture_name,
	.create = game_capture_create,
	.destroy = game_capture_destroy,
	.get_width = game_capture_width,
	.get_height = game_capture_height,
	.get_defaults = game_capture_defaults,
	.get_properties = game_capture_properties,
	.update = game_capture_update,
	.video_tick = game_capture_tick,
	.video_render = game_capture_render
};
