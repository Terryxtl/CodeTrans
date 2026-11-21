/* main.c - wWinMain: single instance, initialisation, message loop (PLAN.md §6.8). */
#include "app.h"
#include "app_window.h"
#include "compat.h"
#include "config.h"
#include "dbglog.h"
#include <commctrl.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show)
{
    HANDLE mutex;
    INITCOMMONCONTROLSEX icc;
    BOOL start_hidden = FALSE;
    MSG m;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)prev;
    (void)cmdline;
    (void)show;

    for (int i = 1; argv && i < argc; i++)
        if (!lstrcmpiW(argv[i], L"--tray"))
            start_hidden = TRUE;
    if (argv)
        LocalFree(argv);

    /* one instance per session; a second start just shows the first one */
    mutex = CreateMutexW(NULL, TRUE, L"Local\\CodeTrans.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(CPW_MAIN_CLASS, NULL);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, RegisterWindowMessageW(L"CodeTrans.ShowWindow"), 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }

    compat_init();
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    g_app.ini_writable = config_init();
    config_load(&g_app.cfg);
    DBG("start: os %lu.%lu.%lu, ini %ls", compat_os_major(), compat_os_minor(), compat_os_build(), config_path());

    if (!app_create(hi, start_hidden)) {
        MessageBoxW(NULL, L"CodeTrans 初始化失败。", L"CodeTrans", MB_ICONERROR);
        return 1;
    }

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (app_pretranslate(&m))
            continue;
        if (g_app.hwnd && IsDialogMessageW(g_app.hwnd, &m))
            continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    app_shutdown();
    if (g_app.ini_writable)
        config_save(&g_app.cfg);
    if (mutex)
        CloseHandle(mutex);
    return 0;
}
