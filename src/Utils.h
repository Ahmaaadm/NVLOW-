#pragma once

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <string>

namespace nv {

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);
std::wstring ToLower(std::wstring s);

std::wstring FormatSize(uint64_t bytes);
std::wstring FormatDuration(double seconds);
std::wstring FormatBitrate(double bitsPerSecond);
std::wstring FormatDate(const FILETIME& ft);

// Quotes a single argument following the MSVCRT command-line parsing rules.
std::wstring QuoteArg(const std::wstring& arg);

std::wstring ExeDirectory();
std::wstring KnownFolder(REFKNOWNFOLDERID id);
std::wstring FileNameOf(const std::wstring& path);
std::wstring ParentOf(const std::wstring& path);
std::wstring StemOf(const std::wstring& path);

bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool GetFileInfo(const std::wstring& path, uint64_t& size, FILETIME& modified);

// True if a Microsoft Store package whose name starts with `packageName` is installed for this user
// (e.g. L"Microsoft.AV1VideoExtension").
bool IsStorePackageInstalled(const std::wstring& packageName);


}  // namespace nv
