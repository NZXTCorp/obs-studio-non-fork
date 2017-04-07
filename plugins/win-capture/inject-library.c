#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <TlHelp32.h>
#include "obfuscate.h"
#include "inject-library.h"

typedef HANDLE (WINAPI *create_remote_thread_t)(HANDLE, LPSECURITY_ATTRIBUTES,
		SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *write_process_memory_t)(HANDLE, LPVOID, LPCVOID, SIZE_T,
		SIZE_T*);
typedef LPVOID (WINAPI *virtual_alloc_ex_t)(HANDLE, LPVOID, SIZE_T, DWORD,
		DWORD);
typedef BOOL (WINAPI *virtual_free_ex_t)(HANDLE, LPVOID, SIZE_T, DWORD);
typedef VOID (WINAPI *get_system_time_as_file_time_t)(LPFILETIME);


static bool check_library_loaded(DWORD process_id, const wchar_t *dll)
{
	MODULEENTRY32 me = { sizeof(me), 0 };
	HANDLE snapshot = NULL;

	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process_id);
	if (!Module32First(snapshot, &me)) {
		fprintf(stderr, "check_library_loaded: Failed to load module snapshot\n");
		return false;
	}

	bool result = false;
	do {
		if (me.th32ProcessID != process_id)
			continue;

		if (_wcsicmp(dll, me.szExePath) != 0)
			continue;

		fprintf(stderr, "check_library_loaded: Module is already loaded");
		result = true;
		break;
	} while (Module32Next(snapshot, &me));

	CloseHandle(snapshot);

	return result;
}

/* ------------------------------------------------------------------------- */

int inject_library_obf(HANDLE process, const wchar_t *dll,
		const char *create_remote_thread_obf, uint64_t obf1,
		const char *write_process_memory_obf, uint64_t obf2,
		const char *virtual_alloc_ex_obf,     uint64_t obf3,
		const char *virtual_free_ex_obf,      uint64_t obf4,
		const char *load_library_w_obf,       uint64_t obf5)
{
	int ret = INJECT_ERROR_UNLIKELY_FAIL;
	DWORD last_error = 0;
	bool success = false;
	size_t written_size;
	DWORD thread_id;
	HANDLE thread = NULL;
	size_t size;
	void *mem;

	/* -------------------------------- */

	if (check_library_loaded(GetProcessId(process), dll))
		return 0;

	HMODULE kernel32 = GetModuleHandleW(L"KERNEL32");
	create_remote_thread_t create_remote_thread;
	write_process_memory_t write_process_memory;
	virtual_alloc_ex_t virtual_alloc_ex;
	virtual_free_ex_t virtual_free_ex;
	FARPROC load_library_w;
	get_system_time_as_file_time_t get_system_time = NULL;

	create_remote_thread = get_obfuscated_func(kernel32,
			create_remote_thread_obf, obf1);
	write_process_memory = get_obfuscated_func(kernel32,
			write_process_memory_obf, obf2);
	virtual_alloc_ex = get_obfuscated_func(kernel32,
			virtual_alloc_ex_obf, obf3);
	virtual_free_ex = get_obfuscated_func(kernel32,
			virtual_free_ex_obf, obf4);
	load_library_w = get_obfuscated_func(kernel32,
			load_library_w_obf, obf5);

	/* -------------------------------- */

	get_system_time = (get_system_time_as_file_time_t)GetProcAddress(kernel32, "GetSystemTimePreciseAsFileTime");
	if (!get_system_time)
		get_system_time = GetSystemTimeAsFileTime;

	/* -------------------------------- */

	FILETIME create_time, exit_time, kernel_time, user_time;
	FILETIME current_time;
	get_system_time(&current_time);
	if (GetProcessTimes(process, &create_time, &exit_time, &kernel_time, &user_time)) {
		LONGLONG diff = ((LARGE_INTEGER*)&current_time)->QuadPart - ((LARGE_INTEGER*)&create_time)->QuadPart;
		fprintf(stderr, "process has been alive for %g ms\n", diff / 10000.);
	}

	SetLastError(0);

	size = (wcslen(dll) + 1) * sizeof(wchar_t);
	mem = virtual_alloc_ex(process, NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!mem) {
		fprintf(stderr, "virtual_alloc_ex failed (tried with %llu bytes): %#x\n", (unsigned long long)size, GetLastError());
		ret = INJECT_ERROR_VALLOC_FAIL;
		goto fail;
	}

	success = write_process_memory(process, mem, dll, // check for dependencies first? e.g. user32, ...
			size, &written_size);
	if (!success) {
		fprintf(stderr, "write_process_memory failed (dll: '%S', size: %llu, written_size: %llu): %#x\n",
				dll, (unsigned long long)size, (unsigned long long)written_size, GetLastError());
		ret = INJECT_ERROR_WPROCMEM_FAIL;
		goto fail;
	}

	thread = create_remote_thread(process, NULL, 0,
			(LPTHREAD_START_ROUTINE)load_library_w, mem, 0,
			&thread_id);
	if (!thread) {
		fprintf(stderr, "create_remote_thread failed: %#x\n", GetLastError());
		ret = INJECT_ERROR_CREMOTETHREAD_FAIL;
		goto fail;
	}

	if (WaitForSingleObject(thread, 4000) == WAIT_OBJECT_0) {
		DWORD code;
		GetExitCodeThread(thread, &code);
		ret = (code != 0) ? 0 : INJECT_ERROR_INJECT_FAILED;

		SetLastError(0);
	}

fail:
	if (ret == INJECT_ERROR_UNLIKELY_FAIL) {
		last_error = GetLastError();
	}
	if (thread) {
		CloseHandle(thread);
	}
	if (mem && (thread && ret != INJECT_ERROR_UNLIKELY_FAIL)) {
		virtual_free_ex(process, mem, 0, MEM_RELEASE);
	}
	if (last_error != 0) {
		SetLastError(last_error);
	}

	return ret;
}

/* ------------------------------------------------------------------------- */

typedef HHOOK(WINAPI *set_windows_hook_ex_t)(int, HOOKPROC, HINSTANCE, DWORD);

#define MAX_THREADS 20

struct safe_inject_data
{
	set_windows_hook_ex_t set_windows_hook_ex;
	HMODULE lib;
	LPVOID proc;
	size_t num_threads;
	DWORD thread_id[MAX_THREADS];
	HHOOK hook[MAX_THREADS];
};

static BOOL __stdcall enum_thread_windows(HWND hWnd, LPARAM lParam)
{
	return TRUE;
}

static bool try_inject_thread_safe(DWORD thread_id, struct safe_inject_data *inject_data)
{
	if (!EnumThreadWindows(thread_id, enum_thread_windows, 0))
		return false;

	if (inject_data->num_threads >= MAX_THREADS)
		return false;

	inject_data->hook[inject_data->num_threads] = inject_data->set_windows_hook_ex(WH_GETMESSAGE, inject_data->proc, inject_data->lib, thread_id);
	if (!inject_data->hook) {
		fprintf(stderr, "set_windows_hook_ex failed for thread id %#x: %#x\n", thread_id, GetLastError());
		return false;
	}

	fprintf(stderr, "try_inject_thread_safe: added thread id %#x\n", thread_id);

	inject_data->thread_id[inject_data->num_threads] = thread_id;
	inject_data->num_threads += 1;
	return true;
}

static bool try_inject_process_safe(DWORD process_id, struct safe_inject_data *inject_data)
{
	THREADENTRY32 te = { sizeof(te), 0 };
	HANDLE snapshot = NULL;

	snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, process_id);
	if (!Thread32First(snapshot, &te)) {
		fprintf(stderr, "try_inject_process_safe: failed to get threads snapshot: %#x\n", GetLastError());
		return false;
	}

	do
	{
		if (te.th32OwnerProcessID != process_id)
			continue;

		try_inject_thread_safe(te.th32ThreadID, inject_data);
	} while (Thread32Next(snapshot, &te));

	CloseHandle(snapshot);

	return inject_data->num_threads > 0;
}

#define RETRY_INTERVAL_MS      500
#define TOTAL_RETRY_TIME_MS    4000
#define RETRY_COUNT            (TOTAL_RETRY_TIME_MS / RETRY_INTERVAL_MS)

int inject_library_safe_obf(DWORD process_id, const wchar_t *dll,
		const char *set_windows_hook_ex_obf, uint64_t obf1)
{
	HMODULE user32 = GetModuleHandleW(L"USER32");
	struct safe_inject_data inject_data = { 0 };
	inject_data.lib = LoadLibraryW(dll);
	size_t i, j = 0, k;
	size_t thread_messages_posted = 0;

	if (check_library_loaded(process_id, dll))
		return 0;

	if (!inject_data.lib || !user32) {
		fprintf(stderr, "GetModuleHandleW/LoadLibraryW failed (USER32 -> %p, '%S' -> %p): %#x\n", user32, dll, inject_data.lib, GetLastError());
		return INJECT_ERROR_LOADLIB_FAIL;
	}

#ifdef _WIN64
#define DUMMY_PROC "dummy_debug_proc"
#else
#define DUMMY_PROC "_dummy_debug_proc@12"
#endif
	inject_data.proc = GetProcAddress(inject_data.lib, DUMMY_PROC);

	if (!inject_data.proc) {
		fprintf(stderr, "GetProcAddress " DUMMY_PROC ": %#x\n", GetLastError());
		return INJECT_ERROR_GETPROCADDR_FAIL;
	}

	inject_data.set_windows_hook_ex = get_obfuscated_func(user32,
			set_windows_hook_ex_obf, obf1);

try_inject_process:
	if (!try_inject_process_safe(process_id, &inject_data)) {
		fprintf(stderr, "try_inject_process_safe failed\n");
		return INJECT_ERROR_INJECTPROC_FAIL;
	}

	/* SetWindowsHookEx does not inject the library in to the target
	 * process unless the event associated with it has occurred, so
	 * repeatedly send the hook message to start the hook at small
	 * intervals to signal to SetWindowsHookEx to process the message and
	 * therefore inject the library in to the target process.  Repeating
	 * this is mostly just a precaution. */

	for (i = 0; i < RETRY_COUNT; i++) {
		Sleep(RETRY_INTERVAL_MS);
		for (k = 0; k < inject_data.num_threads;) {
			if (PostThreadMessage(inject_data.thread_id[k], WM_USER + 432, 0, (LPARAM)inject_data.hook[k])) {
				k++;
				thread_messages_posted += 1;
				continue;
			}

			DWORD err = GetLastError();

			if (err != ERROR_INVALID_THREAD_ID && err != ERROR_NOT_ENOUGH_QUOTA) {
				fprintf(stderr, "PostThreadMessage(%#x) failed: %#x\n", inject_data.thread_id[k], err);
				return INJECT_ERROR_POSTTHREAD_FAIL;
			}

			if (inject_data.num_threads > 1) {
				fprintf(stderr, "Removing thread %#x (%#x)\n", inject_data.thread_id[k], err);
				inject_data.num_threads -= 1;
				inject_data.thread_id[k] = inject_data.thread_id[inject_data.num_threads];
				inject_data.hook[k] = inject_data.hook[inject_data.num_threads];
				continue;
			}

			if (j++ >= RETRY_COUNT)
				return thread_messages_posted < 5 ? INJECT_ERROR_RETRIES_EXHAUSTED : 0;

			fprintf(stderr, "Retrying safe hook due to all threads becoming invalid\n");

			inject_data.num_threads = 0;
			goto try_inject_process;
		}
	}
	return 0;
}
