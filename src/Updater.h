#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace nv {

struct UpdateInfo {
    bool checked = false;     // the request succeeded
    bool available = false;   // a newer version exists
    std::wstring version;     // "1.2.0"
    std::wstring pageUrl;     // release page (notes)
    std::wstring installerUrl;
    std::wstring installerName;
    uint64_t installerSize = 0;
    std::wstring sha256;      // lower-case hex, from GitHub's asset digest (may be empty)
    std::wstring error;
};

// Asks GitHub for the latest release of `repo` ("owner/name") and compares it with `currentVersion`.
UpdateInfo CheckForUpdate(const std::wstring& repo, const std::wstring& currentVersion);

// Downloads the installer to %TEMP%, verifies size and SHA-256. Returns the local path, or "" on failure.
std::wstring DownloadUpdate(const UpdateInfo& info, const std::function<void(double fraction)>& onProgress,
                            const std::atomic<bool>& cancel, std::wstring& error);

// -1 / 0 / 1 like strcmp, for "1.2.10" style versions (a leading "v" is ignored).
int CompareVersions(const std::wstring& a, const std::wstring& b);

}  // namespace nv
