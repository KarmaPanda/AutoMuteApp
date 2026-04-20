#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <shellapi.h>
#include <stdlib.h>
#include "config.h"
#include "worker.h"

#define AUTOSTART_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define AUTOSTART_VALUE L"AutoMuteApp"
#define AUTOSTART_TASK_NAME L"AutoMuteApp"
#define IDI_TRAY_APP 101
#define TRAY_CLASS_NAME L"AutoMuteAppHiddenWindow"
#define WM_TRAYICON (WM_APP + 1)
#define ID_TRAY_TOGGLE_PAUSE 1001
#define ID_TRAY_EXIT 1002
#define ID_TRAY_RETRY_TIMER 1

static HANDLE g_worker_thread = NULL;
static NOTIFYICONDATAW g_nid;
static int g_tray_icon_added = 0;
static UINT g_taskbar_created_msg = 0;
static HANDLE (WINAPI *g_set_thread_dpi_awareness_context)(HANDLE) = NULL;
static HICON g_custom_tray_icon = NULL;

static int build_exe_dir_path(const wchar_t* file_name, wchar_t* out_path, size_t out_count)
{
    wchar_t exe_path[MAX_PATH];
    wchar_t* last_sep;

    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH))
        return 0;

    last_sep = wcsrchr(exe_path, L'\\');
    if (!last_sep)
        return 0;

    *(last_sep + 1) = L'\0';
    if (swprintf(out_path, out_count, L"%ls%ls", exe_path, file_name) < 0)
        return 0;

    return 1;
}

static void enable_dpi_awareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    BOOL (WINAPI *set_process_dpi_awareness_context)(HANDLE) = NULL;

    if (user32) {
        set_process_dpi_awareness_context = (BOOL (WINAPI *)(HANDLE))GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        g_set_thread_dpi_awareness_context = (HANDLE (WINAPI *)(HANDLE))GetProcAddress(user32, "SetThreadDpiAwarenessContext");
    }

    if (set_process_dpi_awareness_context) {
        if (set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
        set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        return;
    }

    SetProcessDPIAware();
}

static void allow_tray_messages(HWND hwnd)
{
    /* Explorer (medium integrity) must be allowed to send tray callbacks to elevated app windows. */
    ChangeWindowMessageFilterEx(hwnd, WM_TRAYICON, MSGFLT_ALLOW, NULL);
    if (g_taskbar_created_msg != 0) {
        ChangeWindowMessageFilterEx(hwnd, g_taskbar_created_msg, MSGFLT_ALLOW, NULL);
    }
}

static void remove_tray_icon(void)
{
    if (g_tray_icon_added && g_nid.cbSize != 0) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_tray_icon_added = 0;
    }

    if (g_custom_tray_icon) {
        DestroyIcon(g_custom_tray_icon);
        g_custom_tray_icon = NULL;
    }
}

static int create_tray_icon(HWND hwnd)
{
    wchar_t icon_path[MAX_PATH];

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_custom_tray_icon = (HICON)LoadImageW(
        GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(IDI_TRAY_APP),
        IMAGE_ICON,
        0,
        0,
        LR_DEFAULTSIZE
    );

    if (build_exe_dir_path(L"tray.ico", icon_path, MAX_PATH)) {
        if (!g_custom_tray_icon) {
            g_custom_tray_icon = (HICON)LoadImageW(
                NULL,
                icon_path,
                IMAGE_ICON,
                0,
                0,
                LR_LOADFROMFILE | LR_DEFAULTSIZE
            );
        }
    }

    g_nid.hIcon = g_custom_tray_icon ? g_custom_tray_icon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"AutoMuteApp");

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_tray_icon_added = 0;
        return 0;
    }

    g_tray_icon_added = 1;
    g_nid.uVersion = NOTIFYICON_VERSION;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

    /* Re-apply tooltip text explicitly to ensure hover text appears reliably. */
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    /* Restore full flag set for subsequent operations. */
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    return 1;
}

static void show_tray_menu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    HANDLE old_dpi_context = NULL;

    if (!menu)
        return;

    if (g_set_thread_dpi_awareness_context) {
        old_dpi_context = g_set_thread_dpi_awareness_context(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    }

    AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE_PAUSE, worker_is_paused() ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (g_set_thread_dpi_awareness_context && old_dpi_context) {
        g_set_thread_dpi_awareness_context(old_dpi_context);
    }
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UINT tray_event;

    if (msg == g_taskbar_created_msg) {
        create_tray_icon(hwnd);
        if (g_tray_icon_added) {
            KillTimer(hwnd, ID_TRAY_RETRY_TIMER);
        }
        return 0;
    }

    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_TOGGLE_PAUSE:
            worker_set_paused(!worker_is_paused());
            return 0;
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_TRAYICON:
        tray_event = g_nid.uVersion == NOTIFYICON_VERSION_4 ? LOWORD((DWORD)lParam) : (UINT)lParam;
        if (tray_event == WM_RBUTTONUP || tray_event == WM_CONTEXTMENU || tray_event == WM_LBUTTONUP || tray_event == WM_LBUTTONDBLCLK || tray_event == NIN_SELECT || tray_event == NIN_KEYSELECT) {
            show_tray_menu(hwnd);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == ID_TRAY_RETRY_TIMER) {
            if (create_tray_icon(hwnd)) {
                KillTimer(hwnd, ID_TRAY_RETRY_TIMER);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, ID_TRAY_RETRY_TIMER);
        remove_tray_icon();
        worker_request_stop();
        if (g_worker_thread) {
            WaitForSingleObject(g_worker_thread, 5000);
            CloseHandle(g_worker_thread);
            g_worker_thread = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND create_hidden_window(HINSTANCE hInst)
{
    WNDCLASSW wc;

    g_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInst;
    wc.lpszClassName = TRAY_CLASS_NAME;

    if (!RegisterClassW(&wc))
        return NULL;

    return CreateWindowExW(
        0,
        TRAY_CLASS_NAME,
        L"AutoMuteApp",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        NULL,
        NULL,
        hInst,
        NULL
    );
}

static int run_cmd_hidden(const wchar_t* cmdline)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = 1;
    wchar_t* cmd_copy = NULL;
    size_t len;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    len = wcslen(cmdline) + 1;
    cmd_copy = (wchar_t*)malloc(len * sizeof(wchar_t));
    if (!cmd_copy) return 1;

    wcscpy(cmd_copy, cmdline);

    if (!CreateProcessW(NULL, cmd_copy, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(cmd_copy);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    free(cmd_copy);
    return (int)exit_code;
}

static int is_running_as_admin(void)
{
    HANDLE token;
    TOKEN_ELEVATION elevation;
    DWORD out_size;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return 0;

    out_size = sizeof(elevation);
    if (!GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &out_size)) {
        CloseHandle(token);
        return 0;
    }

    CloseHandle(token);
    return elevation.TokenIsElevated ? 1 : 0;
}

static void delete_legacy_run_key_entry(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, AUTOSTART_VALUE);
        RegCloseKey(hKey);
    }
}

static int task_exists(void)
{
    wchar_t cmd[512];
    swprintf(cmd, 512, L"cmd.exe /C schtasks /Query /TN \"%ls\" >NUL 2>NUL", AUTOSTART_TASK_NAME);
    return run_cmd_hidden(cmd) == 0;
}

static int task_has_expected_settings(void)
{
    wchar_t cmd[2048];
    swprintf(
        cmd,
        2048,
        L"cmd.exe /C schtasks /Query /TN \"%ls\" /XML | findstr /C:\"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\" >NUL && schtasks /Query /TN \"%ls\" /XML | findstr /C:\"<MultipleInstancesPolicy>StopExisting</MultipleInstancesPolicy>\" >NUL && schtasks /Query /TN \"%ls\" /XML | findstr /C:\"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\" >NUL && schtasks /Query /TN \"%ls\" /XML | findstr /C:\"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\" >NUL",
        AUTOSTART_TASK_NAME,
        AUTOSTART_TASK_NAME,
        AUTOSTART_TASK_NAME,
        AUTOSTART_TASK_NAME,
        AUTOSTART_TASK_NAME
    );
    return run_cmd_hidden(cmd) == 0;
}

static int get_current_user_qualified(wchar_t* out_user, size_t out_count)
{
    wchar_t username[256];
    wchar_t domain[256];
    DWORD user_len = (DWORD)(sizeof(username) / sizeof(username[0]));
    DWORD domain_len;

    if (!GetUserNameW(username, &user_len))
        return 0;

    domain_len = GetEnvironmentVariableW(L"USERDOMAIN", domain, (DWORD)(sizeof(domain) / sizeof(domain[0])));
    if (domain_len > 0 && domain_len < (DWORD)(sizeof(domain) / sizeof(domain[0]))) {
        if (swprintf(out_user, out_count, L"%ls\\%ls", domain, username) < 0)
            return 0;
        return 1;
    }

    wcsncpy(out_user, username, out_count - 1);
    out_user[out_count - 1] = L'\0';
    return 1;
}

static int xml_escape(const wchar_t* src, wchar_t* dst, size_t dst_count)
{
    size_t i;
    size_t j = 0;

    for (i = 0; src[i] != L'\0'; i++) {
        const wchar_t* repl = NULL;
        size_t k;

        switch (src[i]) {
        case L'&': repl = L"&amp;"; break;
        case L'<': repl = L"&lt;"; break;
        case L'>': repl = L"&gt;"; break;
        case L'\"': repl = L"&quot;"; break;
        case L'\'': repl = L"&apos;"; break;
        default:
            if (j + 1 >= dst_count)
                return 0;
            dst[j++] = src[i];
            continue;
        }

        for (k = 0; repl[k] != L'\0'; k++) {
            if (j + 1 >= dst_count)
                return 0;
            dst[j++] = repl[k];
        }
    }

    if (j >= dst_count)
        return 0;
    dst[j] = L'\0';
    return 1;
}

static int create_or_update_task(const wchar_t* exe_path)
{
    wchar_t cmd[8192];

    swprintf(
        cmd,
        8192,
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -Command \"$ErrorActionPreference='Stop'; $u = $env:USERDOMAIN + '\\' + $env:USERNAME; $svc = New-Object -ComObject 'Schedule.Service'; $svc.Connect(); $root = $svc.GetFolder('\\'); $task = $svc.NewTask(0); $task.Principal.UserId = $u; $task.Principal.LogonType = 3; $task.Principal.RunLevel = 1; $tr = $task.Triggers.Create(9); $tr.Enabled = $true; $tr.UserId = $u; $ac = $task.Actions.Create(0); $ac.Path = '%ls'; $st = $task.Settings; $st.ExecutionTimeLimit = 'PT0S'; $st.MultipleInstances = 3; $st.DisallowStartIfOnBatteries = $false; $st.StopIfGoingOnBatteries = $false; $st.StartWhenAvailable = $true; $st.Enabled = $true; $root.RegisterTaskDefinition('%ls', $task, 6, $null, $null, 3, $null) | Out-Null\"",
        exe_path,
        AUTOSTART_TASK_NAME
    );

    return run_cmd_hidden(cmd) == 0;
}

static void delete_task(void)
{
    wchar_t cmd[512];
    swprintf(cmd, 512, L"cmd.exe /C schtasks /Delete /F /TN \"%ls\" >NUL 2>NUL", AUTOSTART_TASK_NAME);
    run_cmd_hidden(cmd);
}

static void request_elevation_for_autostart_setup(void)
{
    wchar_t exe_path[MAX_PATH];
    HINSTANCE rc;

    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH))
        return;

    rc = ShellExecuteW(NULL, L"runas", exe_path, L"--configure-autostart-only", NULL, SW_SHOWNORMAL);
    if ((INT_PTR)rc <= 32) {
        MessageBoxW(
            NULL,
            L"AutoMuteApp could not request administrator privileges to configure startup.\n"
            L"Please run AutoMuteApp.exe once as Administrator.",
            L"AutoMuteApp",
            MB_OK | MB_ICONWARNING
        );
    }
}

static int build_config_path(wchar_t* out_path, size_t out_count)
{
    wchar_t exe_path[MAX_PATH];
    wchar_t* last_sep;

    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH))
        return 0;

    last_sep = wcsrchr(exe_path, L'\\');
    if (!last_sep)
        return 0;

    *(last_sep + 1) = L'\0';
    if (swprintf(out_path, out_count, L"%lsconfig.json", exe_path) < 0)
        return 0;

    return 1;
}

static int get_current_exe_name(wchar_t* out_name, size_t out_count)
{
    wchar_t exe_path[MAX_PATH];
    wchar_t* last_sep;

    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH))
        return 0;

    last_sep = wcsrchr(exe_path, L'\\');
    if (!last_sep)
        return 0;

    wcsncpy(out_name, last_sep + 1, out_count - 1);
    out_name[out_count - 1] = L'\0';
    return 1;
}

static void close_existing_instances(void)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    DWORD self_pid;
    wchar_t current_exe[MAX_PATH];

    if (!get_current_exe_name(current_exe, MAX_PATH))
        return;

    self_pid = GetCurrentProcessId();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return;
    }

    do {
        if (pe.th32ProcessID == self_pid)
            continue;

        if (_wcsicmp(pe.szExeFile, current_exe) == 0) {
            HANDLE proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (proc) {
                TerminateProcess(proc, 0);
                WaitForSingleObject(proc, 5000);
                CloseHandle(proc);
            }
        }
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
}

static void configure_autostart(void)
{
    wchar_t exe_path[MAX_PATH];

    delete_legacy_run_key_entry();

    if (g_config.autostart) {
        if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH))
            return;

        if (!is_running_as_admin()) {
            if (!task_exists() || !task_has_expected_settings()) {
                request_elevation_for_autostart_setup();
            }
            return;
        }

        if (!create_or_update_task(exe_path)) {
            MessageBoxW(
                NULL,
                L"AutoMuteApp could not create/update the startup task.\n"
                L"Please open Task Scheduler as Administrator and verify permissions.",
                L"AutoMuteApp",
                MB_OK | MB_ICONWARNING
            );
        }
    } else {
        delete_task();
    }
}

int WINAPI WinMain(
    HINSTANCE hInst,
    HINSTANCE hPrevInst,
    LPSTR lpCmdLine,
    int nShowCmd
) {
    int i;
    int exit_code;
    int configure_only = 0;
    wchar_t config_path[MAX_PATH];
    LPWSTR* argv = NULL;
    int argc = 0;
    HWND hwnd;
    MSG msg;

    (void)hPrevInst; 
    (void)nShowCmd;

    enable_dpi_awareness();

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--configure-autostart-only") == 0) {
                configure_only = 1;
            }
        }
        LocalFree(argv);
    }

    (void)lpCmdLine;

    if (!configure_only) {
        close_existing_instances();
    }

    if (!build_config_path(config_path, MAX_PATH)) {
        load_config(L"config.json");
    } else {
        load_config(config_path);
    }

    configure_autostart();

    if (configure_only) {
        return 0;
    }

    g_worker_thread = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    if (!g_worker_thread) {
        return 1;
    }

    hwnd = create_hidden_window(hInst);
    if (!hwnd) {
        worker_request_stop();
        WaitForSingleObject(g_worker_thread, 5000);
        CloseHandle(g_worker_thread);
        g_worker_thread = NULL;
        return 1;
    }

    allow_tray_messages(hwnd);

    if (!create_tray_icon(hwnd)) {
        SetTimer(hwnd, ID_TRAY_RETRY_TIMER, 2000, NULL);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    exit_code = (int)msg.wParam;
    UnregisterClassW(TRAY_CLASS_NAME, hInst);
    return exit_code;
}
