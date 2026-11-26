#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace dexlink {

// Low-latency WASAPI PCM playback engine (48kHz, 16-bit Stereo, <= 10ms cap)
class WasapiAudioPlayer {
public:
    WasapiAudioPlayer() = default;
    ~WasapiAudioPlayer();

    WasapiAudioPlayer(const WasapiAudioPlayer&) = delete;
    WasapiAudioPlayer& operator=(const WasapiAudioPlayer&) = delete;

    bool Start();
    void Stop();

    void SetMuted(bool muted) { muted_.store(muted); }
    bool IsMuted() const { return muted_.load(); }

    void PushPcm(const uint8_t* data, size_t bytes);

private:
    void AudioThreadLoop();

    std::atomic<bool> running_{false};
    std::atomic<bool> muted_{false};
    std::thread audio_thread_;

    std::mutex ring_mu_;
    std::vector<int16_t> ring_; // interleaved L/R 48kHz samples
    size_t read_pos_ = 0;
};

}  // namespace dexlink
