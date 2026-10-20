#include "Settings.h"

#include <shlobj.h>

#include <algorithm>

#include "Utils.h"

namespace nv {

static const wchar_t* kSection = L"NVLOW";

static std::wstring IniPath() {
    std::wstring dir = KnownFolder(FOLDERID_RoamingAppData) + L"\\NVLOW";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

static int ReadInt(const std::wstring& ini, const wchar_t* key, int def, int lo, int hi) {
    int v = (int)GetPrivateProfileIntW(kSection, key, def, ini.c_str());
    return std::clamp(v, lo, hi);
}

static std::wstring ReadStr(const std::wstring& ini, const wchar_t* key) {
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(kSection, key, L"", buf, (DWORD)std::size(buf), ini.c_str());
    return buf;
}

static void WriteInt(const std::wstring& ini, const wchar_t* key, int v) {
    WritePrivateProfileStringW(kSection, key, std::to_wstring(v).c_str(), ini.c_str());
}

std::wstring DefaultOutputDir() { return KnownFolder(FOLDERID_Videos) + L"\\NVLOW"; }

AppSettings LoadSettings() {
    const std::wstring ini = IniPath();
    AppSettings s;
    s.encode.codec = (Codec)ReadInt(ini, L"Codec", 0, 0, (int)Codec::Count - 1);
    s.codecAuto = ReadInt(ini, L"CodecAuto", 1, 0, 1) != 0;
    s.encode.quality = (Quality)ReadInt(ini, L"Quality", 1, 0, (int)Quality::Count - 1);
    s.encode.resolution = (TargetRes)ReadInt(ini, L"Resolution", 0, 0, (int)TargetRes::Count - 1);
    s.encode.audio = (AudioMode)ReadInt(ini, L"Audio", 0, 0, (int)AudioMode::Count - 1);
    s.encode.performance = (Performance)ReadInt(ini, L"Performance", (int)Performance::Medium, 0, (int)Performance::Count - 1);
    s.gameFilter = ReadStr(ini, L"GameFilter");
    s.skippedVersion = ReadStr(ini, L"SkippedVersion");
    s.sortColumn = ReadInt(ini, L"SortColumn", 2, 0, 7);
    s.sortAscending = ReadInt(ini, L"SortAscending", 0, 0, 1) != 0;
    s.libraryRoot = ReadStr(ini, L"LibraryRoot");
    s.outputDir = ReadStr(ini, L"OutputDir");
    s.saveNextToOriginal = ReadInt(ini, L"SaveNextToOriginal", 0, 0, 1) != 0;
    s.recycleOriginal = ReadInt(ini, L"RecycleOriginal", 0, 0, 1) != 0;
    s.windowWidth = ReadInt(ini, L"WindowWidth", 1240, 900, 8000);
    s.windowHeight = ReadInt(ini, L"WindowHeight", 800, 620, 8000);
    s.maximized = ReadInt(ini, L"Maximized", 0, 0, 1) != 0;
    if (s.outputDir.empty()) s.outputDir = DefaultOutputDir();
    return s;
}

void SaveSettings(const AppSettings& s) {
    const std::wstring ini = IniPath();
    WriteInt(ini, L"Codec", (int)s.encode.codec);
    WriteInt(ini, L"CodecAuto", s.codecAuto);
    WriteInt(ini, L"Quality", (int)s.encode.quality);
    WriteInt(ini, L"Resolution", (int)s.encode.resolution);
    WriteInt(ini, L"Audio", (int)s.encode.audio);
    WriteInt(ini, L"Performance", (int)s.encode.performance);
    WritePrivateProfileStringW(kSection, L"GameFilter", s.gameFilter.c_str(), ini.c_str());
    WritePrivateProfileStringW(kSection, L"SkippedVersion", s.skippedVersion.c_str(), ini.c_str());
    WriteInt(ini, L"SortColumn", s.sortColumn);
    WriteInt(ini, L"SortAscending", s.sortAscending);
    WritePrivateProfileStringW(kSection, L"LibraryRoot", s.libraryRoot.c_str(), ini.c_str());
    WritePrivateProfileStringW(kSection, L"OutputDir", s.outputDir.c_str(), ini.c_str());
    WriteInt(ini, L"SaveNextToOriginal", s.saveNextToOriginal);
    WriteInt(ini, L"RecycleOriginal", s.recycleOriginal);
    WriteInt(ini, L"WindowWidth", s.windowWidth);
    WriteInt(ini, L"WindowHeight", s.windowHeight);
    WriteInt(ini, L"Maximized", s.maximized);
}

}  // namespace nv
