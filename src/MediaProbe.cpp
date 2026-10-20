#include "MediaProbe.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "Utils.h"

using Microsoft::WRL::ComPtr;

namespace nv {

static std::wstring CodecName(const GUID& sub) {
    if (sub == MFVideoFormat_H264 || sub == MFVideoFormat_H264_ES) return L"H.264";
    if (sub == MFVideoFormat_HEVC || sub == MFVideoFormat_HEVC_ES) return L"HEVC";
    if (sub == MFVideoFormat_AV1) return L"AV1";
    if (sub == MFVideoFormat_VP90) return L"VP9";
    if (sub == MFVideoFormat_MPEG2) return L"MPEG-2";
    if (sub == MFVideoFormat_MP4V) return L"MPEG-4";
    if (sub == MFVideoFormat_MJPG) return L"MJPEG";
    if (sub == MFVideoFormat_WMV3 || sub == MFVideoFormat_WVC1) return L"WMV";
    // FourCC subtypes keep the code in Data1.
    wchar_t cc[5] = {(wchar_t)(sub.Data1 & 0xFF), (wchar_t)((sub.Data1 >> 8) & 0xFF), (wchar_t)((sub.Data1 >> 16) & 0xFF),
                     (wchar_t)((sub.Data1 >> 24) & 0xFF), 0};
    for (wchar_t& c : cc)
        if (c && (c < 32 || c > 126)) return L"Video";
    return cc;
}

MediaInfo ProbeMedia(const std::wstring& file) {
    MediaInfo info;
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader))) return info;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)))
        info.duration = pv.uhVal.QuadPart / 1e7;
    PropVariantClear(&pv);

    bool haveVideo = false;
    for (DWORD s = 0;; ++s) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(s, 0, &type))) break;
        GUID major{}, sub{};
        type->GetMajorType(&major);
        type->GetGUID(MF_MT_SUBTYPE, &sub);
        if (major == MFMediaType_Video && !haveVideo) {
            haveVideo = true;
            UINT32 w = 0, h = 0, fn = 0, fd = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &fn, &fd);
            info.width = (int)w;
            info.height = (int)h;
            info.fps = fd ? (double)fn / fd : 0;
            info.videoCodec = CodecName(sub);
        } else if (major == MFMediaType_Audio) {
            ++info.audioTracks;
        }
    }

    uint64_t size = 0;
    FILETIME ft;
    if (info.duration > 0 && GetFileInfo(file, size, ft)) info.bitrate = size * 8.0 / info.duration;
    info.ok = haveVideo && info.duration > 0;
    return info;
}

void ProbeQueue::Start(Callback cb) {
    m_callback = std::move(cb);
    m_thread = std::thread(&ProbeQueue::Loop, this);
}

void ProbeQueue::Push(int id, std::wstring path) {
    {
        std::lock_guard lock(m_mutex);
        m_queue.emplace_back(id, std::move(path));
    }
    m_cv.notify_one();
}

void ProbeQueue::Stop() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        m_queue.clear();
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void ProbeQueue::Loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::pair<int, std::wstring> job;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
            if (m_stop) break;
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        MediaInfo info = ProbeMedia(job.second);
        if (m_callback) m_callback(job.first, info);
    }
    CoUninitialize();
}

}  // namespace nv
