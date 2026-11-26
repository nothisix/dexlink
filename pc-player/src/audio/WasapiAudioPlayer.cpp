#include "audio/WasapiAudioPlayer.hpp"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace dexlink {

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr size_t kMaxLatencySamples = (kSampleRate / 100) * kChannels; // 10ms = 480 stereo frames = 960 samples

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

}  // namespace

WasapiAudioPlayer::~WasapiAudioPlayer() {
    Stop();
}

bool WasapiAudioPlayer::Start() {
    if (running_.exchange(true)) return true;
    ring_.reserve(kMaxLatencySamples * 4);
    audio_thread_ = std::thread(&WasapiAudioPlayer::AudioThreadLoop, this);
    return true;
}

void WasapiAudioPlayer::Stop() {
    if (!running_.exchange(false)) return;
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
    std::lock_guard<std::mutex> lock(ring_mu_);
    ring_.clear();
    read_pos_ = 0;
}

void WasapiAudioPlayer::PushPcm(const uint8_t* data, size_t bytes) {
    if (!running_.load() || !data || bytes < 4) return;
    const size_t sample_count = bytes / sizeof(int16_t);
    const auto* samples = reinterpret_cast<const int16_t*>(data);

    std::lock_guard<std::mutex> lock(ring_mu_);
    if (read_pos_ > 0) {
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<ptrdiff_t>(read_pos_));
        read_pos_ = 0;
    }
    ring_.insert(ring_.end(), samples, samples + sample_count);

    // Drop-oldest guard: cap audio queue at ~12ms to prevent drift
    const size_t cap = kMaxLatencySamples + (kSampleRate / 500) * kChannels;
    if (ring_.size() > cap) {
        const size_t excess = ring_.size() - kMaxLatencySamples;
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<ptrdiff_t>(excess & ~size_t(1)));
    }
}

void WasapiAudioPlayer::AudioThreadLoop() {
    HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD task_idx = 0;
    HANDLE mm_task = AvSetMmThreadCharacteristicsA("Pro Audio", &task_idx);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* mix_fmt = nullptr;
    HANDLE audio_event = nullptr;

    auto cleanup = [&]() {
        if (client) client->Stop();
        if (audio_event) CloseHandle(audio_event);
        if (mix_fmt) CoTaskMemFree(mix_fmt);
        SafeRelease(render);
        SafeRelease(client);
        SafeRelease(device);
        SafeRelease(enumerator);
        if (mm_task) AvRevertMmThreadCharacteristics(mm_task);
        if (SUCCEEDED(com_hr)) CoUninitialize();
    };

    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator)))) {
        cleanup();
        return;
    }

    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
        cleanup();
        return;
    }

    if (FAILED(client->GetMixFormat(&mix_fmt)) || !mix_fmt) {
        cleanup();
        return;
    }

    WAVEFORMATEX pcm_fmt{};
    pcm_fmt.wFormatTag = WAVE_FORMAT_PCM;
    pcm_fmt.nChannels = kChannels;
    pcm_fmt.nSamplesPerSec = kSampleRate;
    pcm_fmt.wBitsPerSample = 16;
    pcm_fmt.nBlockAlign = (pcm_fmt.nChannels * pcm_fmt.wBitsPerSample) / 8;
    pcm_fmt.nAvgBytesPerSec = pcm_fmt.nSamplesPerSec * pcm_fmt.nBlockAlign;

    audio_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!audio_event) {
        cleanup();
        return;
    }

    // 10ms shared low-latency buffer (100,000 * 100ns)
    const REFERENCE_TIME buf_duration = 100000;
    const DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                               AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    bool using_native_pcm = false;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, buf_duration, 0, &pcm_fmt, nullptr);
    if (SUCCEEDED(hr)) {
        using_native_pcm = true;
    } else {
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, buf_duration, 0, mix_fmt, nullptr);
    }

    if (FAILED(hr) ||
        FAILED(client->SetEventHandle(audio_event)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render)))) {
        cleanup();
        return;
    }

    UINT32 buffer_frames = 0;
    client->GetBufferSize(&buffer_frames);
    client->Start();

    const bool mix_is_float = (mix_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         mix_fmt->wBitsPerSample == 32);
    const int out_channels = using_native_pcm ? kChannels : static_cast<int>(mix_fmt->nChannels);

    std::vector<int16_t> temp_pcm;

    while (running_.load()) {
        DWORD wait_res = WaitForSingleObject(audio_event, 20);
        if (!running_.load()) break;
        if (wait_res != WAIT_OBJECT_0 && wait_res != WAIT_TIMEOUT) continue;

        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        if (padding >= buffer_frames) continue;

        UINT32 avail_frames = buffer_frames - padding;
        if (avail_frames == 0) continue;

        BYTE* out_ptr = nullptr;
        if (FAILED(render->GetBuffer(avail_frames, &out_ptr)) || !out_ptr) continue;

        const size_t req_samples = static_cast<size_t>(avail_frames) * kChannels;
        temp_pcm.assign(req_samples, 0);

        {
            std::lock_guard<std::mutex> lock(ring_mu_);
            const size_t avail_samples = (ring_.size() > read_pos_) ? (ring_.size() - read_pos_) : 0;
            const size_t to_copy = std::min(req_samples, avail_samples);
            if (to_copy > 0) {
                std::memcpy(temp_pcm.data(), ring_.data() + read_pos_, to_copy * sizeof(int16_t));
                read_pos_ += to_copy;
            }
        }

        if (using_native_pcm) {
            std::memcpy(out_ptr, temp_pcm.data(), req_samples * sizeof(int16_t));
        } else if (mix_is_float) {
            float* dst = reinterpret_cast<float*>(out_ptr);
            for (UINT32 f = 0; f < avail_frames; ++f) {
                float l = static_cast<float>(temp_pcm[f * 2]) / 32768.0f;
                float r = static_cast<float>(temp_pcm[f * 2 + 1]) / 32768.0f;
                for (int c = 0; c < out_channels; ++c) {
                    dst[f * out_channels + c] = (c == 0) ? l : (c == 1 ? r : 0.0f);
                }
            }
        } else {
            std::memset(out_ptr, 0, avail_frames * mix_fmt->nBlockAlign);
        }

        render->ReleaseBuffer(avail_frames, 0);
    }

    cleanup();
}

}  // namespace dexlink
