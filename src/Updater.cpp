#include "Updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <map>
#include <memory>
#include <vector>

#include "Utils.h"

namespace nv {

namespace {

// ---- Minimal JSON reader (enough for the GitHub releases API) ----

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::string str;
    double num = 0;
    bool b = false;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json& operator[](const char* key) const {
        static const Json none;
        auto it = fields.find(key);
        return it == fields.end() ? none : it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : m_s(s) {}
    bool Parse(Json& out) { return Value(out, 0); }

private:
    void Ws() {
        while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\n' || m_s[m_i] == '\r' || m_s[m_i] == '\t')) ++m_i;
    }
    bool Lit(const char* w) {
        size_t n = strlen(w);
        if (m_s.compare(m_i, n, w) != 0) return false;
        m_i += n;
        return true;
    }
    static void Utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool Str(std::string& o) {
        if (m_s[m_i] != '"') return false;
        ++m_i;
        while (m_i < m_s.size()) {
            char c = m_s[m_i++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (m_i >= m_s.size()) return false;
            char e = m_s[m_i++];
            switch (e) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u':
                    if (m_i + 4 > m_s.size()) return false;
                    Utf8(o, (unsigned)strtoul(m_s.substr(m_i, 4).c_str(), nullptr, 16));
                    m_i += 4;
                    break;
                default: o += e;
            }
        }
        return false;
    }
    bool Value(Json& v, int depth) {
        if (depth > 64) return false;
        Ws();
        if (m_i >= m_s.size()) return false;
        char c = m_s[m_i];
        if (c == '{') {
            v.type = Json::Object;
            ++m_i;
            Ws();
            if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
            for (;;) {
                Ws();
                std::string key;
                if (m_i >= m_s.size() || !Str(key)) return false;
                Ws();
                if (m_i >= m_s.size() || m_s[m_i++] != ':') return false;
                if (!Value(v.fields[key], depth + 1)) return false;
                Ws();
                if (m_i >= m_s.size()) return false;
                if (m_s[m_i] == ',') { ++m_i; continue; }
                if (m_s[m_i] == '}') { ++m_i; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            ++m_i;
            Ws();
            if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
            for (;;) {
                v.items.emplace_back();
                if (!Value(v.items.back(), depth + 1)) return false;
                Ws();
                if (m_i >= m_s.size()) return false;
                if (m_s[m_i] == ',') { ++m_i; continue; }
                if (m_s[m_i] == ']') { ++m_i; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Json::String; return Str(v.str); }
        if (Lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (Lit("false")) { v.type = Json::Bool; return true; }
        if (Lit("null")) return true;
        char* end = nullptr;
        v.num = strtod(m_s.c_str() + m_i, &end);
        if (end == m_s.c_str() + m_i) return false;
        v.type = Json::Number;
        m_i = end - m_s.c_str();
        return true;
    }

    const std::string& m_s;
    size_t m_i = 0;
};

// ---- WinHTTP helpers ----

struct HInternetDeleter {
    void operator()(void* h) const { if (h) WinHttpCloseHandle(h); }
};
using HInternet = std::unique_ptr<void, HInternetDeleter>;

struct Request {
    HInternet session, connect, request;
    DWORD status = 0;
    uint64_t length = 0;
};

bool Open(const std::wstring& url, const wchar_t* accept, Request& r, std::wstring& error) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"Invalid update URL.";
        return false;
    }
    const std::wstring agent = std::wstring(L"NVLOW/") + NVLOW_VERSION;
    r.session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0));
    if (!r.session) r.session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                                WINHTTP_NO_PROXY_BYPASS, 0));
    if (!r.session) { error = L"No internet access."; return false; }
    WinHttpSetTimeouts(r.session.get(), 8000, 8000, 15000, 30000);
    r.connect.reset(WinHttpConnect(r.session.get(), host, uc.nPort, 0));
    if (!r.connect) { error = L"Can't reach the update server."; return false; }
    r.request.reset(WinHttpOpenRequest(r.connect.get(), L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!r.request) { error = L"Can't reach the update server."; return false; }
    std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(r.request.get(), headers.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(r.request.get(), nullptr)) {
        error = L"No internet connection.";
        return false;
    }
    DWORD size = sizeof(r.status);
    WinHttpQueryHeaders(r.request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &r.status,
                        &size, nullptr);
    wchar_t len[32] = {};
    size = sizeof(len);
    if (WinHttpQueryHeaders(r.request.get(), WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &size, nullptr))
        r.length = _wcstoui64(len, nullptr, 10);
    return true;
}

template <class Sink>
bool ReadBody(Request& r, Sink&& sink) {
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(r.request.get(), buf.data(), (DWORD)buf.size(), &got)) return false;
        if (got == 0) return true;
        if (!sink(buf.data(), got)) return false;
    }
}

std::wstring ToHex(const unsigned char* p, size_t n) {
    static const wchar_t* d = L"0123456789abcdef";
    std::wstring s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

}  // namespace

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    auto parts = [](const std::wstring& v) {
        std::vector<long> out;
        size_t i = (!v.empty() && (v[0] == L'v' || v[0] == L'V')) ? 1 : 0;
        while (i < v.size() && iswdigit(v[i])) {
            long n = 0;
            while (i < v.size() && iswdigit(v[i])) n = n * 10 + (v[i++] - L'0');
            out.push_back(n);
            if (i < v.size() && v[i] == L'.') ++i;
            else break;
        }
        while (out.size() < 3) out.push_back(0);
        return out;
    };
    auto pa = parts(a), pb = parts(b);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

UpdateInfo CheckForUpdate(const std::wstring& repo, const std::wstring& currentVersion) {
    UpdateInfo info;
    if (repo.empty()) {
        info.error = L"Updates are not configured.";
        return info;
    }
    Request r;
    if (!Open(L"https://api.github.com/repos/" + repo + L"/releases/latest", L"application/vnd.github+json", r,
              info.error))
        return info;
    if (r.status == 404) {  // no release published yet
        info.checked = true;
        return info;
    }
    if (r.status != 200) {
        info.error = L"Update server answered " + std::to_wstring(r.status) + L".";
        return info;
    }
    std::string body;
    if (!ReadBody(r, [&](const char* p, DWORD n) { body.append(p, n); return body.size() < 4 * 1024 * 1024; })) {
        info.error = L"Download of release info failed.";
        return info;
    }
    Json root;
    if (!JsonParser(body).Parse(root) || root.type != Json::Object) {
        info.error = L"Unexpected answer from the update server.";
        return info;
    }

    info.checked = true;
    std::wstring tag = Widen(root["tag_name"].str);
    if (!tag.empty() && (tag[0] == L'v' || tag[0] == L'V')) tag.erase(0, 1);
    info.version = tag;
    info.pageUrl = Widen(root["html_url"].str);
    if (tag.empty() || root["draft"].b || root["prerelease"].b) return info;

    // Pick the installer asset: NVLOW-Setup-*.exe
    for (const Json& a : root["assets"].items) {
        std::wstring name = Widen(a["name"].str);
        std::wstring lower = ToLower(name);
        if (!lower.starts_with(L"nvlow-setup") || !lower.ends_with(L".exe")) continue;
        info.installerName = name;
        info.installerUrl = Widen(a["browser_download_url"].str);
        info.installerSize = (uint64_t)a["size"].num;
        std::wstring digest = ToLower(Widen(a["digest"].str));
        if (digest.starts_with(L"sha256:")) info.sha256 = digest.substr(7);
        break;
    }
    info.available = CompareVersions(tag, currentVersion) > 0 && !info.installerUrl.empty();
    return info;
}

std::wstring DownloadUpdate(const UpdateInfo& info, const std::function<void(double)>& onProgress,
                            const std::atomic<bool>& cancel, std::wstring& error) {
    // Only download from GitHub over HTTPS.
    if (!ToLower(info.installerUrl).starts_with(L"https://github.com/")) {
        error = L"Unexpected download location.";
        return {};
    }
    Request r;
    if (!Open(info.installerUrl, L"application/octet-stream", r, error)) return {};
    if (r.status != 200) {
        error = L"Download failed (HTTP " + std::to_wstring(r.status) + L").";
        return {};
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"NVLOW-Update";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\" + FileNameOf(info.installerName);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Can't save the update.";
        return {};
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (alg) BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);

    const uint64_t total = r.length ? r.length : info.installerSize;
    uint64_t done = 0;
    bool ok = ReadBody(r, [&](const char* p, DWORD n) {
        if (cancel) return false;
        DWORD written = 0;
        if (!WriteFile(file, p, n, &written, nullptr) || written != n) return false;
        if (hash) BCryptHashData(hash, (PUCHAR)p, n, 0);
        done += n;
        if (onProgress && total) onProgress((double)done / (double)total);
        return true;
    });
    CloseHandle(file);

    unsigned char digest[32] = {};
    if (hash) {
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        BCryptDestroyHash(hash);
    }
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);

    if (!ok || cancel) {
        error = cancel ? L"Cancelled." : L"Download interrupted.";
    } else if (info.installerSize && done != info.installerSize) {
        error = L"The download is incomplete.";
    } else if (!info.sha256.empty() && ToHex(digest, sizeof(digest)) != info.sha256) {
        error = L"The download is damaged (checksum mismatch).";
    } else {
        return path;
    }
    DeleteFileW(path.c_str());
    return {};
}

}  // namespace nv
