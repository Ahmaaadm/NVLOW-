#include "VideoScanner.h"

#include <shlobj.h>

#include <functional>

#include "Utils.h"

namespace nv {

bool IsVideoFile(const std::wstring& path) {
    static const wchar_t* exts[] = {L".mp4", L".mkv", L".mov", L".avi", L".webm", L".m4v",
                                    L".flv", L".wmv", L".ts",  L".m2ts", L".mpg", L".mpeg"};
    std::wstring lower = ToLower(path);
    for (const wchar_t* e : exts) {
        size_t n = wcslen(e);
        if (lower.size() > n && lower.compare(lower.size() - n, n, e) == 0) return true;
    }
    return false;
}

bool IsNvlowOutput(const std::wstring& path) {
    return ToLower(StemOf(path)).ends_with(L"_nvlow");
}

static std::wstring RegistryCapturePath() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\NVIDIA Corporation\\Global\\ShadowPlay\\NVSPCAPS", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    std::wstring out;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, L"DefaultPathW", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS) out = buf;
    RegCloseKey(key);
    while (!out.empty() && (out.back() == L'\\' || out.back() == L' ')) out.pop_back();
    return out;
}

std::wstring DetectNvidiaFolder() {
    std::wstring reg = RegistryCapturePath();
    if (!reg.empty()) {
        if (DirExists(reg + L"\\NVIDIA")) return reg + L"\\NVIDIA";
        if (DirExists(reg)) return reg;
    }
    std::wstring videos = KnownFolder(FOLDERID_Videos) + L"\\NVIDIA";
    return DirExists(videos) ? videos : std::wstring();
}

static void Walk(const std::wstring& dir, int depth, const std::function<void(const std::wstring&, const WIN32_FIND_DATAW&)>& fn) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) Walk(full, depth - 1, fn);
        } else {
            fn(full, fd);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<FoundVideo> ScanLibraryFolder(const std::wstring& root) {
    std::vector<FoundVideo> out;
    if (root.empty() || !DirExists(root)) return out;

    const std::wstring rootName = FileNameOf(root);
    const size_t prefix = root.size() + 1;
    Walk(root, 3, [&](const std::wstring& path, const WIN32_FIND_DATAW& fd) {
        if (!IsVideoFile(path) || IsNvlowOutput(path)) return;
        FoundVideo v;
        v.path = path;
        std::wstring rel = path.substr(prefix);
        size_t sep = rel.find(L'\\');
        v.game = sep == std::wstring::npos ? rootName : rel.substr(0, sep);
        v.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        v.modified = fd.ftLastWriteTime;
        out.push_back(std::move(v));
    });
    return out;
}

std::vector<std::wstring> FindVideosIn(const std::wstring& folder) {
    std::vector<std::wstring> out;
    Walk(folder, 8, [&](const std::wstring& path, const WIN32_FIND_DATAW&) {
        if (IsVideoFile(path) && !IsNvlowOutput(path)) out.push_back(path);
    });
    return out;
}

}  // namespace nv
