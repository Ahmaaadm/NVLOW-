#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace nv {

enum class Codec : int { Av1Gpu, HevcGpu, H264Gpu, HevcCpu, H264Cpu, Count };
enum class Quality : int { Lossless, High, Balanced, Small, Count };
enum class TargetRes : int { Original, P1440, P1080, P720, Count };
enum class AudioMode : int { Copy, Aac192, Aac128, Remove, Count };
// How hard NVLOW works the GPU/CPU. Affects speed and system load, not file size.
enum class Performance : int { Low, Medium, High, MaxQuality, Count };

inline bool IsGpuCodec(Codec c) { return c == Codec::Av1Gpu || c == Codec::HevcGpu || c == Codec::H264Gpu; }

struct EncodeOptions {
    Codec codec = Codec::HevcGpu;
    Quality quality = Quality::High;
    TargetRes resolution = TargetRes::Original;
    AudioMode audio = AudioMode::Copy;
    Performance performance = Performance::Medium;
};

struct EncodeJob {
    std::wstring input;
    std::wstring output;
    double duration = 0;     // seconds, used for progress
    double sourceBitrate = 0;  // bits per second, used to never produce a bigger file
    EncodeOptions options;
};

struct EncodeProgress {
    double percent = -1;  // 0..100, or -1 when unknown
    double speed = 0;     // realtime multiplier
    double etaSeconds = -1;
};

struct EncodeResult {
    bool ok = false;
    bool cancelled = false;
    std::wstring error;
};

struct EncoderCaps {
    unsigned available = 0;  // bit per Codec
    std::wstring gpuName;    // adapter used for hardware encoding
};

// Finds the hardware (GPU) and software encoders Windows Media Foundation offers on this PC.
EncoderCaps DetectEncoders();

// Transcodes with Windows Media Foundation: GPU decode -> GPU encode -> MP4, audio copied as-is.
class Encoder {
public:
    using ProgressFn = std::function<void(const EncodeProgress&)>;

    EncodeResult Run(const EncodeJob& job, const ProgressFn& onProgress);
    void Cancel() { m_cancel = true; }
    bool Cancelled() const { return m_cancel; }

private:
    std::atomic<bool> m_cancel{false};
};

}  // namespace nv
