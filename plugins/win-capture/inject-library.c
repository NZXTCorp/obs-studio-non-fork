#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include "obfuscate.h"
#include "inject-library.h"

typedef HANDLE (WINAPI *create_remote_thread_t)(HANDLE, LPSECURITY_ATTRIBUTES,
		SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *write_process_memory_t)(HANDLE, LPVOID, LPCVOID, SIZE_T,
		SIZE_T*);
typedef LPVOID (WINAPI *virtual_alloc_ex_t)(HANDLE, LPVOID, SIZE_T, DWORD,
		DWORD);
typedef BOOL (WINAPI *virtual_free_ex_t)(HANDLE, LPVOID, SIZE_T, DWORD);

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

	HMODULE kernel32 = GetModuleHandleW(L"KERNEL32");
	create_remote_thread_t create_remote_thread;
	write_process_memory_t write_process_memory;
	virtual_alloc_ex_t virtual_alloc_ex;
	virtual_free_ex_t virtual_free_ex;
	FARPROC load_library_w;

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

	FILETIME create_time, exit_time, kernel_time, user_time;
	FILETIME current_time;
	GetSystemTimePreciseAsFileTime(&current_time);
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
	if (mem) {
		virtual_free_ex(process, mem, 0, MEM_RELEASE);
	}
	if (last_error != 0) {
		SetLastError(last_error);
	}

	return ret;
}

/* ------------------------------------------------------------------------- */

typedef HHOOK (WINAPI *set_windows_hook_ex_t)(int, HOOKPROC, HINSTANCE, DWORD);

#define RETRY_INTERVAL_MS      500
#define TOTAL_RETRY_TIME_MS    4000
#define RETRY_COUNT            (TOTAL_RETRY_TIME_MS / RETRY_INTERVAL_MS)

int inject_library_safe_obf(DWORD thread_id, const wchar_t *dll,
		const char *set_windows_hook_ex_obf, uint64_t obf1)
{
	HMODULE user32 = GetModuleHandleW(L"USER32");
	set_windows_hook_ex_t set_windows_hook_ex;
	HMODULE lib = LoadLibraryW(dll);
	LPVOID proc;
	HHOOK hook;
	size_t i;

	if (!lib || !user32) {
		fprintf(stderr, "GetModuleHandleW/LoadLibraryW failed (USER32 -> %p, '%S' -> %p): %#x\n", user32, dll, lib, GetLastError());
		return INJECT_ERROR_LOADLIB_FAIL;
	}

#ifdef _WIN64
#define DUMMY_PROC "dummy_debug_proc"
#else
#define DUMMY_PROC "_dummy_debug_proc@12"
#endif
	proc = GetProcAddress(lib, DUMMY_PROC);

	if (!proc) {
		fprintf(stderr, "GetProcAddress " DUMMY_PROC ": %#x\n", GetLastError());
		return INJECT_ERROR_GETPROCADDR_FAIL;
	}

	set_windows_hook_ex = get_obfuscated_func(user32,
			set_windows_hook_ex_obf, obf1);

	hook = set_windows_hook_ex(WH_GETMESSAGE, proc, lib, thread_id);
	if (!hook) {
		fprintf(stderr, "set_windows_hook_ex failed: %#x\n", GetLastError());
		return INJECT_ERROR_WINHOOKEX_FAIL;
	}

	/* SetWindowsHookEx does not inject the library in to the target
	 * process unless the event associated with it has occurred, so
	 * repeatedly send the hook message to start the hook at small
	 * intervals to signal to SetWindowsHookEx to process the message and
	 * therefore inject the library in to the target process.  Repeating
	 * this is mostly just a precaution. */

	for (i = 0; i < RETRY_COUNT; i++) {
		Sleep(RETRY_INTERVAL_MS);
		PostThreadMessage(thread_id, WM_USER + 432, 0, (LPARAM)hook);
	}
	return 0;
}
