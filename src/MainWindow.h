#pragma once

#include <windows.h>
#include <shobjidl.h>

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Encoder.h"
#include "MediaProbe.h"
#include "Settings.h"
#include "Updater.h"

namespace nv {

enum class ItemState { Analyzing, Ready, Queued, Encoding, Done, NoGain, Failed, Cancelled };

struct VideoItem {
    int id = 0;
    std::wstring path;
    std::wstring name;
    std::wstring game;
    bool imported = false;  // added by the user, not found in the NVIDIA library
    uint64_t size = 0;
    FILETIME modified{};
    MediaInfo info;
    ItemState state = ItemState::Analyzing;
    std::wstring result;
    uint64_t outSize = 0;
    std::wstring outputPath;
    bool checked = false;
};

struct QueuedJob {
    int id = 0;
    EncodeJob job;
    uint64_t inputSize = 0;
};

struct JobOutcome {
    EncodeResult result;
    std::wstring output;
    uint64_t outSize = 0;
    bool noGain = false;
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE instance, int showCmd);
    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    // Window & layout
    void OnCreate();
    bool OnClose();
    void OnDestroy();
    void CreateControls();
    void CreateFonts();
    void ApplyFonts();
    void Layout();
    void Paint();
    int S(int v) const { return MulDiv(v, m_dpi, 96); }
    int BannerHeight() const { return S(72); }
    int UpdateBarHeight() const { return m_updateBarVisible ? S(46) : 0; }

    // Updates
    void StartUpdateCheck(bool manual);
    void OnUpdateInfo(const UpdateInfo& info);
    void ShowUpdateBar(bool show);
    void OnUpdateButton();
    void OnUpdateDownloaded(const std::wstring& path, const std::wstring& error);
    void ShowAbout();
    bool IsInstalledCopy() const;
    HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0);

    // Events
    void OnCommand(int id, int code);
    LRESULT OnNotify(NMHDR* hdr);
    void OnContextMenu(int x, int y);
    void OnDropFiles(HDROP drop);

    // Library & list
    void ScanLibrary();
    void AddPaths(const std::vector<std::wstring>& paths);
    void AddItem(const std::wstring& path, const std::wstring& game, bool imported, uint64_t size, FILETIME modified);
    void RebuildGameCombo();
    void RebuildList();
    void SortList();
    void UpdateSortArrows();
    std::wstring CurrentGameFilter() const;
    int FindRow(int id) const;
    void RefreshRow(int id);
    std::wstring ColumnText(const VideoItem& item, int column) const;
    static int CALLBACK CompareItems(LPARAM a, LPARAM b, LPARAM self);
    std::vector<int> SelectedIds() const;
    std::vector<int> TargetIds() const;
    void SetAllChecked(bool checked);
    void RemoveSelected();
    void OpenPath(const std::wstring& path, bool select);
    void MarkExistingOutputs();

    // Settings
    void LoadSettingsIntoControls();
    void ReadSettingsFromControls();
    void UpdateOutputControls();
    void RebuildCodecCombo();
    std::wstring OutputPathFor(const VideoItem& item) const;
    void ChooseLibrary();
    void ChooseOutputDir();
    void AddFilesDialog();
    std::wstring PickFolder(const wchar_t* title, const std::wstring& initial);

    // Compression
    void DetectEncoders();
    void OnEncoderCaps(const EncoderCaps& caps);
    void StartCompression();
    void CancelCompression();
    void WorkerMain(std::vector<QueuedJob> jobs);
    void SetBusy(bool busy);
    void OnJobStart(int id);
    void OnJobProgress(int id, const EncodeProgress& p);
    void OnJobDone(int id, const JobOutcome& outcome);
    void OnQueueDone();
    void UpdateSummary();
    void SetStatus(const std::wstring& text);
    void SetTaskbarProgress(int done, int total, bool active);

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;

    HFONT m_font = nullptr;
    HFONT m_fontBold = nullptr;
    HFONT m_fontTitle = nullptr;
    HFONT m_fontSubtitle = nullptr;

    // Controls
    HWND m_lblGame = nullptr, m_cmbGame = nullptr, m_btnCheckAll = nullptr, m_btnRemove = nullptr;
    HWND m_btnAdd = nullptr, m_btnLibrary = nullptr, m_btnRefresh = nullptr;
    HWND m_list = nullptr;
    HWND m_grpSettings = nullptr;
    HWND m_lblCodec = nullptr, m_cmbCodec = nullptr, m_lblQuality = nullptr, m_cmbQuality = nullptr;
    HWND m_lblRes = nullptr, m_cmbRes = nullptr, m_lblAudio = nullptr, m_cmbAudio = nullptr;
    HWND m_lblPerf = nullptr, m_cmbPerf = nullptr;
    HWND m_lblOutput = nullptr, m_edtOutput = nullptr, m_btnBrowse = nullptr;
    HWND m_chkNextTo = nullptr, m_chkRecycle = nullptr;
    HWND m_lblStatus = nullptr, m_lblFile = nullptr, m_prgFile = nullptr, m_lblTotal = nullptr, m_prgTotal = nullptr;
    HWND m_btnStart = nullptr, m_btnCancel = nullptr;

    // Data
    AppSettings m_settings;
    std::wstring m_libraryRoot;
    std::map<int, VideoItem> m_items;
    std::set<std::wstring> m_knownPaths;  // lower-cased
    std::vector<std::wstring> m_gameFilters;
    int m_nextId = 1;
    bool m_populating = false;
    int m_sortColumn = 2;  // date
    bool m_sortAscending = false;

    // Encoders
    unsigned m_caps = 0;  // bit per Codec
    bool m_capsKnown = false;
    bool m_av1Playable = false;   // Windows "AV1 Video Extension" installed
    bool m_hevcPlayable = false;  // Windows "HEVC Video Extension" installed
    std::wstring m_encoderText = L"Detecting GPU encoders…";
    std::thread m_capsThread;

    // Work
    ProbeQueue m_probe;
    std::unique_ptr<Encoder> m_encoder;
    std::thread m_worker;
    bool m_busy = false;
    bool m_cancelRequested = false;
    int m_queueTotal = 0;
    int m_queueDone = 0;
    int m_currentId = 0;
    uint64_t m_batchIn = 0, m_batchOut = 0;
    int m_batchOk = 0, m_batchFailed = 0;

    // Update bar
    HWND m_lblUpdate = nullptr, m_btnUpdate = nullptr, m_btnNotes = nullptr, m_btnSkip = nullptr;
    HBRUSH m_updateBrush = nullptr;
    UpdateInfo m_update;
    bool m_updateBarVisible = false;
    bool m_updateBusy = false;     // checking or downloading
    bool m_manualCheck = false;
    std::atomic<bool> m_updateCancel{false};
    std::thread m_updateThread;

    UINT m_taskbarCreatedMsg = 0;
    ITaskbarList3* m_taskbar = nullptr;
};

}  // namespace nv
