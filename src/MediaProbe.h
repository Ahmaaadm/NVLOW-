#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace nv {

struct MediaInfo {
    bool ok = false;
    double duration = 0;  // seconds
    int width = 0;
    int height = 0;
    double fps = 0;
    double bitrate = 0;  // bits per second (whole file)
    std::wstring videoCodec;
    int audioTracks = 0;
};

// Reads duration, resolution, frame rate and codec with Windows Media Foundation.
MediaInfo ProbeMedia(const std::wstring& file);

// Background queue that probes files one at a time.
class ProbeQueue {
public:
    using Callback = std::function<void(int id, const MediaInfo& info)>;

    ~ProbeQueue() { Stop(); }
    void Start(Callback cb);
    void Push(int id, std::wstring path);
    void Stop();

private:
    void Loop();

    Callback m_callback;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<int, std::wstring>> m_queue;
    bool m_stop = false;
    std::thread m_thread;
};

}  // namespace nv
