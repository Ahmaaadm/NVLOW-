#pragma once

#include <string>

#include "Encoder.h"

namespace nv {

struct AppSettings {
    EncodeOptions encode;
    bool codecAuto = true;          // pick the best encoder this PC can both encode and play
    std::wstring libraryRoot;       // empty = auto-detect NVIDIA folder
    std::wstring outputDir;         // empty = Videos\NVLOW
    bool saveNextToOriginal = false;
    bool recycleOriginal = false;
    std::wstring gameFilter;        // last selected game ("" = all games)
    std::wstring skippedVersion;    // update the user chose to skip
    int sortColumn = 2;             // recorded date
    bool sortAscending = false;
    int windowWidth = 1240;         // 96-DPI units
    int windowHeight = 800;
    bool maximized = false;
};

AppSettings LoadSettings();
void SaveSettings(const AppSettings& s);
std::wstring DefaultOutputDir();

}  // namespace nv
