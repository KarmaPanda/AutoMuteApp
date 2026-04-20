#include "worker.h"
#include "audio.h"
#include "config.h"
#include <windows.h>

static volatile LONG g_worker_paused = 0;
static volatile LONG g_worker_stop = 0;

void worker_set_paused(int paused)
{
    InterlockedExchange(&g_worker_paused, paused ? 1 : 0);
}

int worker_is_paused(void)
{
    return InterlockedCompareExchange(&g_worker_paused, 0, 0) != 0;
}

void worker_request_stop(void)
{
    InterlockedExchange(&g_worker_stop, 1);
}

DWORD WINAPI worker_thread(LPVOID param)
{
    (void)param;
    CoInitialize(NULL);

    while (InterlockedCompareExchange(&g_worker_stop, 0, 0) == 0)
    {
        if (worker_is_paused()) {
            Sleep(200);
            continue;
        }

        scan_devices_and_mute();
        Sleep(g_config.poll_interval_ms);
    }

    CoUninitialize();
    return 0;
}
