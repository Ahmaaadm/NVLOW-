#include "MainWindow.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>

#include "../res/resource.h"
#include "Utils.h"
#include "VideoScanner.h"

namespace nv {

namespace {

constexpr UINT WM_APP_PROBED = WM_APP + 1;      // wParam: id, lParam: MediaInfo*
constexpr UINT WM_APP_CAPS = WM_APP + 2;        // lParam: EncoderCaps*
constexpr UINT WM_APP_JOB_START = WM_APP + 3;   // wParam: id
constexpr UINT WM_APP_JOB_PROGRESS = WM_APP + 4; // wParam: id, lParam: EncodeProgress*
constexpr UINT WM_APP_JOB_DONE = WM_APP + 5;    // wParam: id, lParam: JobOutcome*
constexpr UINT WM_APP_QUEUE_DONE = WM_APP + 6;
constexpr UINT WM_APP_UPDATE_INFO = WM_APP + 7;      // lParam: UpdateInfo*
constexpr UINT WM_APP_UPDATE_PROGRESS = WM_APP + 8;  // wParam: 0..1000
constexpr UINT WM_APP_UPDATE_DONE = WM_APP + 9;      // lParam: std::pair<path, error>*

// System-menu commands (must be multiples of 16 below 0xF000).
constexpr UINT IDM_SYS_UPDATE = 0x0010;
constexpr UINT IDM_SYS_ABOUT = 0x0020;

constexpr COLORREF kUpdateBg = RGB(234, 245, 214);

enum ControlId : int {
    IDC_GAME = 1001, IDC_CHECKALL, IDC_REMOVE, IDC_ADD, IDC_LIBRARY, IDC_REFRESH, IDC_LIST,
    IDC_CODEC, IDC_QUALITY, IDC_RES, IDC_AUDIO, IDC_PERF, IDC_OUTPUT, IDC_BROWSE, IDC_NEXTTO, IDC_RECYCLE,
    IDC_START, IDC_CANCEL, IDC_UPD_GO, IDC_UPD_NOTES, IDC_UPD_SKIP,
    IDM_OPEN = 2001, IDM_REVEAL, IDM_OPEN_OUTPUT, IDM_REVEAL_OUTPUT, IDM_CHECK_ALL, IDM_UNCHECK_ALL, IDM_REMOVE,
};

enum Column { ColName, ColGame, ColDate, ColDuration, ColVideo, ColBitrate, ColSize, ColResult, ColCount };

struct ColumnDef {
    const wchar_t* title;
    int width;
    int format;
};
const ColumnDef kColumns[ColCount] = {
    {L"File", 290, LVCFMT_LEFT},     {L"Game", 130, LVCFMT_LEFT},     {L"Recorded", 125, LVCFMT_LEFT},
    {L"Length", 58, LVCFMT_RIGHT},   {L"Video", 175, LVCFMT_LEFT},    {L"Bitrate", 80, LVCFMT_RIGHT},
    {L"Size", 80, LVCFMT_RIGHT},     {L"Result", 220, LVCFMT_LEFT},
};

const wchar_t* kCodecLabels[] = {
    L"AV1 — GPU  · smallest files",
    L"HEVC (H.265) — GPU",
    L"H.264 — GPU  · plays everywhere",
    L"HEVC (H.265) — CPU  · slow",
    L"H.264 — CPU  · slow",
};
const wchar_t* kQualityLabels[] = {
    L"Visually lossless  (largest file)",
    L"High quality  · recommended",
    L"Balanced",
    L"Smallest file",
};
const wchar_t* kResLabels[] = {L"Keep original", L"1440p", L"1080p", L"720p"};
const wchar_t* kAudioLabels[] = {L"Keep original  (no re-encode)", L"AAC 192 kbps", L"AAC 128 kbps", L"Remove audio"};
const wchar_t* kPerfLabels[] = {
    L"Low  · quiet & cool, PC stays smooth",
    L"Medium  · balanced (recommended)",
    L"High  · fastest, full GPU power",
    L"Max quality  · slowest, best detail",
};

constexpr COLORREF kAccent = RGB(118, 185, 0);  // NVIDIA green
constexpr COLORREF kBannerBg = RGB(22, 23, 25);
constexpr COLORREF kBannerText = RGB(236, 236, 236);
constexpr COLORREF kBannerMuted = RGB(150, 154, 160);
constexpr COLORREF kOkText = RGB(40, 120, 20);
constexpr COLORREF kFailText = RGB(196, 30, 30);
constexpr COLORREF kBusyText = RGB(0, 100, 190);

const wchar_t kClassName[] = L"NVLOW.MainWindow";
const wchar_t kImportedGame[] = L"Imported";

template <class T>
void PostOwned(HWND hwnd, UINT msg, WPARAM wp, T* ptr) {
    if (!PostMessageW(hwnd, msg, wp, (LPARAM)ptr)) delete ptr;
}

int StateRank(ItemState s) {
    switch (s) {
        case ItemState::Encoding: return 0;
        case ItemState::Queued: return 1;
        case ItemState::Failed: return 2;
        case ItemState::Cancelled: return 3;
        case ItemState::Analyzing: return 4;
        case ItemState::Ready: return 5;
        case ItemState::NoGain: return 6;
        case ItemState::Done: return 7;
    }
    return 8;
}

std::wstring Percent(uint64_t in, uint64_t out) {
    if (in == 0) return {};
    double saved = 100.0 * (1.0 - (double)out / (double)in);
    wchar_t buf[32];
    swprintf(buf, 32, L"−%.0f%%", saved);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    if (m_taskbar) m_taskbar->Release();
    for (HFONT f : {m_font, m_fontBold, m_fontTitle, m_fontSubtitle})
        if (f) DeleteObject(f);
}

bool MainWindow::Create(HINSTANCE instance, int showCmd) {
    m_instance = instance;
    m_settings = LoadSettings();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    UINT dpi = GetDpiForSystem();
    int w = MulDiv(m_settings.windowWidth, dpi, 96);
    int h = MulDiv(m_settings.windowHeight, dpi, 96);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    w = std::min(w, (int)(work.right - work.left));
    h = std::min(h, (int)(work.bottom - work.top));
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    m_hwnd = CreateWindowExW(0, kClassName, L"NVLOW — NVIDIA Recording Compressor",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, instance, this);
    if (!m_hwnd) return false;

    ShowWindow(m_hwnd, m_settings.maximized ? SW_SHOWMAXIMIZED : showCmd);
    UpdateWindow(m_hwnd);
    return true;
}

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (m_taskbarCreatedMsg && msg == m_taskbarCreatedMsg) {
        if (!m_taskbar &&
            SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_taskbar))))
            m_taskbar->HrInit();
        return 0;
    }

    switch (msg) {
        case WM_CREATE:
            OnCreate();
            return 0;

        case WM_SIZE:
            Layout();
            {
                RECT banner{0, 0, LOWORD(lp), BannerHeight()};
                InvalidateRect(m_hwnd, &banner, FALSE);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = {S(960), S(640)};
            return 0;
        }

        case WM_DPICHANGED: {
            UINT oldDpi = m_dpi;
            m_dpi = HIWORD(wp);
            CreateFonts();
            ApplyFonts();
            for (int c = 0; c < ColCount; ++c)
                ListView_SetColumnWidth(m_list, c, MulDiv(ListView_GetColumnWidth(m_list, c), m_dpi, oldDpi));
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(m_hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            rc.top = BannerHeight();
            if (m_updateBarVisible) {
                RECT bar = rc;
                bar.bottom = rc.top + UpdateBarHeight();
                FillRect((HDC)wp, &bar, m_updateBrush);
                rc.top = bar.bottom;
            }
            FillRect((HDC)wp, &rc, GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }

        case WM_PAINT:
            Paint();
            return 0;

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            if ((HWND)lp == m_lblUpdate) {
                SetBkColor(dc, kUpdateBg);
                SetTextColor(dc, RGB(30, 60, 0));
                return (LRESULT)m_updateBrush;
            }
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return 0;

        case WM_NOTIFY:
            return OnNotify(reinterpret_cast<NMHDR*>(lp));

        case WM_CONTEXTMENU:
            if ((HWND)wp == m_list) {
                OnContextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                return 0;
            }
            break;

        case WM_DROPFILES:
            OnDropFiles((HDROP)wp);
            return 0;

        case WM_APP_PROBED: {
            std::unique_ptr<MediaInfo> info(reinterpret_cast<MediaInfo*>(lp));
            auto it = m_items.find((int)wp);
            if (it != m_items.end()) {
                it->second.info = *info;
                if (it->second.state == ItemState::Analyzing) {
                    it->second.state = ItemState::Ready;
                    if (!info->ok) it->second.result = L"Could not read video info";
                }
                RefreshRow(it->first);
            }
            return 0;
        }

        case WM_APP_CAPS: {
            std::unique_ptr<EncoderCaps> caps(reinterpret_cast<EncoderCaps*>(lp));
            OnEncoderCaps(*caps);
            return 0;
        }

        case WM_APP_JOB_START:
            OnJobStart((int)wp);
            return 0;

        case WM_APP_JOB_PROGRESS: {
            std::unique_ptr<EncodeProgress> p(reinterpret_cast<EncodeProgress*>(lp));
            OnJobProgress((int)wp, *p);
            return 0;
        }

        case WM_APP_JOB_DONE: {
            std::unique_ptr<JobOutcome> o(reinterpret_cast<JobOutcome*>(lp));
            OnJobDone((int)wp, *o);
            return 0;
        }

        case WM_APP_QUEUE_DONE:
            OnQueueDone();
            return 0;

        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == IDM_SYS_UPDATE) {
                StartUpdateCheck(true);
                return 0;
            }
            if ((wp & 0xFFF0) == IDM_SYS_ABOUT) {
                ShowAbout();
                return 0;
            }
            break;

        case WM_APP_UPDATE_INFO: {
            std::unique_ptr<UpdateInfo> info(reinterpret_cast<UpdateInfo*>(lp));
            OnUpdateInfo(*info);
            return 0;
        }

        case WM_APP_UPDATE_PROGRESS: {
            wchar_t buf[96];
            swprintf(buf, 96, L"Downloading NVLOW %ls…  %u%%", m_update.version.c_str(), (unsigned)(wp / 10));
            SetWindowTextW(m_lblUpdate, buf);
            return 0;
        }

        case WM_APP_UPDATE_DONE: {
            std::unique_ptr<std::pair<std::wstring, std::wstring>> r(
                reinterpret_cast<std::pair<std::wstring, std::wstring>*>(lp));
            OnUpdateDownloaded(r->first, r->second);
            return 0;
        }

        case WM_CLOSE:
            if (OnClose()) DestroyWindow(m_hwnd);
            return 0;

        case WM_DESTROY:
            OnDestroy();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

void MainWindow::OnCreate() {
    m_dpi = GetDpiForWindow(m_hwnd);
    m_av1Playable = IsStorePackageInstalled(L"Microsoft.AV1VideoExtension");
    m_hevcPlayable = IsStorePackageInstalled(L"Microsoft.HEVCVideoExtension") ||
                     IsStorePackageInstalled(L"Microsoft.HEVCVideoExtensions");
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");
    ChangeWindowMessageFilterEx(m_hwnd, m_taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);

    CreateFonts();
    CreateControls();
    ApplyFonts();
    LoadSettingsIntoControls();
    DragAcceptFiles(m_hwnd, TRUE);

    m_updateBrush = CreateSolidBrush(kUpdateBg);
    if (HMENU sys = GetSystemMenu(m_hwnd, FALSE)) {
        AppendMenuW(sys, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sys, MF_STRING, IDM_SYS_UPDATE, L"Check for updates…");
        AppendMenuW(sys, MF_STRING, IDM_SYS_ABOUT, L"About NVLOW…");
    }

    HWND hwnd = m_hwnd;
    m_probe.Start([hwnd](int id, const MediaInfo& info) {
        PostOwned(hwnd, WM_APP_PROBED, (WPARAM)id, new MediaInfo(info));
    });

    m_libraryRoot = m_settings.libraryRoot.empty() || !DirExists(m_settings.libraryRoot) ? DetectNvidiaFolder()
                                                                                        : m_settings.libraryRoot;
    m_sortColumn = m_settings.sortColumn;
    m_sortAscending = m_settings.sortAscending;
    ScanLibrary();
    DetectEncoders();
    StartUpdateCheck(false);
    SetBusy(false);
    Layout();
}

bool MainWindow::OnClose() {
    if (m_busy) {
        int r = MessageBoxW(m_hwnd, L"Compression is still running. Stop it and exit?", L"NVLOW",
                            MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
        if (r != IDYES) return false;
        CancelCompression();
    }
    if (m_worker.joinable()) m_worker.join();

    ReadSettingsFromControls();
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (GetWindowPlacement(m_hwnd, &wp)) {
        RECT r = wp.rcNormalPosition;
        m_settings.windowWidth = MulDiv(r.right - r.left, 96, m_dpi);
        m_settings.windowHeight = MulDiv(r.bottom - r.top, 96, m_dpi);
        m_settings.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
    m_settings.libraryRoot = m_libraryRoot;
    SaveSettings(m_settings);
    return true;
}

void MainWindow::OnDestroy() {
    m_updateCancel = true;
    if (m_updateThread.joinable()) m_updateThread.join();
    if (m_updateBrush) DeleteObject(m_updateBrush);
    m_probe.Stop();
    if (m_capsThread.joinable()) m_capsThread.join();
    if (m_worker.joinable()) m_worker.join();
}

// ---------------------------------------------------------------------------------------------
// Controls & layout
// ---------------------------------------------------------------------------------------------

HWND MainWindow::Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, m_hwnd,
                           (HMENU)(INT_PTR)id, m_instance, nullptr);
}

void MainWindow::CreateFonts() {
    for (HFONT* f : {&m_font, &m_fontBold, &m_fontTitle, &m_fontSubtitle}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
    auto make = [&](int pt, int weight, const wchar_t* face) {
        return CreateFontW(-MulDiv(pt, m_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    m_font = make(9, FW_NORMAL, L"Segoe UI");
    m_fontBold = make(9, FW_SEMIBOLD, L"Segoe UI");
    m_fontTitle = make(20, FW_BOLD, L"Segoe UI");
    m_fontSubtitle = make(10, FW_NORMAL, L"Segoe UI");
}

void MainWindow::ApplyFonts() {
    EnumChildWindows(
        m_hwnd,
        [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
            return TRUE;
        },
        (LPARAM)m_font);
    SendMessageW(m_btnStart, WM_SETFONT, (WPARAM)m_fontBold, TRUE);
    SendMessageW(m_grpSettings, WM_SETFONT, (WPARAM)m_fontBold, TRUE);
}

void MainWindow::CreateControls() {
    const DWORD btn = BS_PUSHBUTTON | WS_TABSTOP;
    const DWORD combo = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;

    // Settings group first so it sits on top of the z-order and clips the controls inside it.
    m_grpSettings = Ctl(L"BUTTON", L"Compression settings", BS_GROUPBOX | WS_CLIPSIBLINGS, 0);

    m_lblGame = Ctl(L"STATIC", L"Game:", SS_LEFT, 0);
    m_cmbGame = Ctl(WC_COMBOBOXW, L"", combo, IDC_GAME);
    m_btnCheckAll = Ctl(L"BUTTON", L"Select all", btn, IDC_CHECKALL);
    m_btnRemove = Ctl(L"BUTTON", L"Remove", btn, IDC_REMOVE);
    m_btnAdd = Ctl(L"BUTTON", L"Add videos…", btn, IDC_ADD);
    m_btnLibrary = Ctl(L"BUTTON", L"Change folder…", btn, IDC_LIBRARY);
    m_btnRefresh = Ctl(L"BUTTON", L"Refresh", btn, IDC_REFRESH);

    m_list = Ctl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER, IDC_LIST);
    SetWindowTheme(m_list, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(m_list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER |
                                                  LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
    for (int c = 0; c < ColCount; ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(kColumns[c].title);
        col.cx = S(kColumns[c].width);
        col.fmt = kColumns[c].format;
        col.iSubItem = c;
        ListView_InsertColumn(m_list, c, &col);
    }

    m_lblCodec = Ctl(L"STATIC", L"Encoder", SS_LEFT, 0);
    m_cmbCodec = Ctl(WC_COMBOBOXW, L"", combo, IDC_CODEC);
    m_lblQuality = Ctl(L"STATIC", L"Quality", SS_LEFT, 0);
    m_cmbQuality = Ctl(WC_COMBOBOXW, L"", combo, IDC_QUALITY);
    m_lblRes = Ctl(L"STATIC", L"Resolution", SS_LEFT, 0);
    m_cmbRes = Ctl(WC_COMBOBOXW, L"", combo, IDC_RES);
    m_lblAudio = Ctl(L"STATIC", L"Audio", SS_LEFT, 0);
    m_cmbAudio = Ctl(WC_COMBOBOXW, L"", combo, IDC_AUDIO);
    m_lblPerf = Ctl(L"STATIC", L"Performance", SS_LEFT, 0);
    m_cmbPerf = Ctl(WC_COMBOBOXW, L"", combo, IDC_PERF);

    m_lblOutput = Ctl(L"STATIC", L"Save to:", SS_LEFT, 0);
    m_edtOutput = Ctl(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, IDC_OUTPUT);
    m_btnBrowse = Ctl(L"BUTTON", L"Browse…", btn, IDC_BROWSE);
    m_chkNextTo = Ctl(L"BUTTON", L"Save next to the original video", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_NEXTTO);
    m_chkRecycle = Ctl(L"BUTTON", L"Move the original to the Recycle Bin after a successful compression",
                       BS_AUTOCHECKBOX | WS_TABSTOP, IDC_RECYCLE);

    m_lblStatus = Ctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    m_lblFile = Ctl(L"STATIC", L"Current", SS_LEFT, 0);
    m_prgFile = Ctl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0);
    m_lblTotal = Ctl(L"STATIC", L"Overall", SS_LEFT, 0);
    m_prgTotal = Ctl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0);
    SendMessageW(m_prgFile, PBM_SETRANGE32, 0, 1000);
    SendMessageW(m_prgTotal, PBM_SETRANGE32, 0, 1000);

    m_btnStart = Ctl(L"BUTTON", L"Compress selected", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_START);
    m_btnCancel = Ctl(L"BUTTON", L"Cancel", btn, IDC_CANCEL);

    // Update bar (hidden until a new version is found)
    m_lblUpdate = Ctl(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX, 0);
    m_btnUpdate = Ctl(L"BUTTON", L"Update now", BS_PUSHBUTTON | WS_TABSTOP, IDC_UPD_GO);
    m_btnNotes = Ctl(L"BUTTON", L"What's new", btn, IDC_UPD_NOTES);
    m_btnSkip = Ctl(L"BUTTON", L"Skip this version", btn, IDC_UPD_SKIP);
    for (HWND h : {m_lblUpdate, m_btnUpdate, m_btnNotes, m_btnSkip}) ShowWindow(h, SW_HIDE);

    for (auto* l : kQualityLabels) ComboBox_AddString(m_cmbQuality, l);
    for (auto* l : kResLabels) ComboBox_AddString(m_cmbRes, l);
    for (auto* l : kAudioLabels) ComboBox_AddString(m_cmbAudio, l);
    for (auto* l : kPerfLabels) ComboBox_AddString(m_cmbPerf, l);
    RebuildCodecCombo();
}

void MainWindow::Layout() {
    if (!m_list) return;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = S(16), gap = S(8), rowH = S(28);

    HDWP dwp = BeginDeferWindowPos(40);
    auto move = [&](HWND h, int x, int y, int w, int hh) {
        dwp = DeferWindowPos(dwp, h, nullptr, x, y, std::max(w, 0), std::max(hh, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // Toolbar row
    // Update bar
    if (m_updateBarVisible) {
        const int by = BannerHeight() + (UpdateBarHeight() - rowH) / 2;
        int bx = W - m - S(130);
        move(m_btnSkip, bx, by, S(130), rowH);
        bx -= gap + S(110);
        move(m_btnNotes, bx, by, S(110), rowH);
        bx -= gap + S(120);
        move(m_btnUpdate, bx, by, S(120), rowH);
        move(m_lblUpdate, m, by, bx - gap - m, rowH);
    }

    int y = BannerHeight() + UpdateBarHeight() + S(12);
    int x = m;
    move(m_lblGame, x, y + S(6), S(42), S(18));
    x += S(44);
    move(m_cmbGame, x, y + S(2), S(260), S(320));
    x += S(260) + gap;
    move(m_btnCheckAll, x, y, S(100), rowH);
    x += S(100) + gap;
    move(m_btnRemove, x, y, S(90), rowH);

    int rx = W - m;
    rx -= S(90);
    move(m_btnRefresh, rx, y, S(90), rowH);
    rx -= gap + S(130);
    move(m_btnLibrary, rx, y, S(130), rowH);
    rx -= gap + S(120);
    move(m_btnAdd, rx, y, S(120), rowH);
    y += rowH + S(10);

    // Bottom bar & settings panel heights
    const int bottomH = S(70);
    const int settingsH = S(150);
    const int bottomY = H - m - bottomH;
    const int settingsY = bottomY - gap - settingsH;

    move(m_list, m, y, W - 2 * m, std::max(settingsY - gap - y, S(80)));

    // Settings group
    move(m_grpSettings, m, settingsY, W - 2 * m, settingsH);
    const int pad = S(16);
    const int innerX = m + pad, innerW = W - 2 * m - 2 * pad;
    const int colGap = S(16);
    const int colW = (innerW - 4 * colGap) / 5;
    int cy = settingsY + S(26);
    HWND labels[] = {m_lblCodec, m_lblQuality, m_lblRes, m_lblPerf, m_lblAudio};
    HWND combos[] = {m_cmbCodec, m_cmbQuality, m_cmbRes, m_cmbPerf, m_cmbAudio};
    const int widths[] = {colW + colW / 4, colW, colW / 2, colW + colW / 4, colW};
    int cx = innerX;
    for (int i = 0; i < 5; ++i) {
        move(labels[i], cx, cy, widths[i], S(18));
        move(combos[i], cx, cy + S(20), widths[i], S(300));
        cx += widths[i] + colGap;
    }

    cy += S(58);
    move(m_lblOutput, innerX, cy + S(4), S(56), S(18));
    move(m_edtOutput, innerX + S(60), cy, innerW - S(60) - gap - S(100), S(24));
    move(m_btnBrowse, innerX + innerW - S(100), cy - S(1), S(100), S(26));

    cy += S(34);
    move(m_chkNextTo, innerX, cy, S(240), S(20));
    move(m_chkRecycle, innerX + S(256), cy, innerW - S(256), S(20));

    // Bottom bar
    const int btnW = S(170), cancelW = S(96), btnH = S(38);
    const int btnY = bottomY + (bottomH - btnH) / 2 + S(4);
    move(m_btnCancel, W - m - cancelW, btnY, cancelW, btnH);
    move(m_btnStart, W - m - cancelW - gap - btnW, btnY, btnW, btnH);

    const int progW = W - 2 * m - cancelW - btnW - 2 * gap - S(16);
    move(m_lblStatus, m, bottomY, progW, S(20));
    move(m_lblFile, m, bottomY + S(26), S(56), S(16));
    move(m_prgFile, m + S(60), bottomY + S(28), progW - S(60), S(12));
    move(m_lblTotal, m, bottomY + S(48), S(56), S(16));
    move(m_prgTotal, m + S(60), bottomY + S(50), progW - S(60), S(12));

    EndDeferWindowPos(dwp);

    // Let the Result column take whatever width is left, so the list never scrolls sideways.
    RECT lr;
    GetClientRect(m_list, &lr);
    int used = 0;
    for (int c = 0; c < ColCount; ++c)
        if (c != ColResult) used += ListView_GetColumnWidth(m_list, c);
    int scroll = (GetWindowLongW(m_list, GWL_STYLE) & WS_VSCROLL) ? 0 : GetSystemMetricsForDpi(SM_CXVSCROLL, m_dpi);
    ListView_SetColumnWidth(m_list, ColResult, std::max(S(180), (int)lr.right - used - scroll));
}

void MainWindow::Paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(m_hwnd, &ps);
    RECT client;
    GetClientRect(m_hwnd, &client);
    RECT banner{0, 0, client.right, BannerHeight()};

    // Double-buffer the banner to avoid flicker while resizing.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, banner.right), banner.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(kBannerBg);
    FillRect(mem, &banner, bg);
    DeleteObject(bg);

    RECT accent{0, banner.bottom - S(3), banner.right, banner.bottom};
    HBRUSH ab = CreateSolidBrush(kAccent);
    FillRect(mem, &accent, ab);
    DeleteObject(ab);

    SetBkMode(mem, TRANSPARENT);
    const int m = S(18);

    HGDIOBJ oldFont = SelectObject(mem, m_fontTitle);
    SetTextColor(mem, kAccent);
    RECT t{m, S(8), banner.right / 2, S(44)};
    DrawTextW(mem, L"NV", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_CALCRECT);
    int nvRight = t.right;
    t.bottom = S(44);
    DrawTextW(mem, L"NV", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SetTextColor(mem, kBannerText);
    RECT t2{nvRight, S(8), banner.right / 2, S(44)};
    DrawTextW(mem, L"LOW", -1, &t2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(mem, m_fontSubtitle);
    SetTextColor(mem, kBannerMuted);
    RECT sub{m, S(42), banner.right / 2, S(64)};
    DrawTextW(mem, L"Shrink NVIDIA recordings — keep the quality, lose the gigabytes.", -1, &sub,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(mem, m_font);
    const int rightX = banner.right / 2;
    std::wstring lib = L"Library:  " + (m_libraryRoot.empty() ? std::wstring(L"NVIDIA folder not found") : m_libraryRoot);
    RECT r1{rightX, S(16), banner.right - m, S(36)};
    SetTextColor(mem, kBannerText);
    DrawTextW(mem, lib.c_str(), -1, &r1, DT_RIGHT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    std::wstring enc = L"Encoder:  " + m_encoderText;
    RECT r2{rightX, S(38), banner.right - m, S(58)};
    const unsigned gpuMask = (1u << (int)Codec::Av1Gpu) | (1u << (int)Codec::HevcGpu) | (1u << (int)Codec::H264Gpu);
    SetTextColor(mem, (m_caps & gpuMask) ? kAccent : kBannerMuted);
    DrawTextW(mem, enc.c_str(), -1, &r2, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    BitBlt(dc, 0, 0, banner.right, banner.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(m_hwnd, &ps);
}

// ---------------------------------------------------------------------------------------------
// Commands & notifications
// ---------------------------------------------------------------------------------------------

void MainWindow::OnCommand(int id, int code) {
    switch (id) {
        case IDC_GAME:
            if (code == CBN_SELCHANGE) {
                RebuildList();
                m_settings.gameFilter = CurrentGameFilter();
                SaveSettings(m_settings);
            }
            break;
        case IDC_CHECKALL: {
            bool anyUnchecked = false;
            for (int i = 0, n = ListView_GetItemCount(m_list); i < n; ++i)
                if (!ListView_GetCheckState(m_list, i)) anyUnchecked = true;
            SetAllChecked(anyUnchecked);
            break;
        }
        case IDC_REMOVE: RemoveSelected(); break;
        case IDC_ADD: AddFilesDialog(); break;
        case IDC_LIBRARY: ChooseLibrary(); break;
        case IDC_REFRESH: ScanLibrary(); break;
        case IDC_BROWSE: ChooseOutputDir(); break;
        case IDC_OUTPUT:
            if (code == EN_KILLFOCUS) {
                ReadSettingsFromControls();
                MarkExistingOutputs();
            }
            break;
        case IDC_NEXTTO:
            UpdateOutputControls();
            ReadSettingsFromControls();
            MarkExistingOutputs();
            break;
        case IDC_CODEC:
            if (code == CBN_SELCHANGE) {
                m_settings.codecAuto = false;
                ReadSettingsFromControls();
            }
            break;
        case IDC_QUALITY:
        case IDC_RES:
        case IDC_AUDIO:
        case IDC_PERF:
            if (code == CBN_SELCHANGE) ReadSettingsFromControls();
            break;
        case IDC_START: StartCompression(); break;
        case IDC_CANCEL: CancelCompression(); break;
        case IDC_UPD_GO: OnUpdateButton(); break;
        case IDC_UPD_NOTES:
            if (!m_update.pageUrl.empty())
                ShellExecuteW(m_hwnd, L"open", m_update.pageUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDC_UPD_SKIP:
            m_settings.skippedVersion = m_update.version;
            SaveSettings(m_settings);
            ShowUpdateBar(false);
            break;
    }
}

LRESULT MainWindow::OnNotify(NMHDR* hdr) {
    if (hdr->hwndFrom != m_list) return 0;

    switch (hdr->code) {
        case LVN_GETDISPINFOW: {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
            if (di->item.mask & LVIF_TEXT) {
                auto it = m_items.find((int)di->item.lParam);
                if (it == m_items.end()) {
                    LVITEMW lv{};
                    lv.mask = LVIF_PARAM;
                    lv.iItem = di->item.iItem;
                    ListView_GetItem(m_list, &lv);
                    it = m_items.find((int)lv.lParam);
                }
                std::wstring text = it != m_items.end() ? ColumnText(it->second, di->item.iSubItem) : L"";
                lstrcpynW(di->item.pszText, text.c_str(), di->item.cchTextMax);
            }
            return 0;
        }

        case LVN_ITEMCHANGED: {
            auto* nm = reinterpret_cast<NMLISTVIEW*>(hdr);
            if (!m_populating && (nm->uChanged & LVIF_STATE) &&
                ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) && nm->iItem >= 0) {
                auto it = m_items.find((int)nm->lParam);
                if (it != m_items.end()) it->second.checked = ListView_GetCheckState(m_list, nm->iItem) != 0;
                UpdateSummary();
            }
            return 0;
        }

        case LVN_COLUMNCLICK: {
            int col = reinterpret_cast<NMLISTVIEW*>(hdr)->iSubItem;
            if (col == m_sortColumn)
                m_sortAscending = !m_sortAscending;
            else {
                m_sortColumn = col;
                m_sortAscending = col == ColName || col == ColGame;
            }
            SortList();
            m_settings.sortColumn = m_sortColumn;
            m_settings.sortAscending = m_sortAscending;
            SaveSettings(m_settings);
            return 0;
        }

        case LVN_KEYDOWN: {
            auto* kd = reinterpret_cast<NMLVKEYDOWN*>(hdr);
            if (kd->wVKey == VK_DELETE) {
                RemoveSelected();
            } else if (kd->wVKey == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                ListView_SetItemState(m_list, -1, LVIS_SELECTED, LVIS_SELECTED);
            } else if (kd->wVKey == VK_F5) {
                ScanLibrary();
            }
            return 0;
        }

        case NM_DBLCLK: {
            auto* ia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
            if (ia->iItem >= 0) {
                LVITEMW lv{};
                lv.mask = LVIF_PARAM;
                lv.iItem = ia->iItem;
                ListView_GetItem(m_list, &lv);
                auto it = m_items.find((int)lv.lParam);
                if (it != m_items.end()) OpenPath(it->second.path, false);
            }
            return 0;
        }

        case NM_CUSTOMDRAW: {
            auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
            if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
                if (cd->iSubItem == ColResult) {
                    auto it = m_items.find((int)cd->nmcd.lItemlParam);
                    if (it != m_items.end()) {
                        switch (it->second.state) {
                            case ItemState::Done: cd->clrText = kOkText; break;
                            case ItemState::Failed: cd->clrText = kFailText; break;
                            case ItemState::Encoding:
                            case ItemState::Queued: cd->clrText = kBusyText; break;
                            case ItemState::Analyzing:
                            case ItemState::NoGain:
                            case ItemState::Cancelled: cd->clrText = GetSysColor(COLOR_GRAYTEXT); break;
                            default: break;
                        }
                    }
                } else if (cd->iSubItem != ColName) {
                    cd->clrText = RGB(90, 90, 90);
                }
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
    }
    return 0;
}

void MainWindow::OnContextMenu(int x, int y) {
    std::vector<int> sel = SelectedIds();
    if (x == -1 && y == -1) {
        int row = ListView_GetNextItem(m_list, -1, LVNI_FOCUSED);
        RECT r{};
        if (row >= 0) ListView_GetItemRect(m_list, row, &r, LVIR_LABEL);
        POINT pt{r.left + S(20), r.bottom};
        ClientToScreen(m_list, &pt);
        x = pt.x;
        y = pt.y;
    }

    const VideoItem* first = sel.empty() ? nullptr : &m_items.at(sel.front());
    bool hasOutput = first && !first->outputPath.empty() && FileExists(first->outputPath);

    HMENU menu = CreatePopupMenu();
    UINT one = sel.size() == 1 ? MF_STRING : MF_STRING | MF_GRAYED;
    AppendMenuW(menu, one, IDM_OPEN, L"Play original");
    AppendMenuW(menu, one, IDM_REVEAL, L"Show original in folder");
    AppendMenuW(menu, sel.size() == 1 && hasOutput ? MF_STRING : MF_STRING | MF_GRAYED, IDM_OPEN_OUTPUT,
                L"Play compressed video");
    AppendMenuW(menu, sel.size() == 1 && hasOutput ? MF_STRING : MF_STRING | MF_GRAYED, IDM_REVEAL_OUTPUT,
                L"Show compressed video in folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_CHECK_ALL, L"Select all");
    AppendMenuW(menu, MF_STRING, IDM_UNCHECK_ALL, L"Select none");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, sel.empty() ? MF_STRING | MF_GRAYED : MF_STRING, IDM_REMOVE, L"Remove from list\tDel");

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case IDM_OPEN: OpenPath(first->path, false); break;
        case IDM_REVEAL: OpenPath(first->path, true); break;
        case IDM_OPEN_OUTPUT: OpenPath(first->outputPath, false); break;
        case IDM_REVEAL_OUTPUT: OpenPath(first->outputPath, true); break;
        case IDM_CHECK_ALL: SetAllChecked(true); break;
        case IDM_UNCHECK_ALL: SetAllChecked(false); break;
        case IDM_REMOVE: RemoveSelected(); break;
    }
}

void MainWindow::OnDropFiles(HDROP drop) {
    std::vector<std::wstring> paths;
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        paths.push_back(p);
    }
    DragFinish(drop);
    AddPaths(paths);
}

// ---------------------------------------------------------------------------------------------
// Library & list
// ---------------------------------------------------------------------------------------------

void MainWindow::ScanLibrary() {
    if (m_busy) return;

    // Keep user-imported files; re-read everything from the library.
    for (auto it = m_items.begin(); it != m_items.end();) {
        if (!it->second.imported) {
            m_knownPaths.erase(ToLower(it->second.path));
            it = m_items.erase(it);
        } else {
            ++it;
        }
    }

    for (const FoundVideo& v : ScanLibraryFolder(m_libraryRoot)) AddItem(v.path, v.game, false, v.size, v.modified);

    MarkExistingOutputs();
    RebuildGameCombo();
    RebuildList();
    InvalidateRect(m_hwnd, nullptr, FALSE);

    if (m_items.empty())
        SetStatus(m_libraryRoot.empty() ? L"No NVIDIA recordings folder found. Use “Change folder…” or drag videos here."
                                        : L"No videos found. Record with NVIDIA, or drag any video here.");
}

void MainWindow::AddItem(const std::wstring& path, const std::wstring& game, bool imported, uint64_t size,
                         FILETIME modified) {
    std::wstring key = ToLower(path);
    if (m_knownPaths.count(key)) return;
    m_knownPaths.insert(key);

    VideoItem item;
    item.id = m_nextId++;
    item.path = path;
    item.name = FileNameOf(path);
    item.game = game;
    item.imported = imported;
    item.size = size;
    item.modified = modified;
    item.state = ItemState::Analyzing;
    m_items.emplace(item.id, item);
    m_probe.Push(item.id, path);
}

void MainWindow::AddPaths(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> files;
    for (const auto& p : paths) {
        if (DirExists(p)) {
            auto found = FindVideosIn(p);
            files.insert(files.end(), found.begin(), found.end());
        } else if (IsVideoFile(p)) {
            files.push_back(p);
        }
    }
    if (files.empty()) {
        SetStatus(L"No supported video files in what you dropped.");
        return;
    }

    const std::wstring rootLower = ToLower(m_libraryRoot) + L"\\";
    size_t added = m_items.size();
    for (const auto& f : files) {
        uint64_t size = 0;
        FILETIME ft{};
        if (!GetFileInfo(f, size, ft)) continue;
        std::wstring lower = ToLower(f);
        std::wstring game = kImportedGame;
        bool imported = true;
        if (!m_libraryRoot.empty() && lower.starts_with(rootLower)) {
            std::wstring rel = f.substr(rootLower.size());
            size_t sep = rel.find(L'\\');
            game = sep == std::wstring::npos ? FileNameOf(m_libraryRoot) : rel.substr(0, sep);
            imported = false;
        }
        AddItem(f, game, imported, size, ft);
    }
    added = m_items.size() - added;

    MarkExistingOutputs();
    RebuildGameCombo();
    ComboBox_SetCurSel(m_cmbGame, 0);  // show everything so the new files are visible
    RebuildList();
    SetStatus(std::to_wstring(added) + (added == 1 ? L" video added." : L" videos added."));
}

void MainWindow::RebuildGameCombo() {
    // First fill after startup: restore the game the user had selected last time.
    std::wstring current = m_gameFilters.empty() ? m_settings.gameFilter : CurrentGameFilter();
    std::map<std::wstring, int> counts;
    for (const auto& [id, it] : m_items) counts[it.game]++;

    ComboBox_ResetContent(m_cmbGame);
    m_gameFilters.clear();
    m_gameFilters.push_back(L"");
    ComboBox_AddString(m_cmbGame, (L"All games  (" + std::to_wstring(m_items.size()) + L")").c_str());
    int sel = 0;
    for (const auto& [game, n] : counts) {
        if (game == current) sel = (int)m_gameFilters.size();
        m_gameFilters.push_back(game);
        ComboBox_AddString(m_cmbGame, (game + L"  (" + std::to_wstring(n) + L")").c_str());
    }
    ComboBox_SetCurSel(m_cmbGame, sel);
}

std::wstring MainWindow::CurrentGameFilter() const {
    int sel = m_cmbGame ? ComboBox_GetCurSel(m_cmbGame) : -1;
    return sel > 0 && sel < (int)m_gameFilters.size() ? m_gameFilters[sel] : std::wstring();
}

void MainWindow::RebuildList() {
    m_populating = true;
    SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(m_list);

    const std::wstring filter = CurrentGameFilter();
    int row = 0;
    for (const auto& [id, item] : m_items) {
        if (!filter.empty() && item.game != filter) continue;
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_PARAM;
        lv.iItem = row;
        lv.pszText = LPSTR_TEXTCALLBACKW;
        lv.lParam = id;
        int idx = ListView_InsertItem(m_list, &lv);
        for (int c = 1; c < ColCount; ++c) ListView_SetItemText(m_list, idx, c, LPSTR_TEXTCALLBACKW);
        ListView_SetCheckState(m_list, idx, item.checked);
        ++row;
    }

    SortList();
    SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(m_list, nullptr, TRUE);
    m_populating = false;
    UpdateSummary();
}

int CALLBACK MainWindow::CompareItems(LPARAM a, LPARAM b, LPARAM selfPtr) {
    auto* self = reinterpret_cast<MainWindow*>(selfPtr);
    const VideoItem& A = self->m_items.at((int)a);
    const VideoItem& B = self->m_items.at((int)b);
    auto cmp = [](auto x, auto y) { return x < y ? -1 : (y < x ? 1 : 0); };
    int r = 0;
    switch (self->m_sortColumn) {
        case ColName: r = StrCmpLogicalW(A.name.c_str(), B.name.c_str()); break;
        case ColGame: r = StrCmpLogicalW(A.game.c_str(), B.game.c_str()); break;
        case ColDate: r = CompareFileTime(&A.modified, &B.modified); break;
        case ColDuration: r = cmp(A.info.duration, B.info.duration); break;
        case ColVideo: r = cmp((long long)A.info.width * A.info.height, (long long)B.info.width * B.info.height); break;
        case ColBitrate: r = cmp(A.info.bitrate, B.info.bitrate); break;
        case ColSize: r = cmp(A.size, B.size); break;
        case ColResult: r = cmp(StateRank(A.state), StateRank(B.state)); break;
    }
    if (r == 0) r = CompareFileTime(&B.modified, &A.modified);
    return self->m_sortAscending ? r : -r;
}

void MainWindow::SortList() {
    ListView_SortItems(m_list, CompareItems, (LPARAM)this);
    UpdateSortArrows();
}

void MainWindow::UpdateSortArrows() {
    HWND header = ListView_GetHeader(m_list);
    for (int c = 0; c < ColCount; ++c) {
        HDITEMW hd{};
        hd.mask = HDI_FORMAT;
        Header_GetItem(header, c, &hd);
        hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (c == m_sortColumn) hd.fmt |= m_sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(header, c, &hd);
    }
}

int MainWindow::FindRow(int id) const {
    LVFINDINFOW fi{};
    fi.flags = LVFI_PARAM;
    fi.lParam = id;
    return ListView_FindItem(m_list, -1, &fi);
}

void MainWindow::RefreshRow(int id) {
    int row = FindRow(id);
    if (row >= 0) ListView_RedrawItems(m_list, row, row);
}

std::wstring MainWindow::ColumnText(const VideoItem& item, int column) const {
    const MediaInfo& mi = item.info;
    switch (column) {
        case ColName: return item.name;
        case ColGame: return item.game;
        case ColDate: return FormatDate(item.modified);
        case ColDuration: return mi.ok ? FormatDuration(mi.duration) : L"—";
        case ColVideo: {
            if (!mi.ok) return L"—";
            wchar_t buf[96];
            swprintf(buf, 96, L"%d×%d · %.0f fps · %ls", mi.width, mi.height, mi.fps, mi.videoCodec.c_str());
            return buf;
        }
        case ColBitrate: {
            double br = mi.bitrate > 0 ? mi.bitrate : (mi.duration > 0 ? item.size * 8.0 / mi.duration : 0);
            return br > 0 ? FormatBitrate(br) : L"—";
        }
        case ColSize: return FormatSize(item.size);
        case ColResult:
            switch (item.state) {
                case ItemState::Analyzing: return L"Analyzing…";
                case ItemState::Queued: return L"Queued";
                case ItemState::Ready: return item.result.empty() ? L"Ready" : item.result;
                default: return item.result;
            }
    }
    return {};
}

std::vector<int> MainWindow::SelectedIds() const {
    std::vector<int> ids;
    for (int row = ListView_GetNextItem(m_list, -1, LVNI_SELECTED); row >= 0;
         row = ListView_GetNextItem(m_list, row, LVNI_SELECTED)) {
        LVITEMW lv{};
        lv.mask = LVIF_PARAM;
        lv.iItem = row;
        ListView_GetItem(m_list, &lv);
        ids.push_back((int)lv.lParam);
    }
    return ids;
}

std::vector<int> MainWindow::TargetIds() const {
    // Checked rows in the current view; if none are checked, fall back to highlighted rows.
    std::vector<int> ids;
    for (int row = 0, n = ListView_GetItemCount(m_list); row < n; ++row) {
        if (!ListView_GetCheckState(m_list, row)) continue;
        LVITEMW lv{};
        lv.mask = LVIF_PARAM;
        lv.iItem = row;
        ListView_GetItem(m_list, &lv);
        ids.push_back((int)lv.lParam);
    }
    return ids.empty() ? SelectedIds() : ids;
}

void MainWindow::SetAllChecked(bool checked) {
    m_populating = true;
    for (int row = 0, n = ListView_GetItemCount(m_list); row < n; ++row) {
        ListView_SetCheckState(m_list, row, checked);
        LVITEMW lv{};
        lv.mask = LVIF_PARAM;
        lv.iItem = row;
        ListView_GetItem(m_list, &lv);
        m_items.at((int)lv.lParam).checked = checked;
    }
    m_populating = false;
    UpdateSummary();
}

void MainWindow::RemoveSelected() {
    std::vector<int> ids = SelectedIds();
    bool skipped = false;
    for (int id : ids) {
        auto it = m_items.find(id);
        if (it == m_items.end()) continue;
        if (it->second.state == ItemState::Queued || it->second.state == ItemState::Encoding) {
            skipped = true;
            continue;
        }
        m_knownPaths.erase(ToLower(it->second.path));
        m_items.erase(it);
        int row = FindRow(id);
        if (row >= 0) ListView_DeleteItem(m_list, row);
    }
    RebuildGameCombo();
    UpdateSummary();
    if (skipped) SetStatus(L"Videos that are being compressed can't be removed.");
}

void MainWindow::OpenPath(const std::wstring& path, bool select) {
    if (select) {
        std::wstring args = L"/select," + QuoteArg(path);
        ShellExecuteW(m_hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(m_hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::MarkExistingOutputs() {
    for (auto& [id, item] : m_items) {
        if (item.state == ItemState::Queued || item.state == ItemState::Encoding) continue;
        std::wstring out = OutputPathFor(item);
        uint64_t size = 0;
        FILETIME ft;
        if (GetFileInfo(out, size, ft)) {
            item.state = ItemState::Done;
            item.outputPath = out;
            item.outSize = size;
            item.result = L"✓ Already compressed · " + FormatSize(size) + L"  (" + Percent(item.size, size) + L")";
        } else if (item.state == ItemState::Done) {
            item.state = ItemState::Ready;
            item.result.clear();
            item.outputPath.clear();
        }
    }
    InvalidateRect(m_list, nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

void MainWindow::RebuildCodecCombo() {
    int sel = ComboBox_GetCurSel(m_cmbCodec);
    if (sel < 0) sel = (int)m_settings.encode.codec;
    ComboBox_ResetContent(m_cmbCodec);
    for (int c = 0; c < (int)Codec::Count; ++c) {
        std::wstring label = kCodecLabels[c];
        if (m_capsKnown && !(m_caps & (1u << c)))
            label += L"   (not available)";
        else if (c == (int)Codec::Av1Gpu && !m_av1Playable)
            label += L"   (Windows needs the free AV1 add-on to play it)";
        ComboBox_AddString(m_cmbCodec, label.c_str());
    }
    ComboBox_SetCurSel(m_cmbCodec, sel);
}

void MainWindow::LoadSettingsIntoControls() {
    const AppSettings& s = m_settings;
    ComboBox_SetCurSel(m_cmbCodec, (int)s.encode.codec);
    ComboBox_SetCurSel(m_cmbQuality, (int)s.encode.quality);
    ComboBox_SetCurSel(m_cmbRes, (int)s.encode.resolution);
    ComboBox_SetCurSel(m_cmbAudio, (int)s.encode.audio);
    ComboBox_SetCurSel(m_cmbPerf, (int)s.encode.performance);
    SetWindowTextW(m_edtOutput, s.outputDir.c_str());
    Button_SetCheck(m_chkNextTo, s.saveNextToOriginal ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(m_chkRecycle, s.recycleOriginal ? BST_CHECKED : BST_UNCHECKED);
    UpdateOutputControls();
}

void MainWindow::ReadSettingsFromControls() {
    AppSettings& s = m_settings;
    s.encode.codec = (Codec)std::max(0, ComboBox_GetCurSel(m_cmbCodec));
    s.encode.quality = (Quality)std::max(0, ComboBox_GetCurSel(m_cmbQuality));
    s.encode.resolution = (TargetRes)std::max(0, ComboBox_GetCurSel(m_cmbRes));
    s.encode.audio = (AudioMode)std::max(0, ComboBox_GetCurSel(m_cmbAudio));
    s.encode.performance = (Performance)std::max(0, ComboBox_GetCurSel(m_cmbPerf));
    wchar_t buf[MAX_PATH * 2] = {};
    GetWindowTextW(m_edtOutput, buf, (int)std::size(buf));
    s.outputDir = buf;
    while (!s.outputDir.empty() && (s.outputDir.back() == L'\\' || s.outputDir.back() == L' ')) s.outputDir.pop_back();
    if (s.outputDir.empty()) s.outputDir = DefaultOutputDir();
    s.saveNextToOriginal = Button_GetCheck(m_chkNextTo) == BST_CHECKED;
    s.recycleOriginal = Button_GetCheck(m_chkRecycle) == BST_CHECKED;
    SaveSettings(s);  // remember every change immediately, not only on exit
}

void MainWindow::UpdateOutputControls() {
    bool custom = Button_GetCheck(m_chkNextTo) != BST_CHECKED;
    EnableWindow(m_edtOutput, custom && !m_busy);
    EnableWindow(m_btnBrowse, custom && !m_busy);
}

std::wstring MainWindow::OutputPathFor(const VideoItem& item) const {
    std::wstring dir;
    if (m_settings.saveNextToOriginal) {
        dir = ParentOf(item.path);
    } else {
        dir = m_settings.outputDir;
        if (!item.imported && !item.game.empty()) dir += L"\\" + item.game;
    }
    return dir + L"\\" + StemOf(item.path) + L"_nvlow.mp4";
}

std::wstring MainWindow::PickFolder(const wchar_t* title, const std::wstring& initial) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (!initial.empty() && DirExists(initial)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    if (SUCCEEDED(dlg->Show(m_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

void MainWindow::ChooseLibrary() {
    if (m_busy) return;
    std::wstring dir = PickFolder(L"Choose the folder where your recordings are saved", m_libraryRoot);
    if (dir.empty()) return;
    m_libraryRoot = dir;
    m_settings.libraryRoot = dir;
    ScanLibrary();
}

void MainWindow::ChooseOutputDir() {
    ReadSettingsFromControls();
    std::wstring dir = PickFolder(L"Choose where compressed videos are saved", m_settings.outputDir);
    if (dir.empty()) return;
    SetWindowTextW(m_edtOutput, dir.c_str());
    ReadSettingsFromControls();
    MarkExistingOutputs();
}

void MainWindow::AddFilesDialog() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC filters[] = {
        {L"Video files", L"*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.flv;*.wmv;*.ts;*.m2ts;*.mpg;*.mpeg"},
        {L"All files", L"*.*"},
    };
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(L"Add videos to compress");
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);

    std::vector<std::wstring> paths;
    if (SUCCEEDED(dlg->Show(m_hwnd))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item))) continue;
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    paths.emplace_back(p);
                    CoTaskMemFree(p);
                }
                item->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    if (!paths.empty()) AddPaths(paths);
}

// ---------------------------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------------------------

void MainWindow::DetectEncoders() {
    HWND hwnd = m_hwnd;
    m_capsThread = std::thread([hwnd] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        PostOwned(hwnd, WM_APP_CAPS, 0, new EncoderCaps(nv::DetectEncoders()));
        CoUninitialize();
    });
}

void MainWindow::OnEncoderCaps(const EncoderCaps& ec) {
    const unsigned caps = ec.available;
    m_caps = caps;
    m_capsKnown = true;

    std::wstring gpu;
    for (auto [codec, name] : {std::pair{Codec::Av1Gpu, L"AV1"}, {Codec::HevcGpu, L"HEVC"}, {Codec::H264Gpu, L"H.264"}})
        if (caps & (1u << (int)codec)) gpu += (gpu.empty() ? L"" : L", ") + std::wstring(name);
    m_encoderText = gpu.empty() ? L"No GPU video encoder found — using CPU"
                                : ec.gpuName + L"  (" + gpu + L")";

    RebuildCodecCombo();
    int sel = ComboBox_GetCurSel(m_cmbCodec);
    if (m_settings.codecAuto) {
        // Best codec this PC can both encode on the GPU and play back in Windows' own players.
        Codec best = Codec::H264Gpu;
        if ((caps & (1u << (int)Codec::Av1Gpu)) && m_av1Playable)
            best = Codec::Av1Gpu;
        else if ((caps & (1u << (int)Codec::HevcGpu)) && m_hevcPlayable)
            best = Codec::HevcGpu;
        else if (!(caps & (1u << (int)Codec::H264Gpu)))
            best = Codec::H264Cpu;
        ComboBox_SetCurSel(m_cmbCodec, (int)best);
        ReadSettingsFromControls();
    } else if (sel >= 0 && !(caps & (1u << sel))) {
        for (Codec fallback : {Codec::Av1Gpu, Codec::HevcGpu, Codec::H264Gpu, Codec::HevcCpu, Codec::H264Cpu})
            if (caps & (1u << (int)fallback)) {
                ComboBox_SetCurSel(m_cmbCodec, (int)fallback);
                break;
            }
        ReadSettingsFromControls();
    }
    RECT banner{0, 0, 0, BannerHeight()};
    GetClientRect(m_hwnd, &banner);
    banner.bottom = BannerHeight();
    InvalidateRect(m_hwnd, &banner, FALSE);
}

// ---------------------------------------------------------------------------------------------
// Updates (GitHub Releases)
// ---------------------------------------------------------------------------------------------

void MainWindow::StartUpdateCheck(bool manual) {
    if (m_updateBusy) return;
    if (m_updateThread.joinable()) m_updateThread.join();
    m_updateBusy = true;
    m_manualCheck = manual;
    HWND hwnd = m_hwnd;
    m_updateThread = std::thread([hwnd] {
        PostOwned(hwnd, WM_APP_UPDATE_INFO, 0, new UpdateInfo(CheckForUpdate(NVLOW_GITHUB_REPO, NVLOW_VERSION)));
    });
}

void MainWindow::OnUpdateInfo(const UpdateInfo& info) {
    m_updateBusy = false;
    const bool manual = m_manualCheck;
    m_manualCheck = false;

    if (!info.checked) {
        if (manual)
            MessageBoxW(m_hwnd, (L"Couldn't check for updates.\n\n" + info.error).c_str(), L"NVLOW", MB_ICONWARNING);
        return;
    }
    if (!info.available) {
        if (manual)
            MessageBoxW(m_hwnd, (std::wstring(L"You're up to date.\n\nNVLOW ") + NVLOW_VERSION + L" is the latest version.").c_str(),
                        L"NVLOW", MB_ICONINFORMATION);
        return;
    }
    // A version the user skipped only shows again when they ask explicitly.
    if (!manual && info.version == m_settings.skippedVersion) return;

    m_update = info;
    SetWindowTextW(m_lblUpdate, (L"NVLOW " + info.version + L" is available  —  you have " + NVLOW_VERSION + L".").c_str());
    SetWindowTextW(m_btnUpdate, IsInstalledCopy() ? L"Update now" : L"Download");
    ShowUpdateBar(true);
}

void MainWindow::ShowUpdateBar(bool show) {
    m_updateBarVisible = show;
    for (HWND h : {m_lblUpdate, m_btnUpdate, m_btnNotes, m_btnSkip}) {
        ShowWindow(h, show ? SW_SHOW : SW_HIDE);
        EnableWindow(h, TRUE);
    }
    Layout();
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

bool MainWindow::IsInstalledCopy() const {
    // Inno Setup puts its uninstaller next to the installed exe; a portable copy has none.
    return FileExists(ExeDirectory() + L"\\unins000.exe");
}

void MainWindow::OnUpdateButton() {
    if (m_updateBusy || !m_update.available) return;
    if (!IsInstalledCopy()) {
        // Portable copy: let the user download the new setup from the release page.
        ShellExecuteW(m_hwnd, L"open", m_update.pageUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (m_busy) {
        MessageBoxW(m_hwnd, L"Compression is running. Wait for it to finish (or cancel it), then click Update now.",
                    L"NVLOW", MB_ICONINFORMATION);
        return;
    }

    m_updateBusy = true;
    m_updateCancel = false;
    for (HWND h : {m_btnUpdate, m_btnSkip}) EnableWindow(h, FALSE);
    SetWindowTextW(m_lblUpdate, (L"Downloading NVLOW " + m_update.version + L"…").c_str());

    if (m_updateThread.joinable()) m_updateThread.join();
    HWND hwnd = m_hwnd;
    UpdateInfo info = m_update;
    m_updateThread = std::thread([this, hwnd, info] {
        std::wstring error;
        LONG lastPermille = -1;
        std::wstring path = DownloadUpdate(
            info,
            [&](double f) {
                LONG p = (LONG)(f * 1000);
                if (p / 10 != lastPermille / 10) {  // post on whole-percent changes only
                    lastPermille = p;
                    PostMessageW(hwnd, WM_APP_UPDATE_PROGRESS, (WPARAM)p, 0);
                }
            },
            m_updateCancel, error);
        PostOwned(hwnd, WM_APP_UPDATE_DONE, 0, new std::pair<std::wstring, std::wstring>(path, error));
    });
}

extern HANDLE g_singleInstanceMutex;

void MainWindow::OnUpdateDownloaded(const std::wstring& path, const std::wstring& error) {
    m_updateBusy = false;
    if (path.empty()) {
        SetWindowTextW(m_lblUpdate, (L"Update failed: " + error).c_str());
        SetWindowTextW(m_btnUpdate, L"Try again");
        for (HWND h : {m_btnUpdate, m_btnSkip}) EnableWindow(h, TRUE);
        return;
    }

    SetWindowTextW(m_lblUpdate, (L"Installing NVLOW " + m_update.version + L"…").c_str());
    // Install the same way the current copy was installed (per-user or for all users).
    std::wstring programFiles = ToLower(KnownFolder(FOLDERID_ProgramFiles));
    bool allUsers = !programFiles.empty() && ToLower(ExeDirectory()).starts_with(programFiles);
    std::wstring params = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCANCEL ";
    params += allUsers ? L"/ALLUSERS" : L"/CURRENTUSER";

    // Release the single-instance lock so the installer doesn't wait for us, then exit.
    if (g_singleInstanceMutex) {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }
    HINSTANCE r = ShellExecuteW(m_hwnd, L"open", path.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        SetWindowTextW(m_lblUpdate, L"Couldn't start the installer.");
        EnableWindow(m_btnUpdate, TRUE);
        return;
    }
    PostMessageW(m_hwnd, WM_CLOSE, 0, 0);  // the installer relaunches NVLOW when it's done
}

void MainWindow::ShowAbout() {
    std::wstring text = std::wstring(L"NVLOW ") + NVLOW_VERSION +
                        L"\nShrink NVIDIA recordings — keep the quality, lose the gigabytes.\n\n"
                        L"Encoder:  " + m_encoderText +
                        L"\nUpdates:  github.com/" + NVLOW_GITHUB_REPO +
                        L"\nSettings:  %APPDATA%\\NVLOW\\settings.ini";
    MessageBoxW(m_hwnd, text.c_str(), L"About NVLOW", MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------------------------
// Compression
// ---------------------------------------------------------------------------------------------

void MainWindow::StartCompression() {
    if (m_busy) return;
    ReadSettingsFromControls();

    if (m_capsKnown && !(m_caps & (1u << (int)m_settings.encode.codec))) {
        MessageBoxW(m_hwnd, L"The selected encoder is not available on this PC. Please choose another one.", L"NVLOW",
                    MB_ICONWARNING);
        return;
    }

    const Codec codec = m_settings.encode.codec;
    if ((codec == Codec::Av1Gpu && !m_av1Playable) ||
        ((codec == Codec::HevcGpu || codec == Codec::HevcCpu) && !m_hevcPlayable)) {
        const bool av1 = codec == Codec::Av1Gpu;
        std::wstring msg = std::wstring(L"Windows Media Player can't show ") + (av1 ? L"AV1" : L"HEVC") +
                           L" video on this PC yet: you would only hear the audio.\n\n"
                           L"Yes  —  open the Microsoft Store to install the " +
                           (av1 ? L"free AV1 Video Extension" : L"HEVC Video Extension") +
                           L"\nNo  —  compress anyway (VLC, browsers and Discord still play it)\n"
                           L"Cancel  —  go back and pick H.264 instead";
        int r = MessageBoxW(m_hwnd, msg.c_str(), L"NVLOW — video add-on missing", MB_ICONWARNING | MB_YESNOCANCEL);
        if (r == IDCANCEL) return;
        if (r == IDYES) {
            ShellExecuteW(m_hwnd, L"open",
                          av1 ? L"ms-windows-store://pdp/?ProductId=9MVZQVXJBQ9V"
                              : L"ms-windows-store://pdp/?ProductId=9NMZLZ57R3T7",
                          nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }

    std::vector<int> ids = TargetIds();
    if (ids.empty()) {
        MessageBoxW(m_hwnd, L"Tick the videos you want to compress (or select them in the list).", L"NVLOW",
                    MB_ICONINFORMATION);
        return;
    }

    if (!m_settings.saveNextToOriginal) {
        int r = SHCreateDirectoryExW(nullptr, m_settings.outputDir.c_str(), nullptr);
        if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) {
            MessageBoxW(m_hwnd, (L"Can't create the output folder:\n" + m_settings.outputDir).c_str(), L"NVLOW",
                        MB_ICONERROR);
            return;
        }
    }

    if (m_settings.recycleOriginal) {
        int r = MessageBoxW(m_hwnd,
                            L"Originals will be moved to the Recycle Bin after each successful compression.\n\nContinue?",
                            L"NVLOW", MB_ICONQUESTION | MB_YESNO);
        if (r != IDYES) return;
    }

    std::vector<QueuedJob> jobs;
    for (int id : ids) {
        VideoItem& item = m_items.at(id);
        if (!FileExists(item.path)) {
            item.state = ItemState::Failed;
            item.result = L"File no longer exists";
            RefreshRow(id);
            continue;
        }
        QueuedJob q;
        q.id = id;
        q.inputSize = item.size;
        q.job.input = item.path;
        q.job.output = OutputPathFor(item);
        q.job.duration = item.info.duration;
        q.job.sourceBitrate = item.info.bitrate;
        q.job.options = m_settings.encode;
        item.state = ItemState::Queued;
        item.result.clear();
        item.outputPath = q.job.output;
        RefreshRow(id);
        jobs.push_back(std::move(q));
    }
    if (jobs.empty()) return;

    if (m_worker.joinable()) m_worker.join();
    m_queueTotal = (int)jobs.size();
    m_queueDone = 0;
    m_batchIn = m_batchOut = 0;
    m_batchOk = m_batchFailed = 0;
    m_cancelRequested = false;
    SendMessageW(m_prgFile, PBM_SETPOS, 0, 0);
    SendMessageW(m_prgTotal, PBM_SETPOS, 0, 0);
    SetBusy(true);

    m_encoder = std::make_unique<Encoder>();
    m_worker = std::thread(&MainWindow::WorkerMain, this, std::move(jobs));
}

void MainWindow::CancelCompression() {
    if (!m_busy || !m_encoder) return;
    m_cancelRequested = true;
    m_encoder->Cancel();
    SetStatus(L"Cancelling…");
}

void MainWindow::WorkerMain(std::vector<QueuedJob> jobs) {
    const HWND hwnd = m_hwnd;
    Encoder* encoder = m_encoder.get();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Low performance: run NVLOW below other apps so games and work stay smooth.
    const bool low = !jobs.empty() && jobs.front().job.options.performance == Performance::Low;
    if (low) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    for (QueuedJob& q : jobs) {
        auto* outcome = new JobOutcome();
        outcome->output = q.job.output;
        if (encoder->Cancelled()) {
            outcome->result.cancelled = true;
            PostOwned(hwnd, WM_APP_JOB_DONE, q.id, outcome);
            continue;
        }
        PostMessageW(hwnd, WM_APP_JOB_START, q.id, 0);

        SHCreateDirectoryExW(nullptr, ParentOf(q.job.output).c_str(), nullptr);
        EncodeJob job = q.job;
        job.output = q.job.output + L".part";

        outcome->result = encoder->Run(job, [&](const EncodeProgress& p) {
            PostOwned(hwnd, WM_APP_JOB_PROGRESS, q.id, new EncodeProgress(p));
        });

        if (outcome->result.ok) {
            if (!MoveFileExW(job.output.c_str(), q.job.output.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                outcome->result.ok = false;
                outcome->result.error = L"Could not write the output file.";
            } else {
                // Keep the original timestamps so galleries keep sorting by recording date.
                HANDLE src = CreateFileW(q.job.input.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, 0, nullptr);
                HANDLE dst = CreateFileW(q.job.output.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, 0, nullptr);
                FILETIME c, a, w;
                if (src != INVALID_HANDLE_VALUE && dst != INVALID_HANDLE_VALUE && GetFileTime(src, &c, &a, &w))
                    SetFileTime(dst, &c, &a, &w);
                if (src != INVALID_HANDLE_VALUE) CloseHandle(src);
                if (dst != INVALID_HANDLE_VALUE) CloseHandle(dst);

                FILETIME ft;
                GetFileInfo(q.job.output, outcome->outSize, ft);
                if (outcome->outSize == 0 || outcome->outSize >= q.inputSize) {
                    outcome->noGain = true;
                    DeleteFileW(q.job.output.c_str());
                }
            }
        } else {
            DeleteFileW(job.output.c_str());
        }
        PostOwned(hwnd, WM_APP_JOB_DONE, q.id, outcome);
    }
    if (low) SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    CoUninitialize();
    PostMessageW(hwnd, WM_APP_QUEUE_DONE, 0, 0);
}

void MainWindow::SetBusy(bool busy) {
    m_busy = busy;
    EnableWindow(m_btnStart, !busy);
    EnableWindow(m_btnCancel, busy);
    for (HWND h : {m_btnLibrary, m_btnRefresh, m_cmbCodec, m_cmbQuality, m_cmbRes, m_cmbAudio, m_cmbPerf, m_chkNextTo, m_chkRecycle})
        EnableWindow(h, !busy);
    UpdateOutputControls();
    if (!busy) UpdateSummary();
}

void MainWindow::OnJobStart(int id) {
    m_currentId = id;
    auto it = m_items.find(id);
    if (it == m_items.end()) return;
    it->second.state = ItemState::Encoding;
    it->second.result = L"Starting…";
    RefreshRow(id);
    int row = FindRow(id);
    if (row >= 0) ListView_EnsureVisible(m_list, row, FALSE);
    SendMessageW(m_prgFile, PBM_SETPOS, 0, 0);
    SetStatus(L"Compressing " + std::to_wstring(m_queueDone + 1) + L" of " + std::to_wstring(m_queueTotal) + L" — " +
              it->second.name);
}

void MainWindow::OnJobProgress(int id, const EncodeProgress& p) {
    auto it = m_items.find(id);
    if (it == m_items.end() || it->second.state != ItemState::Encoding) return;

    wchar_t buf[128];
    if (p.percent >= 0) {
        if (p.etaSeconds >= 0)
            swprintf(buf, 128, L"%.0f%%  ·  %.1f× speed  ·  %ls left", p.percent, p.speed, FormatDuration(p.etaSeconds).c_str());
        else
            swprintf(buf, 128, L"%.0f%%  ·  %.1f× speed", p.percent, p.speed);
    } else {
        swprintf(buf, 128, L"Compressing…  %.1f× speed", p.speed);
    }
    it->second.result = buf;
    RefreshRow(id);

    double frac = p.percent >= 0 ? p.percent / 100.0 : 0;
    SendMessageW(m_prgFile, PBM_SETPOS, (WPARAM)(frac * 1000), 0);
    double total = m_queueTotal > 0 ? (m_queueDone + frac) / m_queueTotal : 0;
    SendMessageW(m_prgTotal, PBM_SETPOS, (WPARAM)(total * 1000), 0);
    SetTaskbarProgress((int)(total * 1000), 1000, true);

    SetStatus(L"Compressing " + std::to_wstring(m_queueDone + 1) + L" of " + std::to_wstring(m_queueTotal) + L" — " +
              it->second.name + L"   " + buf);
}

void MainWindow::OnJobDone(int id, const JobOutcome& o) {
    ++m_queueDone;
    auto it = m_items.find(id);
    if (it != m_items.end()) {
        VideoItem& item = it->second;
        if (o.result.cancelled) {
            item.state = ItemState::Cancelled;
            item.result = L"Cancelled";
            item.outputPath.clear();
        } else if (!o.result.ok) {
            item.state = ItemState::Failed;
            item.result = L"Failed: " + o.result.error;
            item.outputPath.clear();
            ++m_batchFailed;
        } else if (o.noGain) {
            item.state = ItemState::NoGain;
            item.result = L"Already well compressed — original kept";
            item.outputPath.clear();
        } else {
            item.state = ItemState::Done;
            item.outSize = o.outSize;
            item.outputPath = o.output;
            item.result = L"✓ " + FormatSize(item.size) + L" → " + FormatSize(o.outSize) + L"  (" +
                          Percent(item.size, o.outSize) + L")";
            m_batchIn += item.size;
            m_batchOut += o.outSize;
            ++m_batchOk;

            if (m_settings.recycleOriginal) {
                std::wstring from = item.path;
                from.push_back(L'\0');  // double-null terminated
                SHFILEOPSTRUCTW op{};
                op.hwnd = m_hwnd;
                op.wFunc = FO_DELETE;
                op.pFrom = from.c_str();
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
                if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) item.result += L" · original recycled";
            }
        }
        RefreshRow(id);
    }
    SendMessageW(m_prgFile, PBM_SETPOS, 1000, 0);
    SendMessageW(m_prgTotal, PBM_SETPOS, m_queueTotal ? m_queueDone * 1000 / m_queueTotal : 0, 0);
}

void MainWindow::OnQueueDone() {
    if (m_worker.joinable()) m_worker.join();
    m_encoder.reset();
    m_currentId = 0;

    for (auto& [id, item] : m_items)
        if (item.state == ItemState::Queued || item.state == ItemState::Encoding) {
            item.state = ItemState::Cancelled;
            item.result = L"Cancelled";
        }
    InvalidateRect(m_list, nullptr, FALSE);

    SetBusy(false);
    SetTaskbarProgress(0, 0, false);
    SendMessageW(m_prgFile, PBM_SETPOS, 0, 0);

    std::wstring msg;
    if (m_batchOk > 0) {
        msg = L"Done — " + std::to_wstring(m_batchOk) + (m_batchOk == 1 ? L" video: " : L" videos: ") +
              FormatSize(m_batchIn) + L" → " + FormatSize(m_batchOut) + L"   (saved " +
              FormatSize(m_batchIn - m_batchOut) + L", " + Percent(m_batchIn, m_batchOut) + L")";
    } else {
        msg = m_cancelRequested ? L"Compression cancelled." : L"Finished.";
    }
    if (m_batchFailed > 0) msg += L"   ·   " + std::to_wstring(m_batchFailed) + L" failed";
    SetStatus(msg);

    if (GetForegroundWindow() != m_hwnd) {
        FLASHWINFO fi{sizeof(fi), m_hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&fi);
    }
    MessageBeep(MB_OK);
}

void MainWindow::UpdateSummary() {
    if (m_busy) return;
    uint64_t total = 0, checkedSize = 0;
    int shown = ListView_GetItemCount(m_list), checked = 0;
    for (int row = 0; row < shown; ++row) {
        LVITEMW lv{};
        lv.mask = LVIF_PARAM;
        lv.iItem = row;
        ListView_GetItem(m_list, &lv);
        const VideoItem& it = m_items.at((int)lv.lParam);
        total += it.size;
        if (it.checked) {
            ++checked;
            checkedSize += it.size;
        }
    }
    std::wstring s = std::to_wstring(shown) + (shown == 1 ? L" video" : L" videos") + L"  ·  " + FormatSize(total);
    if (checked > 0) s += L"      |      " + std::to_wstring(checked) + L" selected  ·  " + FormatSize(checkedSize);
    else if (shown > 0) s += L"      |      Tick videos to compress, or drag & drop any video here";
    SetStatus(s);
    SetWindowTextW(m_btnCheckAll, checked == shown && shown > 0 ? L"Select none" : L"Select all");
}

void MainWindow::SetStatus(const std::wstring& text) { SetWindowTextW(m_lblStatus, text.c_str()); }

void MainWindow::SetTaskbarProgress(int done, int total, bool active) {
    if (!m_taskbar) return;
    if (!active) {
        m_taskbar->SetProgressState(m_hwnd, TBPF_NOPROGRESS);
        return;
    }
    m_taskbar->SetProgressState(m_hwnd, TBPF_NORMAL);
    m_taskbar->SetProgressValue(m_hwnd, done, total);
}

}  // namespace nv
