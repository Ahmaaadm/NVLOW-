#include "Encoder.h"

#include <windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "Utils.h"

using Microsoft::WRL::ComPtr;

namespace nv {

namespace {

const GUID& SubtypeFor(Codec c) {
    switch (c) {
        case Codec::Av1Gpu: return MFVideoFormat_AV1;
        case Codec::HevcGpu:
        case Codec::HevcCpu: return MFVideoFormat_HEVC;
        default: return MFVideoFormat_H264;
    }
}

// Target bitrate (Mbps) for 1920x1200 @ 60 fps, indexed by Quality.
// Calibrated with VMAF on CS2 gameplay (27 Mbps H.264 source) on an RTX 5070 Laptop GPU:
//   Visually lossless ≈ 97.5, High ≈ 95.5, Balanced ≈ 93, Smallest ≈ 90.
double ReferenceMbps(Codec codec, Quality q) {
    static const double av1[] = {12.0, 8.5, 6.0, 4.5};
    static const double hevc[] = {15.0, 10.0, 7.5, 5.5};
    static const double h264[] = {16.0, 12.0, 9.5, 7.0};
    switch (codec) {
        case Codec::Av1Gpu: return av1[(int)q];
        case Codec::HevcGpu:
        case Codec::HevcCpu: return hevc[(int)q];
        default: return h264[(int)q];
    }
}

UINT32 TargetBitrate(const EncodeJob& job, UINT32 w, UINT32 h, double fps) {
    // Bits needed grow slower than pixel count / frame rate, hence the exponents.
    const double pixels = (double)w * h / (1920.0 * 1200.0);
    const double rate = std::clamp(fps, 10.0, 240.0) / 60.0;
    double bps = ReferenceMbps(job.options.codec, job.options.quality) * 1e6 * std::pow(pixels, 0.75) * std::pow(rate, 0.6);
    if (job.sourceBitrate > 0) bps = std::min(bps, job.sourceBitrate * 0.85);  // never larger than the original
    return (UINT32)std::clamp(bps, 300'000.0, 400'000'000.0);
}

// Encoder speed/quality trade-off (0 = fastest, 100 = best). Measured on the NVIDIA HEVC encoder, 2-min 1200p60 clip:
//   100 -> 2.7x realtime, VMAF 95.6 | 50 -> 7.3x, 95.4 | 0 -> 15x, 95.0  (same file size)
UINT32 QualityVsSpeed(Performance p) {
    switch (p) {
        case Performance::High: return 0;
        case Performance::MaxQuality: return 100;
        default: return 50;
    }
}

// Low performance mode caps encoding speed so the GPU/CPU mostly idle (quiet fans, smooth games).
double SpeedCap(Performance p) { return p == Performance::Low ? 2.0 : 0.0; }

UINT32 TargetHeight(TargetRes r) {
    switch (r) {
        case TargetRes::P1440: return 1440;
        case TargetRes::P1080: return 1080;
        case TargetRes::P720: return 720;
        default: return 0;
    }
}

std::wstring HrText(HRESULT hr) {
    switch (hr) {
        case MF_E_UNSUPPORTED_BYTESTREAM_TYPE:
        case MF_E_INVALIDMEDIATYPE:
        case MF_E_TOPO_CODEC_NOT_FOUND: return L"This video format isn't supported by Windows.";
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND): return L"File not found.";
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED): return L"Access denied.";
        case HRESULT_FROM_WIN32(ERROR_DISK_FULL): return L"The disk is full.";
    }
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   (DWORD)hr, 0, (LPWSTR)&msg, 0, nullptr);
    wchar_t code[32];
    swprintf(code, 32, L" (0x%08lX)", (unsigned long)hr);
    std::wstring s = msg ? msg : L"Media Foundation error";
    LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s + code;
}

// Prefers an NVIDIA GPU, then the adapter with the most dedicated video memory.
ComPtr<IDXGIAdapter1> PickAdapter(DXGI_ADAPTER_DESC1* descOut) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    ComPtr<IDXGIAdapter1> best, a;
    DXGI_ADAPTER_DESC1 bestDesc{};
    auto score = [](const DXGI_ADAPTER_DESC1& d) -> double {
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return -1;
        return (d.VendorId == 0x10DE ? 1e15 : 0) + (double)d.DedicatedVideoMemory;
    };
    for (UINT i = 0; factory->EnumAdapters1(i, &a) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        if (score(d) > (best ? score(bestDesc) : -1)) {
            best = a;
            bestDesc = d;
        }
    }
    if (descOut) *descOut = bestDesc;
    return best;
}

struct GpuDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
};

bool CreateGpuDevice(GpuDevice& out) {
    ComPtr<IDXGIAdapter1> adapter = PickAdapter(nullptr);
    if (!adapter) return false;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &out.device, &fl, nullptr)))
        return false;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(out.device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &out.manager))) return false;
    return SUCCEEDED(out.manager->ResetDevice(out.device.Get(), token));
}

bool HasEncoder(const GUID& subtype, bool hardware, const std::wstring& vendorId) {
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, subtype};
    UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT) |
                   MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &acts, &n))) return false;
    bool found = false;
    for (UINT32 i = 0; i < n; ++i) {
        if (!found) {
            if (!hardware || vendorId.empty()) {
                found = true;
            } else {
                wchar_t* v = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &v, &len))) {
                    found = _wcsicmp(v, vendorId.c_str()) == 0;
                    CoTaskMemFree(v);
                }
            }
        }
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    return found;
}

}  // namespace

EncoderCaps DetectEncoders() {
    EncoderCaps caps;
    DXGI_ADAPTER_DESC1 desc{};
    if (PickAdapter(&desc)) {
        caps.gpuName = desc.Description;
        wchar_t vendor[16];
        swprintf(vendor, 16, L"VEN_%04X", desc.VendorId);
        for (Codec c : {Codec::Av1Gpu, Codec::HevcGpu, Codec::H264Gpu})
            if (HasEncoder(SubtypeFor(c), true, vendor)) caps.available |= 1u << (int)c;
    }
    for (Codec c : {Codec::HevcCpu, Codec::H264Cpu})
        if (HasEncoder(SubtypeFor(c), false, L"")) caps.available |= 1u << (int)c;
    return caps;
}

EncodeResult Encoder::Run(const EncodeJob& job, const ProgressFn& onProgress) {
    EncodeResult result;
    if (m_cancel) {
        result.cancelled = true;
        return result;
    }

    const EncodeOptions& opt = job.options;
    const bool gpu = IsGpuCodec(opt.codec);
    HRESULT hr = S_OK;
    std::wstring stage;
    auto fail = [&](const wchar_t* what) {
        result.error = std::wstring(what) + L": " + HrText(hr);
        return result;
    };

    GpuDevice dev;
    if (gpu && !CreateGpuDevice(dev)) {
        result.error = L"Could not start the GPU video engine.";
        return result;
    }

    // ---- Reader (decoder) ----
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 4);
    if (gpu) {
        ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dev.manager.Get());
        ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);  // colour conversion + scaling
    ComPtr<IMFSourceReader> reader;
    if (FAILED(hr = MFCreateSourceReaderFromURL(job.input.c_str(), ra.Get(), &reader))) return fail(L"Can't open the video");

    double duration = job.duration;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)))
        duration = pv.uhVal.QuadPart / 1e7;
    PropVariantClear(&pv);

    // ---- Writer (encoder + MP4 muxer) ----
    ComPtr<IMFAttributes> wa;
    MFCreateAttributes(&wa, 4);
    if (gpu) {
        wa->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dev.manager.Get());
        wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    wa->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    wa->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    ComPtr<IMFSinkWriter> writer;
    if (FAILED(hr = MFCreateSinkWriterFromURL(job.output.c_str(), nullptr, wa.Get(), &writer)))
        return fail(L"Can't create the output file");

    // ---- Streams ----
    std::vector<int> streamMap;  // reader stream -> writer stream (-1 = skip)
    DWORD videoStream = MAXDWORD;
    for (DWORD s = 0;; ++s) {
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(s, 0, &native))) break;
        streamMap.push_back(-1);
        GUID major{};
        native->GetMajorType(&major);

        if (major == MFMediaType_Video && videoStream == MAXDWORD) {
            videoStream = s;
            reader->SetStreamSelection(s, TRUE);
            UINT32 w = 0, h = 0, fn = 60, fd = 1;
            MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fn, &fd);
            if (fd == 0 || fn == 0) fn = 60, fd = 1;
            UINT32 ow = w & ~1u, oh = h & ~1u;
            UINT32 th = TargetHeight(opt.resolution);
            if (th && th < h) {
                oh = th & ~1u;
                ow = ((UINT32)std::lround((double)w * oh / h)) & ~1u;
            }

            ComPtr<IMFMediaType> rt;
            MFCreateMediaType(&rt);
            rt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            rt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            MFSetAttributeSize(rt.Get(), MF_MT_FRAME_SIZE, ow, oh);
            MFSetAttributeRatio(rt.Get(), MF_MT_FRAME_RATE, fn, fd);
            MFSetAttributeRatio(rt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            rt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (FAILED(hr = reader->SetCurrentMediaType(s, nullptr, rt.Get()))) return fail(L"Can't decode the video");
            ComPtr<IMFMediaType> decoded;
            reader->GetCurrentMediaType(s, &decoded);

            const UINT32 bitrate = TargetBitrate(job, ow, oh, (double)fn / fd);
            ComPtr<IMFMediaType> et;
            MFCreateMediaType(&et);
            et->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            et->SetGUID(MF_MT_SUBTYPE, SubtypeFor(opt.codec));
            et->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
            MFSetAttributeSize(et.Get(), MF_MT_FRAME_SIZE, ow, oh);
            MFSetAttributeRatio(et.Get(), MF_MT_FRAME_RATE, fn, fd);
            MFSetAttributeRatio(et.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            et->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            if (SubtypeFor(opt.codec) == MFVideoFormat_H264) et->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
            DWORD out = 0;
            if (FAILED(hr = writer->AddStream(et.Get(), &out))) return fail(L"The encoder rejected the video settings");
            streamMap[s] = (int)out;

            ComPtr<IMFAttributes> enc;
            MFCreateAttributes(&enc, 4);
            enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
            enc->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
            enc->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, QualityVsSpeed(opt.performance));
            enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, std::max(1u, 2 * fn / fd));
            if (FAILED(hr = writer->SetInputMediaType(out, decoded.Get(), enc.Get())))
                return fail(gpu ? L"The GPU encoder couldn't start" : L"The encoder couldn't start");

        } else if (major == MFMediaType_Audio && opt.audio != AudioMode::Remove) {
            reader->SetStreamSelection(s, TRUE);
            bool ok = false;
            DWORD out = 0;
            if (opt.audio == AudioMode::Copy) {
                // Bit-exact passthrough (works for AAC, the NVIDIA default).
                ok = SUCCEEDED(reader->SetCurrentMediaType(s, nullptr, native.Get())) &&
                     SUCCEEDED(writer->AddStream(native.Get(), &out)) &&
                     SUCCEEDED(writer->SetInputMediaType(out, native.Get(), nullptr));
            }
            if (!ok) {
                // Re-encode to AAC: decode to 16-bit PCM, let the writer's AAC encoder compress it.
                UINT32 rate = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                UINT32 ch = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
                rate = rate == 44100 ? 44100 : 48000;
                ch = ch >= 6 ? 6 : (ch == 1 ? 1 : 2);
                ComPtr<IMFMediaType> pcm;
                MFCreateMediaType(&pcm);
                pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
                ComPtr<IMFMediaType> aac;
                MFCreateMediaType(&aac);
                aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
                aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, opt.audio == AudioMode::Aac128 ? 16000 : 24000);
                ComPtr<IMFMediaType> decoded;
                ok = SUCCEEDED(reader->SetCurrentMediaType(s, nullptr, pcm.Get())) &&
                     SUCCEEDED(reader->GetCurrentMediaType(s, &decoded)) &&
                     SUCCEEDED(writer->AddStream(aac.Get(), &out)) &&
                     SUCCEEDED(writer->SetInputMediaType(out, decoded.Get(), nullptr));
            }
            if (ok)
                streamMap[s] = (int)out;
            else
                reader->SetStreamSelection(s, FALSE);  // unsupported audio track: drop it rather than fail
        } else {
            reader->SetStreamSelection(s, FALSE);
        }
    }
    if (videoStream == MAXDWORD) {
        result.error = L"No video track found.";
        return result;
    }

    if (FAILED(hr = writer->BeginWriting())) return fail(L"Can't start writing");

    // ---- Pump ----
    std::vector<bool> ended(streamMap.size(), false);
    for (size_t i = 0; i < streamMap.size(); ++i) ended[i] = streamMap[i] < 0;
    const ULONGLONG start = GetTickCount64();
    ULONGLONG lastReport = 0;
    LONGLONG firstTs = -1;
    const double speedCap = SpeedCap(opt.performance);

    for (;;) {
        if (m_cancel) {
            result.cancelled = true;
            return result;  // writer released without Finalize; caller deletes the partial file
        }
        DWORD si = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(hr = reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &si, &flags, &ts, &sample)))
            return fail(L"Decoding failed");
        if (flags & MF_SOURCE_READERF_ERROR) {
            result.error = L"Decoding failed.";
            return result;
        }
        if (si >= streamMap.size()) continue;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            ended[si] = true;
            if (std::all_of(ended.begin(), ended.end(), [](bool e) { return e; })) break;
            continue;
        }
        if (streamMap[si] < 0) continue;
        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            writer->SendStreamTick(streamMap[si], ts);
            continue;
        }
        if (!sample) continue;
        if (FAILED(hr = writer->WriteSample(streamMap[si], sample.Get()))) return fail(L"Encoding failed");

        if (si == videoStream && speedCap > 0) {
            if (firstTs < 0) firstTs = ts;
            // Ahead of the allowed pace? Wait, so the GPU/CPU stay lightly loaded.
            const double ahead = (ts - firstTs) / 1e7 / speedCap - (GetTickCount64() - start) / 1000.0;
            if (ahead > 0.005) Sleep((DWORD)std::min(ahead * 1000.0, 500.0));
        }

        if (si == videoStream && onProgress) {
            if (firstTs < 0) firstTs = ts;
            ULONGLONG now = GetTickCount64();
            if (now - lastReport >= 250) {
                lastReport = now;
                const double done = (ts - firstTs) / 1e7;
                const double elapsed = (now - start) / 1000.0;
                EncodeProgress p;
                p.speed = elapsed > 0.2 ? done / elapsed : 0;
                if (duration > 0) {
                    p.percent = std::clamp(done / duration * 100.0, 0.0, 99.9);
                    if (p.speed > 0) p.etaSeconds = std::max(0.0, (duration - done) / p.speed);
                }
                onProgress(p);
            }
        }
    }

    if (FAILED(hr = writer->Finalize())) return fail(L"Couldn't finish the file");
    if (onProgress) {
        EncodeProgress p;
        p.percent = 100;
        onProgress(p);
    }
    result.ok = true;
    return result;
}

}  // namespace nv
