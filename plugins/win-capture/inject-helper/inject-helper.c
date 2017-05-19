#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
#include <shellapi.h>
#include <stdbool.h>
#include "../obfuscate.h"
#include "../inject-library.h"

#if defined(_MSC_VER) && !defined(inline)
#define inline __inline
#endif

static void load_debug_privilege(void)
{
	const DWORD flags = TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY;
	TOKEN_PRIVILEGES tp;
	HANDLE token;
	LUID val;

	if (!OpenProcessToken(GetCurrentProcess(), flags, &token)) {
		return;
	}

	if (!!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &val)) {
		tp.PrivilegeCount = 1;
		tp.Privileges[0].Luid = val;
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

		AdjustTokenPrivileges(token, false, &tp,
				sizeof(tp), NULL, NULL);
	}

	CloseHandle(token);
}

static inline HANDLE open_process(DWORD desired_access, bool inherit_handle,
		DWORD process_id)
{
	HANDLE (WINAPI *open_process_proc)(DWORD, BOOL, DWORD);
	open_process_proc = get_obfuscated_func(GetModuleHandleW(L"KERNEL32"),
			"HxjcQrmkb|~", 0xc82efdf78201df87);

	return open_process_proc(desired_access, inherit_handle, process_id);
}

static inline int inject_library(HANDLE process, const wchar_t *dll)
{
	return inject_library_obf(process, dll,
			"E}mo|d[cefubWk~bgk", 0x7c3371986918e8f6,
			"Rqbr`T{cnor{Bnlgwz", 0x81bf81adc9456b35,
			"]`~wrl`KeghiCt", 0xadc6a7b9acd73c9b,
			"Zh}{}agHzfd@{", 0x57135138eb08ff1c,
			"DnafGhj}l~sX", 0x350bfacdf81b2018);
}

static inline int inject_library_safe(DWORD process_id, const wchar_t *dll)
{
	return inject_library_safe_obf(process_id, dll,
			"[bs^fbkmwuKfmfOvI", 0xEAD293602FCF9778ULL);
}

static inline int inject_library_full(DWORD process_id, const wchar_t *dll)
{
	HANDLE process = open_process(PROCESS_ALL_ACCESS, false, process_id);
	int ret;

	if (!process)
		return INJECT_ERROR_OPEN_PROCESS_FAIL;

	if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
		return INJECT_ERROR_PROCESS_EXITED;

	ret = inject_library(process, dll);
	CloseHandle(process);

	return ret;
}

static int inject_helper(wchar_t *argv[], const wchar_t *dll)
{
	DWORD id;
	DWORD use_safe_inject;

	use_safe_inject = wcstol(argv[2], NULL, 10);

	id = wcstol(argv[3], NULL, 10);
	if (id == 0) {
		fprintf(stderr, "wcstol returned 0 for '%ls'", argv[3]);
		return INJECT_ERROR_INVALID_PARAMS;
	}

	return use_safe_inject
		? inject_library_safe(id, dll)
		: inject_library_full(id, dll);
}

static wchar_t *canonicalize(wchar_t *path, size_t capacity)
{
	HANDLE file = CreateFile(path, 0, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return path;

	DWORD ret = GetFinalPathNameByHandle(file, path, capacity - 1, 0);
	if (ret > 0 && ret < capacity)
		return path + 4; // remove "\\?\" part
	return path;
}

#define UNUSED_PARAMETER(x) ((void)(x))

int main(int argc, char *argv_ansi[])
{
	wchar_t dll_path[1024] = { 0 };
	LPWSTR pCommandLineW;
	LPWSTR *argv;
	int ret = INJECT_ERROR_INVALID_PARAMS;

	load_debug_privilege();

	pCommandLineW = GetCommandLineW();
	argv = CommandLineToArgvW(pCommandLineW, &argc);
	if (argv && argc == 4) {
		DWORD size = GetModuleFileNameW(NULL,
				dll_path, MAX_PATH);
		if (size) {
			wchar_t *name_start = wcsrchr(dll_path, '\\');
			if (name_start) {
				*(++name_start) = 0;
				wcscpy(name_start, argv[1]);
				wchar_t *target = canonicalize(dll_path, sizeof(dll_path) / sizeof(dll_path[0]));
				ret = inject_helper(argv, target);
			} else {
				fprintf(stderr, "wcsrchr failed: %p ('%S')\n", name_start, dll_path);
			}
		} else {
			fprintf(stderr, "GetModuleFileNameW failed: %d ('%S')\n", size, dll_path);
		}
	} else {
		fprintf(stderr, "GetCommandLineW/CommandLineToArgvW failed: %p (%d): '%S'\n", argv, argc, pCommandLineW);
		if (argv) {
			for (int i = 0; i < argc; i++)
				fprintf(stderr, "arg %d: '%S'\n", i, argv[i]);
			size_t len = wcslen(pCommandLineW);
			if (len) {
				fprintf(stderr, "command line (%d): ", len);
					for (size_t i = 0; i < len; i++)
						fprintf(stderr, "%#x", (int)pCommandLineW[i]);
				fprintf(stderr, "\n");
			}
		}
	}
	LocalFree(argv);

	UNUSED_PARAMETER(argv_ansi);
	return ret;
}
