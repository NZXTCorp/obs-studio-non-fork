#define _CRT_SECURE_NO_WARNINGS
#include <inttypes.h>
#include <stdio.h>
#include <windows.h>
#include "get-graphics-offsets.h"

void Log(const char *message, ...)
{
	va_list args;
	va_start(args, message);

	char buf[1024];
	vsprintf(buf, message, args);

	va_end(args);

	printf("; %s\n", buf);
	
	fflush(stdout);
}

int main(int argc, char *argv[])
{
	struct d3d8_offsets d3d8 = {0};
	struct d3d9_offsets d3d9 = {0};
	struct dxgi_offsets dxgi = {0};

	WNDCLASSA wc     = {0};
	wc.style         = CS_OWNDC;
	wc.hInstance     = GetModuleHandleA(NULL);
	wc.lpfnWndProc   = (WNDPROC)DefWindowProcA;
	wc.lpszClassName = DUMMY_WNDCLASS;

	if (!RegisterClassA(&wc)) {
		printf("failed to register '%s'\n", DUMMY_WNDCLASS);
		return -1;
	}

	get_d3d9_offsets(&d3d9);
	Log("---");
	get_d3d8_offsets(&d3d8);
	Log("---");
	get_dxgi_offsets(&dxgi);
	Log("---");

	Log("Done loading offsets");

#define PRINT_OFFSET(x, ...) { printf(x, ##__VA_ARGS__); fflush(stdout); }
	PRINT_OFFSET("[d3d8]\n");
	PRINT_OFFSET("present=0x%"PRIx32"\n", d3d8.present);
	PRINT_OFFSET("[d3d9]\n");
	PRINT_OFFSET("present=0x%"PRIx32"\n", d3d9.present);
	PRINT_OFFSET("present_ex=0x%"PRIx32"\n", d3d9.present_ex);
	PRINT_OFFSET("present_swap=0x%"PRIx32"\n", d3d9.present_swap);
	PRINT_OFFSET("d3d9_clsoff=0x%"PRIx32"\n", d3d9.d3d9_clsoff);
	PRINT_OFFSET("is_d3d9ex_clsoff=0x%"PRIx32"\n", d3d9.is_d3d9ex_clsoff);
	PRINT_OFFSET("[dxgi]\n");
	PRINT_OFFSET("present=0x%"PRIx32"\n", dxgi.present);
	PRINT_OFFSET("present1=0x%"PRIx32"\n", dxgi.present1);
	PRINT_OFFSET("resize=0x%"PRIx32"\n", dxgi.resize);
#undef PRINT_OFFSET

	Log("Done printing offsets");

	(void)argc;
	(void)argv;
	return 0;
}
