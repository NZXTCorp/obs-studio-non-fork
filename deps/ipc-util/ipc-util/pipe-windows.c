/*
 * Copyright (c) 2014 Hugh Bailey <obs.jim@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "pipe.h"

#include <sddl.h>

#define IPC_PIPE_BUF_SIZE 1024

static inline bool ipc_pipe_internal_create_events(ipc_pipe_server_t *pipe)
{
	pipe->ready_event = CreateEvent(NULL, false, false, NULL);
	return !!pipe->ready_event;
}

static inline void *create_full_access_security_descriptor()
{
	void *sd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
		"D:(A;OICI;GA;;;AU)S:(ML;;;;;LW)", SDDL_REVISION_1, &sd, NULL))
		return NULL;

	return sd;
}

static inline bool ipc_pipe_internal_create_pipe(ipc_pipe_server_t *pipe,
		const char *name)
{
	SECURITY_ATTRIBUTES sa;
	char new_name[512];
	void *sd;
	const DWORD access = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
	const DWORD flags = PIPE_TYPE_MESSAGE     |
	                    PIPE_READMODE_MESSAGE |
	                    PIPE_WAIT;

	strcpy_s(new_name, sizeof(new_name), "\\\\.\\pipe\\");
	strcat_s(new_name, sizeof(new_name), name);

	sd = create_full_access_security_descriptor();
	if (!sd) {
		return false;
	}

	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = sd;
	sa.bInheritHandle = false;

	pipe->handle = CreateNamedPipeA(new_name, access, flags, 1,
			IPC_PIPE_BUF_SIZE, IPC_PIPE_BUF_SIZE, 0, &sa);
	LocalFree(sd);
	
	return pipe->handle != INVALID_HANDLE_VALUE;
}

static inline bool ipc_pipe_internal_io_pending(void)
{
	return GetLastError() == ERROR_IO_PENDING;
}

static DWORD CALLBACK ipc_pipe_internal_server_thread(LPVOID param)
{
	ipc_pipe_server_t *pipe = param;

	size_t  size = 0;
	size_t  capacity = pipe->overlapped_size > 0 ?
		pipe->overlapped_size : IPC_PIPE_BUF_SIZE;
	uint8_t *read_data = malloc(capacity);

	/* wait for connection */
	DWORD wait = WaitForSingleObject(pipe->ready_event, INFINITE);
	if (wait != WAIT_OBJECT_0) {
		pipe->read_callback(pipe->param, NULL, 0);
		return 0;
	}

	for (;;) {
		DWORD bytes = 0;
		bool success;

		success = !!ReadFile(pipe->handle, read_data + size,
				(DWORD)(capacity - size), NULL, &pipe->overlap);
		if (!success && !ipc_pipe_internal_io_pending()) {
			DWORD res = GetLastError();
			if (res != ERROR_MORE_DATA)
				break;
		}

		DWORD wait = WaitForSingleObject(pipe->ready_event, INFINITE);
		if (wait != WAIT_OBJECT_0) {
			break;
		}

		success = !!GetOverlappedResult(pipe->handle, &pipe->overlap,
				&bytes, true);
		if (!success || !bytes) {
			DWORD res = GetLastError();
			if (res != ERROR_MORE_DATA)
				break;

			capacity *= 2;
			uint8_t *new_data = realloc(read_data, capacity);
			if (!new_data)
				break;
			read_data = new_data;
		}

		size += bytes;

		if (success) {
			pipe->read_callback(pipe->param, read_data, size);
			size = 0;
		}
	}

	free(read_data);
	pipe->read_callback(pipe->param, NULL, 0);
	return 0;
}

static inline bool ipc_pipe_internal_start_server_thread(
		ipc_pipe_server_t *pipe)
{
	pipe->thread = CreateThread(NULL, 0, ipc_pipe_internal_server_thread,
			pipe, 0, NULL);
	return pipe->thread != NULL;
}

static inline bool ipc_pipe_internal_wait_for_connection(
		ipc_pipe_server_t *pipe)
{
	bool success;

	pipe->overlap.hEvent = pipe->ready_event;
	success = !!ConnectNamedPipe(pipe->handle, &pipe->overlap);
	return success || (!success && ipc_pipe_internal_io_pending())
		           || (!success && GetLastError() == ERROR_PIPE_CONNECTED);
}

static inline bool ipc_pipe_internal_open_pipe(ipc_pipe_client_t *pipe,
		const char *name)
{
	DWORD mode = PIPE_READMODE_MESSAGE;
	char new_name[512];

	strcpy_s(new_name, sizeof(new_name), "\\\\.\\pipe\\");
	strcat_s(new_name, sizeof(new_name), name);

	pipe->handle = CreateFileA(new_name, GENERIC_READ | GENERIC_WRITE,
			0, NULL, OPEN_EXISTING, 0, NULL);
	if (pipe->handle == INVALID_HANDLE_VALUE) {
		return false;
	}

	ULONG pid;
	if (!GetNamedPipeServerProcessId(pipe->handle, &pid))
		return false;

	pipe->server_process = OpenProcess(SYNCHRONIZE, false, pid);
	if (pipe->server_process == INVALID_HANDLE_VALUE)
		return false;

	return !!SetNamedPipeHandleState(pipe->handle, &mode, NULL, NULL);
}

/* ------------------------------------------------------------------------- */

bool ipc_pipe_server_start_buf(ipc_pipe_server_t *pipe, const char *name,
		ipc_pipe_read_t read_callback, void *param, int buffer)
{
	pipe->read_callback = read_callback;
	pipe->param = param;
	pipe->overlapped_size = buffer;

	if (!ipc_pipe_internal_create_events(pipe)) {
		goto error;
	}
	if (!ipc_pipe_internal_create_pipe(pipe, name)) {
		goto error;
	}
	if (!ipc_pipe_internal_wait_for_connection(pipe)) {
		goto error;
	}
	if (!ipc_pipe_internal_start_server_thread(pipe)) {
		goto error;
	}

	return true;

error:
	ipc_pipe_server_free(pipe);
	return false;
}

bool ipc_pipe_server_start(ipc_pipe_server_t *pipe, const char *name,
		ipc_pipe_read_t read_callback, void *param)
{
	return ipc_pipe_server_start_buf(pipe, name, read_callback, param,
			-1);
}

void ipc_pipe_server_free(ipc_pipe_server_t *pipe)
{
	if (!pipe)
		return;

	if (pipe->thread) {
		CancelIoEx(pipe->handle, &pipe->overlap);
		SetEvent(pipe->ready_event);
		WaitForSingleObject(pipe->thread, INFINITE);
		CloseHandle(pipe->thread);
	}
	if (pipe->ready_event)
		CloseHandle(pipe->ready_event);
	if (pipe->handle)
		CloseHandle(pipe->handle);

	memset(pipe, 0, sizeof(*pipe));
}

bool ipc_pipe_client_open(ipc_pipe_client_t *pipe, const char *name)
{
	if (!ipc_pipe_internal_open_pipe(pipe, name)) {
		ipc_pipe_client_free(pipe);
		return false;
	}

	return true;
}

void ipc_pipe_client_free(ipc_pipe_client_t *pipe)
{
	if (!pipe)
		return;

	if (pipe->handle)
		CloseHandle(pipe->handle);

	memset(pipe, 0, sizeof(*pipe));
}

bool ipc_pipe_client_write(ipc_pipe_client_t *pipe, const void *data,
		size_t size)
{
	DWORD bytes;

	if (!pipe) {
		return false;
	}

	if (!pipe->handle || pipe->handle == INVALID_HANDLE_VALUE) {
		return false;
	}

	return !!WriteFile(pipe->handle, data, (DWORD)size, &bytes, NULL);
}
