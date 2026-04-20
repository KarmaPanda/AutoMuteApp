#include <windows.h>

#pragma once
DWORD WINAPI worker_thread(LPVOID param);
void worker_set_paused(int paused);
int worker_is_paused(void);
void worker_request_stop(void);
