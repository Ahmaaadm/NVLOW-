#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nv {

struct FoundVideo {
    std::wstring path;
    std::wstring game;  // NVIDIA per-game sub-folder name
    uint64_t size = 0;
    FILETIME modified{};
};

// Where NVIDIA (ShadowPlay / NVIDIA App) saves recordings: registry setting, then Videos\NVIDIA.
std::wstring DetectNvidiaFolder();

// Lists recordings in root (and its per-game sub-folders).
std::vector<FoundVideo> ScanLibraryFolder(const std::wstring& root);

// Recursively lists any video files inside a folder.
std::vector<std::wstring> FindVideosIn(const std::wstring& folder);

bool IsVideoFile(const std::wstring& path);
bool IsNvlowOutput(const std::wstring& path);

}  // namespace nv
