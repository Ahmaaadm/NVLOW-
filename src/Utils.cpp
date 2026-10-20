#include "Utils.h"

#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace nv {

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return s;
}

std::wstring FormatSize(uint64_t bytes) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[32];
    if (u == 0)
        swprintf(buf, 32, L"%.0f %ls", v, units[u]);
    else
        swprintf(buf, 32, v < 10.0 ? L"%.2f %ls" : L"%.1f %ls", v, units[u]);
    return buf;
}

std::wstring FormatDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    auto total = (long long)(seconds + 0.5);
    long long h = total / 3600, m = (total / 60) % 60, s = total % 60;
    wchar_t buf[32];
    if (h > 0)
        swprintf(buf, 32, L"%lld:%02lld:%02lld", h, m, s);
    else
        swprintf(buf, 32, L"%lld:%02lld", m, s);
    return buf;
}

std::wstring FormatBitrate(double bps) {
    wchar_t buf[32];
    if (bps >= 1'000'000)
        swprintf(buf, 32, L"%.1f Mbps", bps / 1'000'000.0);
    else
        swprintf(buf, 32, L"%.0f kbps", bps / 1000.0);
    return buf;
}

std::wstring FormatDate(const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return {};
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u-%02u-%02u  %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += c;
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

std::wstring ExeDirectory() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    return ParentOf(std::wstring(buf, n));
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring ParentOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring StemOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool GetFileInfo(const std::wstring& path, uint64_t& size, FILETIME& modified) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    modified = d.ftLastWriteTime;
    return true;
}

bool IsStorePackageInstalled(const std::wstring& packageName) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\"
                      L"Repository\\Packages",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    const std::wstring prefix = ToLower(packageName) + L"_";
    bool found = false;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD len = (DWORD)std::size(name);
        if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (ToLower(name).starts_with(prefix)) {
            found = true;
            break;
        }
    }
    RegCloseKey(key);
    return found;
}



}  // namespace nv
