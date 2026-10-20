#include <windows.h>
#include <commctrl.h>
#include <mfapi.h>
#include <objbase.h>

#include "MainWindow.h"

// Held for the app's lifetime; the installer looks for it. Released early when NVLOW updates itself.
namespace nv {
HANDLE g_singleInstanceMutex = nullptr;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    // Single instance: bring the running window to the front instead of opening a second one.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\NVLOW.SingleInstance");
    nv::g_singleInstanceMutex = mutex;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"NVLOW.MainWindow", nullptr)) {
            ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(MFStartup(MF_VERSION))) {
        MessageBoxW(nullptr,
                    L"Windows Media Foundation is not available on this PC.\n\n"
                    L"On Windows \"N\" editions, install the Media Feature Pack:\n"
                    L"Settings > Apps > Optional features > Add a feature > Media Feature Pack.",
                    L"NVLOW", MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES |
                                              ICC_BAR_CLASSES | ICC_USEREX_CLASSES};
    InitCommonControlsEx(&icc);

    int exitCode = 1;
    {
        nv::MainWindow window;
        if (window.Create(instance, showCmd)) {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (!IsDialogMessageW(window.Hwnd(), &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            exitCode = (int)msg.wParam;
        }
    }

    MFShutdown();
    CoUninitialize();
    if (nv::g_singleInstanceMutex) CloseHandle(nv::g_singleInstanceMutex);
    return exitCode;
}
