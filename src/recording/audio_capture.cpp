#include "recording/audio_capture.h"

#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <thread>

#include "common/win32.h"
#include "core/audio_rules.h"
#include "recording/screen_capture.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kOutputRate = kAudioOutputRate;
constexpr std::uint32_t kOutputChannels = kAudioOutputChannels;

struct FormatDescription {
    bool valid = false;
    bool isFloat = false;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;
};

FormatDescription DescribeFormat(const WAVEFORMATEX& format) {
    FormatDescription description;
    description.bitsPerSample = format.wBitsPerSample;
    description.channels = format.nChannels;
    description.sampleRate = format.nSamplesPerSec;
    description.blockAlign = format.nBlockAlign;

    bool isPcm = format.wFormatTag == WAVE_FORMAT_PCM;
    bool isFloat = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        isPcm = ::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
        isFloat = ::IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
    }

    description.isFloat = isFloat;
    description.valid = description.channels >= 1 && description.sampleRate >= 8000 &&
                        description.blockAlign >= 2 &&
                        ((isFloat && description.bitsPerSample == 32) ||
                         (isPcm && (description.bitsPerSample == 16 ||
                                    description.bitsPerSample == 32)));
    return description;
}

std::uint32_t OutputFrameCount(std::uint32_t inputFrames, std::uint32_t inputRate) {
    if (inputFrames == 0) {
        return 0;
    }
    if (inputRate == kOutputRate) {
        return inputFrames;
    }
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(inputFrames) * kOutputRate + inputRate / 2;
    return static_cast<std::uint32_t>(scaled / inputRate);
}

}  // namespace

struct AudioCapture::Impl {
    AudioSource source = AudioSource::SystemLoopback;
    std::wstring deviceId;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> captureClient;
    HANDLE sampleEvent = nullptr;
    HANDLE stopEvent = nullptr;
    std::vector<std::uint8_t> formatStorage;
    FormatDescription format;
    std::thread thread;

    std::atomic<bool> stopping{false};
    std::atomic<bool> ended{false};
    std::atomic<bool> overflow{false};
    std::atomic<std::int64_t> lastPacketQpc{0};
    std::atomic<float> microphoneRms{0.0f};
    std::atomic<std::uint64_t> microphoneRmsTickMs{0};

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<AudioPacket> queue;
    std::uint32_t queuedFrames = 0;
    std::wstring error;

    void SetError(std::wstring message) {
        std::lock_guard lock(mutex);
        if (error.empty()) {
            error = std::move(message);
        }
        condition.notify_all();
    }

    void PushPacket(std::int64_t qpc, std::vector<std::int16_t> samples) {
        if (samples.empty()) {
            return;
        }
        float rms = 0.0f;
        if (source == AudioSource::Microphone) {
            long double energy = 0.0L;
            for (const std::int16_t sample : samples) {
                const long double normalized = static_cast<long double>(sample) / 32768.0L;
                energy += normalized * normalized;
            }
            rms = static_cast<float>(
                std::sqrt(energy / static_cast<long double>(samples.size())));
        }
        {
            std::lock_guard lock(mutex);
            if (!AudioQueueFits(queuedFrames, samples.size())) {
                overflow = true;
                stopping.store(true, std::memory_order_relaxed);
                if (error.empty()) {
                    error = L"audio capture queue overflow";
                }
                condition.notify_all();
                return;
            }
            queuedFrames += static_cast<std::uint32_t>(AudioFrameCount(samples.size()));
            queue.push_back(AudioPacket{qpc, std::move(samples)});
        }
        lastPacketQpc.store(qpc, std::memory_order_relaxed);
        if (source == AudioSource::Microphone) {
            microphoneRms.store(rms, std::memory_order_relaxed);
            microphoneRmsTickMs.store(::GetTickCount64(), std::memory_order_relaxed);
        }
        condition.notify_all();
    }

    std::vector<std::int16_t> ConvertPacket(const BYTE* data, UINT32 frames,
                                            DWORD flags) const {
        const std::uint32_t outputFrames = OutputFrameCount(frames, format.sampleRate);
        std::vector<std::int16_t> output(
            static_cast<std::size_t>(outputFrames) * kOutputChannels, 0);
        if (outputFrames == 0) {
            return output;
        }
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            return output;
        }

        const auto readChannel = [this, data](UINT32 frame, UINT32 channel) -> float {
            const BYTE* sample = data + static_cast<std::size_t>(frame) * format.blockAlign +
                                 static_cast<std::size_t>(channel) *
                                     (format.bitsPerSample / 8);
            if (format.isFloat) {
                float value = 0.0f;
                std::memcpy(&value, sample, sizeof(value));
                return std::clamp(value, -1.0f, 1.0f);
            }
            if (format.bitsPerSample == 16) {
                std::int16_t value = 0;
                std::memcpy(&value, sample, sizeof(value));
                return static_cast<float>(value) / 32768.0f;
            }
            std::int32_t value = 0;
            std::memcpy(&value, sample, sizeof(value));
            return static_cast<float>(static_cast<double>(value) / 2147483648.0);
        };

        const float ratio =
            static_cast<float>(format.sampleRate) / static_cast<float>(kOutputRate);
        const bool direct = format.sampleRate == kOutputRate;
        const std::uint16_t leftChannel = 0;
        const std::uint16_t rightChannel = format.channels > 1 ? 1 : 0;

        for (std::uint32_t i = 0; i < outputFrames; ++i) {
            float left = 0.0f;
            float right = 0.0f;
            if (direct) {
                left = readChannel(i, leftChannel);
                right = readChannel(i, rightChannel);
            } else {
                const double sourcePosition = static_cast<double>(i) * ratio;
                const auto index0 = static_cast<UINT32>(sourcePosition);
                const UINT32 index1 = std::min<UINT32>(index0 + 1, frames - 1);
                const float fraction =
                    static_cast<float>(sourcePosition - static_cast<double>(index0));
                const float left0 = readChannel(index0, leftChannel);
                const float left1 = readChannel(index1, leftChannel);
                const float right0 = readChannel(index0, rightChannel);
                const float right1 = readChannel(index1, rightChannel);
                left = left0 + (left1 - left0) * fraction;
                right = right0 + (right1 - right0) * fraction;
            }
            const auto toPcm = [](float value) -> std::int16_t {
                const float clamped = std::clamp(value, -1.0f, 1.0f);
                return static_cast<std::int16_t>(std::lround(clamped * 32767.0f));
            };
            output[static_cast<std::size_t>(i) * 2] = toPcm(left);
            output[static_cast<std::size_t>(i) * 2 + 1] = toPcm(right);
        }
        return output;
    }

    void Run() {
        const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
            SetError(Utf8ToWide(std::format("audio thread COM initialization failed: {}",
                                            HResultText(comHr))));
            ended.store(true);
            return;
        }

        while (!stopping.load(std::memory_order_relaxed)) {
            const HANDLE handles[] = {sampleEvent, stopEvent};
            const DWORD waitResult =
                ::WaitForMultipleObjects(2, handles, FALSE, 100);
            if (waitResult == WAIT_OBJECT_0 + 1) {
                break;
            }
            if (waitResult == WAIT_FAILED) {
                SetError(Utf8ToWide(std::format(
                    "WaitForMultipleObjects failed: {}",
                    HResultText(HRESULT_FROM_WIN32(::GetLastError())))));
                break;
            }
            if (waitResult != WAIT_OBJECT_0) {
                continue;
            }

            for (;;) {
                UINT32 packetFrames = 0;
                const HRESULT sizeHr = captureClient->GetNextPacketSize(&packetFrames);
                if (FAILED(sizeHr)) {
                    SetError(Utf8ToWide(
                        std::format("GetNextPacketSize failed: {}", HResultText(sizeHr))));
                    stopping.store(true);
                    break;
                }
                if (packetFrames == 0) {
                    break;
                }

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                const HRESULT bufferHr = captureClient->GetBuffer(
                    &data, &frames, &flags, &devicePosition, &qpcPosition);
                if (FAILED(bufferHr)) {
                    SetError(Utf8ToWide(
                        std::format("IAudioCaptureClient::GetBuffer failed: {}",
                                    HResultText(bufferHr))));
                    stopping.store(true);
                    break;
                }

                const std::int64_t qpc = static_cast<std::int64_t>(qpcPosition);
                if (frames > 0 &&
                    !ValidAudioTimestamp(
                        qpc, QpcNow100ns(), lastPacketQpc.load(std::memory_order_relaxed),
                        (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0)) {
                    captureClient->ReleaseBuffer(frames);
                    SetError(L"audio capture timestamp is invalid; recording stopped to "
                             L"preserve synchronization");
                    stopping.store(true, std::memory_order_relaxed);
                    break;
                }
                if (frames > 0) {
                    PushPacket(qpc, ConvertPacket(data, frames, flags));
                }
                captureClient->ReleaseBuffer(frames);
                if (stopping.load(std::memory_order_relaxed)) {
                    break;
                }
            }
        }

        if (client != nullptr) {
            client->Stop();
        }
        ended.store(true);
        condition.notify_all();
        if (SUCCEEDED(comHr)) {
            ::CoUninitialize();
        }
    }
};

AudioCapture::AudioCapture(AudioSource source, std::wstring deviceId)
    : impl_(std::make_unique<Impl>()) {
    impl_->source = source;
    impl_->deviceId = std::move(deviceId);
}

AudioCapture::~AudioCapture() {
    Stop();
    Join();
}

bool AudioCapture::Start(std::wstring* error) {
    if (impl_ == nullptr || impl_->thread.joinable()) {
        if (error != nullptr) {
            *error = L"audio capture is already running";
        }
        return false;
    }

    const auto fail = [this, error](std::wstring message) {
        if (error != nullptr) {
            *error = message;
        }
        impl_->SetError(message);
        return false;
    };

    const bool loopback = impl_->source == AudioSource::SystemLoopback;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(impl_->enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        return fail(Utf8ToWide(std::format("MMDeviceEnumerator failed: {}", HResultText(hr))));
    }
    if (!impl_->deviceId.empty()) {
        hr = impl_->enumerator->GetDevice(impl_->deviceId.c_str(),
                                          impl_->device.GetAddressOf());
        if (FAILED(hr)) {
            return fail(Utf8ToWide(std::format("selected {} endpoint was not found: {}",
                                               loopback ? "render" : "capture",
                                               HResultText(hr))));
        }
    } else {
        // Match the default endpoint shown in the device list and used by
        // AudioProbeResult. Opening eConsole here could capture a different
        // microphone than the UI's "default input" selection.
        hr = impl_->enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture,
                                                        eMultimedia,
                                                        impl_->device.GetAddressOf());
        if (FAILED(hr)) {
            hr = impl_->enumerator->GetDefaultAudioEndpoint(
                loopback ? eRender : eCapture,
                eConsole,
                impl_->device.GetAddressOf());
        }
        if (FAILED(hr)) {
            return fail(Utf8ToWide(std::format("no default {} endpoint: {}",
                                               loopback ? "render" : "capture",
                                               HResultText(hr))));
        }
    }
    hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(impl_->client.GetAddressOf()));
    if (FAILED(hr)) {
        return fail(Utf8ToWide(std::format("IAudioClient activation failed: {}",
                                           HResultText(hr))));
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = impl_->client->GetMixFormat(&mixFormat);
    if (FAILED(hr) || mixFormat == nullptr) {
        return fail(Utf8ToWide(std::format("GetMixFormat failed: {}", HResultText(hr))));
    }
    const std::size_t formatSize = sizeof(WAVEFORMATEX) + mixFormat->cbSize;
    impl_->formatStorage.assign(reinterpret_cast<const std::uint8_t*>(mixFormat),
                                reinterpret_cast<const std::uint8_t*>(mixFormat) + formatSize);
    impl_->format = DescribeFormat(*reinterpret_cast<const WAVEFORMATEX*>(
        impl_->formatStorage.data()));
    ::CoTaskMemFree(mixFormat);

    if (!impl_->format.valid) {
        return fail(loopback
                        ? L"the default playback mix format is not float32/int16/int32 PCM; "
                          L"system audio cannot be captured"
                        : L"the default microphone mix format is not float32/int16/int32 PCM; "
                          L"microphone audio cannot be captured");
    }

    impl_->sampleEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl_->sampleEvent == nullptr || impl_->stopEvent == nullptr) {
        return fail(Utf8ToWide(std::format("CreateEvent failed: {}",
                                           HResultText(HRESULT_FROM_WIN32(
                                               ::GetLastError())))));
    }

    const WAVEFORMATEX* format =
        reinterpret_cast<const WAVEFORMATEX*>(impl_->formatStorage.data());
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (loopback) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }
    hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 2'000'000, 0,
                                   format, nullptr);
    if (FAILED(hr)) {
        return fail(Utf8ToWide(std::format("IAudioClient::Initialize({}) failed: {}",
                                           loopback ? "loopback" : "microphone",
                                           HResultText(hr))));
    }
    hr = impl_->client->SetEventHandle(impl_->sampleEvent);
    if (FAILED(hr)) {
        return fail(Utf8ToWide(std::format("SetEventHandle failed: {}", HResultText(hr))));
    }
    hr = impl_->client->GetService(IID_PPV_ARGS(impl_->captureClient.GetAddressOf()));
    if (FAILED(hr)) {
        return fail(Utf8ToWide(std::format("GetService(IAudioCaptureClient) failed: {}",
                                           HResultText(hr))));
    }
    hr = impl_->client->Start();
    if (FAILED(hr)) {
        return fail(Utf8ToWide(std::format("IAudioClient::Start failed: {}", HResultText(hr))));
    }

    impl_->stopping.store(false);
    impl_->ended.store(false);
    impl_->overflow.store(false);
    impl_->thread = std::thread([this] { impl_->Run(); });
    return true;
}

void AudioCapture::Stop() {
    if (impl_ == nullptr) {
        return;
    }
    impl_->stopping.store(true);
    if (impl_->stopEvent != nullptr) {
        ::SetEvent(impl_->stopEvent);
    }
}

void AudioCapture::Join() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    if (impl_->client != nullptr && impl_->ended.load()) {
        impl_->client->Stop();
    }
    if (impl_->sampleEvent != nullptr) {
        ::CloseHandle(impl_->sampleEvent);
        impl_->sampleEvent = nullptr;
    }
    if (impl_->stopEvent != nullptr) {
        ::CloseHandle(impl_->stopEvent);
        impl_->stopEvent = nullptr;
    }
    impl_->captureClient.Reset();
    impl_->client.Reset();
    impl_->device.Reset();
    impl_->enumerator.Reset();
}

bool AudioCapture::TryPop(AudioPacket& packet) {
    if (impl_ == nullptr) {
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->queue.empty()) {
        return false;
    }
    packet = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    impl_->queuedFrames -=
        static_cast<std::uint32_t>(AudioFrameCount(packet.samples.size()));
    return true;
}

bool AudioCapture::ended() const noexcept {
    return impl_ != nullptr && impl_->ended.load(std::memory_order_relaxed);
}

bool AudioCapture::overflowed() const noexcept {
    return impl_ != nullptr && impl_->overflow.load(std::memory_order_relaxed);
}

float AudioCapture::inputLevel() const noexcept {
    if (impl_ == nullptr || impl_->source != AudioSource::Microphone) return 0.0f;
    const std::uint64_t updated = impl_->microphoneRmsTickMs.load(std::memory_order_relaxed);
    if (updated == 0) return 0.0f;
    const std::uint64_t ageMs = ::GetTickCount64() - updated;
    const float rms = impl_->microphoneRms.load(std::memory_order_relaxed);
    // Let the meter decay smoothly if an endpoint briefly stops delivering packets.
    return rms * std::exp(-static_cast<float>(ageMs) / 180.0f);
}

std::wstring AudioCapture::error() const {
    if (impl_ == nullptr) {
        return {};
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}

std::int64_t AudioCapture::lastPacketQpc100ns() const noexcept {
    return impl_ != nullptr ? impl_->lastPacketQpc.load(std::memory_order_relaxed) : 0;
}

AudioSource AudioCapture::source() const noexcept {
    return impl_ != nullptr ? impl_->source : AudioSource::SystemLoopback;
}

}  // namespace srl
