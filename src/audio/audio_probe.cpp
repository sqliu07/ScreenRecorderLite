#include "audio/audio_probe.h"

#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>

#include "common/win32.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kStreamFlagsLoopback = AUDCLNT_STREAMFLAGS_LOOPBACK;

std::wstring ReadPropertyString(IPropertyStore* store, REFPROPERTYKEY key) {
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    std::wstring result;
    if (store != nullptr && SUCCEEDED(store->GetValue(key, &value))) {
        PWSTR text = nullptr;
        if (SUCCEEDED(::PropVariantToStringAlloc(value, &text)) && text != nullptr) {
            result = text;
            ::CoTaskMemFree(text);
        }
    }
    ::PropVariantClear(&value);
    return result;
}

std::wstring EndpointId(IMMDevice* device) {
    LPWSTR raw = nullptr;
    if (device == nullptr || FAILED(device->GetId(&raw)) || raw == nullptr) {
        return {};
    }
    std::wstring id(raw);
    ::CoTaskMemFree(raw);
    return id;
}

std::wstring EndpointName(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (device == nullptr || FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
        return {};
    }
    return ReadPropertyString(store.Get(), PKEY_Device_FriendlyName);
}

std::wstring EndpointDeviceName(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    if (device == nullptr || FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
        return {};
    }
    return ReadPropertyString(store.Get(), PKEY_DeviceInterface_FriendlyName);
}

AudioFormatInfo ParseFormat(const WAVEFORMATEX& format) {
    AudioFormatInfo info;
    info.formatTag = format.wFormatTag;
    info.channels = format.nChannels;
    info.sampleRate = format.nSamplesPerSec;
    info.bitsPerSample = format.wBitsPerSample;
    info.validBitsPerSample = format.wBitsPerSample;
    info.blockAlign = format.nBlockAlign;
    info.averageBytesPerSecond = format.nAvgBytesPerSec;
    info.extensible = format.wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    if (info.extensible) {
        const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        info.validBitsPerSample = ext.Samples.wValidBitsPerSample;
        info.channelMask = ext.dwChannelMask;
        if (::IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            info.subFormat = L"IEEE float";
        } else if (::IsEqualGUID(ext.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            info.subFormat = L"PCM";
        } else {
            wchar_t buffer[64]{};
            ::StringFromGUID2(ext.SubFormat, buffer, ARRAYSIZE(buffer));
            info.subFormat = buffer;
        }
    }
    return info;
}

bool IsFloatFormat(const AudioFormatInfo& info) {
    if (info.extensible) {
        return info.subFormat == L"IEEE float";
    }
    return info.formatTag == WAVE_FORMAT_IEEE_FLOAT;
}

std::wstring DescribeFormat(const WAVEFORMATEX* format) {
    if (format == nullptr) {
        return L"unknown";
    }
    const AudioFormatInfo info = ParseFormat(*format);
    return info.Describe();
}

bool ActivateAudioClient(IMMDevice* device, IAudioClient** client) {
    if (device == nullptr) {
        return false;
    }
    return SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(client)));
}

void FillEndpoint(IMMDevice* device, bool render, const std::wstring& defaultId,
                  AudioEndpointInfo& info) {
    info.render = render;
    info.id = EndpointId(device);
    info.friendlyName = EndpointName(device);
    info.deviceName = EndpointDeviceName(device);
    info.defaultMultimedia = !defaultId.empty() && info.id == defaultId;

    ComPtr<IAudioClient> client;
    if (!ActivateAudioClient(device, client.GetAddressOf())) {
        info.formatError = L"IAudioClient activation failed";
        return;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mixFormat)) && mixFormat != nullptr) {
        info.mixFormat = ParseFormat(*mixFormat);
        info.hasMixFormat = true;
        ::CoTaskMemFree(mixFormat);
    } else {
        info.formatError = L"GetMixFormat failed";
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minimumPeriod = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
        info.defaultPeriod100ns = static_cast<std::uint64_t>(defaultPeriod);
        info.minimumPeriod100ns = static_cast<std::uint64_t>(minimumPeriod);
    }
}

void AddEndpoints(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                  const std::wstring& defaultId, std::vector<AudioEndpointInfo>& destination) {
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                              collection.GetAddressOf()))) {
        return;
    }
    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) {
        return;
    }
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, device.GetAddressOf()))) {
            continue;
        }
        AudioEndpointInfo info;
        FillEndpoint(device.Get(), flow == eRender, defaultId, info);
        destination.push_back(std::move(info));
    }
    std::sort(destination.begin(), destination.end(),
              [](const AudioEndpointInfo& a, const AudioEndpointInfo& b) {
                  if (a.defaultMultimedia != b.defaultMultimedia) {
                      return a.defaultMultimedia;
                  }
                  return a.friendlyName < b.friendlyName;
              });
}

AudioEndpointInfo MakeDefaultEndpoint(IMMDeviceEnumerator* enumerator, EDataFlow flow,
                                       bool& available) {
    AudioEndpointInfo info;
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, device.GetAddressOf()))) {
        available = false;
        return info;
    }
    FillEndpoint(device.Get(), flow == eRender, EndpointId(device.Get()), info);
    available = true;
    return info;
}

}  // namespace

bool AudioFormatInfo::isFloat() const {
    return IsFloatFormat(*this);
}

std::wstring AudioFormatInfo::Describe() const {
    const wchar_t* sampleKind = L"unknown";
    if (extensible) {
        sampleKind = subFormat.c_str();
    } else if (formatTag == WAVE_FORMAT_IEEE_FLOAT) {
        sampleKind = L"IEEE float";
    } else if (formatTag == WAVE_FORMAT_PCM) {
        sampleKind = L"PCM";
    }

    std::wstring result = std::to_wstring(sampleRate) + L" Hz, " +
                          std::to_wstring(channels) + L" ch, " +
                          std::to_wstring(bitsPerSample) + L"-bit";
    if (validBitsPerSample != 0 && validBitsPerSample != bitsPerSample) {
        result += L" (" + std::to_wstring(validBitsPerSample) + L"-bit valid)";
    }
    result += L", ";
    result += sampleKind;
    if (extensible && channelMask != 0) {
        wchar_t mask[32]{};
        ::swprintf_s(mask, L", mask 0x%X", channelMask);
        result += mask;
    }
    return result;
}

AudioProbeResult QueryAudioProbe(bool checkLoopback, bool checkMicrophone) {
    AudioProbeResult result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        result.fatalError = Utf8ToWide(
            std::format("MMDeviceEnumerator creation failed: {}", HResultText(hr)));
        return result;
    }

    result.defaultRender = MakeDefaultEndpoint(enumerator.Get(), eRender,
                                               result.hasDefaultRender);
    result.defaultCapture = MakeDefaultEndpoint(enumerator.Get(), eCapture,
                                                result.hasDefaultCapture);

    AddEndpoints(enumerator.Get(), eRender, result.defaultRender.id, result.renderEndpoints);
    AddEndpoints(enumerator.Get(), eCapture, result.defaultCapture.id, result.captureEndpoints);

    if (checkLoopback) {
        result.loopbackChecked = true;
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.GetAddressOf());
        if (FAILED(hr)) {
            result.loopbackDetail =
                Utf8ToWide(std::format("no default render endpoint: {}", HResultText(hr)));
        } else {
            ComPtr<IAudioClient> client;
            if (!ActivateAudioClient(device.Get(), client.GetAddressOf())) {
                result.loopbackDetail = L"IAudioClient activation failed";
            } else {
                WAVEFORMATEX* mixFormat = nullptr;
                if (FAILED(client->GetMixFormat(&mixFormat)) || mixFormat == nullptr) {
                    result.loopbackDetail = L"GetMixFormat failed";
                } else {
                    const HRESULT init = client->Initialize(
                        AUDCLNT_SHAREMODE_SHARED, kStreamFlagsLoopback, 10'000'000, 0,
                        mixFormat, nullptr);
                    if (FAILED(init)) {
                        result.loopbackDetail = Utf8ToWide(
                            std::format("Initialize failed: {}", HResultText(init)));
                    } else {
                        ComPtr<IAudioCaptureClient> capture;
                        const HRESULT service =
                            client->GetService(IID_PPV_ARGS(capture.GetAddressOf()));
                        if (FAILED(service)) {
                            result.loopbackDetail = Utf8ToWide(
                                std::format("GetService(IAudioCaptureClient) failed: {}",
                                            HResultText(service)));
                        } else {
                            const HRESULT started = client->Start();
                            if (FAILED(started)) {
                                result.loopbackDetail = Utf8ToWide(
                                    std::format("Start failed: {}", HResultText(started)));
                            } else {
                                ::Sleep(50);
                                client->Stop();
                                result.loopbackInitializeOk = true;
                                result.loopbackDetail = DescribeFormat(mixFormat);
                            }
                        }
                    }
                    ::CoTaskMemFree(mixFormat);
                }
            }
        }
    }

    if (checkMicrophone) {
        result.microphoneChecked = true;
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia,
                                                 device.GetAddressOf());
        if (FAILED(hr)) {
            result.microphoneDetail =
                Utf8ToWide(std::format("no default capture endpoint: {}", HResultText(hr)));
        } else {
            ComPtr<IAudioClient> client;
            if (!ActivateAudioClient(device.Get(), client.GetAddressOf())) {
                result.microphoneDetail = L"IAudioClient activation failed";
            } else {
                WAVEFORMATEX* mixFormat = nullptr;
                if (FAILED(client->GetMixFormat(&mixFormat)) || mixFormat == nullptr) {
                    result.microphoneDetail = L"GetMixFormat failed";
                } else {
                    const HRESULT init = client->Initialize(
                        AUDCLNT_SHAREMODE_SHARED, 0, 10'000'000, 0, mixFormat, nullptr);
                    if (FAILED(init)) {
                        result.microphoneDetail = Utf8ToWide(
                            std::format("Initialize failed: {}", HResultText(init)));
                    } else {
                        const HRESULT started = client->Start();
                        if (FAILED(started)) {
                            result.microphoneDetail = Utf8ToWide(
                                std::format("Start failed: {}", HResultText(started)));
                        } else {
                            ::Sleep(50);
                            client->Stop();
                            result.microphoneInitializeOk = true;
                            result.microphoneDetail = DescribeFormat(mixFormat);
                        }
                    }
                    ::CoTaskMemFree(mixFormat);
                }
            }
        }
    }

    return result;
}

void PrintAudioProbe(const AudioProbeResult& result) {
    const auto printEndpoint = [](const AudioEndpointInfo& endpoint, int index) {
        const std::string label =
            std::format("  #{} {}{}", index,
                        WideToUtf8(endpoint.friendlyName.empty() ? L"<unnamed>"
                                                                 : endpoint.friendlyName),
                        endpoint.defaultMultimedia ? " [default multimedia]" : "");
        ConsoleLine(label);
        if (!endpoint.deviceName.empty()) {
            ConsoleLine(std::format("      device {}", WideToUtf8(endpoint.deviceName)));
        }
        if (!endpoint.id.empty()) {
            ConsoleLine(std::format("      id {}", WideToUtf8(endpoint.id)));
        }
        if (endpoint.hasMixFormat) {
            ConsoleLine(std::format("      mix {}", WideToUtf8(endpoint.mixFormat.Describe())));
            if (endpoint.defaultPeriod100ns != 0) {
                ConsoleLine(std::format("      period default {:.2f} ms, minimum {:.2f} ms",
                                        static_cast<double>(endpoint.defaultPeriod100ns) / 10000.0,
                                        static_cast<double>(endpoint.minimumPeriod100ns) /
                                            10000.0));
            }
        } else if (!endpoint.formatError.empty()) {
            ConsoleLine(std::format("      mix unavailable: {}",
                                    WideToUtf8(endpoint.formatError)));
        }
    };

    if (!result.fatalError.empty()) {
        ConsoleLine(std::format("Audio: FAILED - {}", WideToUtf8(result.fatalError)));
        return;
    }

    ConsoleLine("Audio");
    ConsoleLine(std::format("  Default render     {}",
                            result.hasDefaultRender
                                ? WideToUtf8(result.defaultRender.friendlyName)
                                : "(none)"));
    if (result.hasDefaultRender) {
        ConsoleLine(std::format("      mix {}", WideToUtf8(result.defaultRender.mixFormat.Describe())));
    }
    ConsoleLine(std::format("  Default capture    {}",
                            result.hasDefaultCapture
                                ? WideToUtf8(result.defaultCapture.friendlyName)
                                : "(none)"));
    if (result.hasDefaultCapture) {
        ConsoleLine(std::format("      mix {}",
                                WideToUtf8(result.defaultCapture.mixFormat.Describe())));
    }

    ConsoleLine(std::format("  Render endpoints   {}", result.renderEndpoints.size()));
    for (std::size_t i = 0; i < result.renderEndpoints.size(); ++i) {
        printEndpoint(result.renderEndpoints[i], static_cast<int>(i) + 1);
    }
    ConsoleLine(std::format("  Capture endpoints  {}", result.captureEndpoints.size()));
    for (std::size_t i = 0; i < result.captureEndpoints.size(); ++i) {
        printEndpoint(result.captureEndpoints[i], static_cast<int>(i) + 1);
    }

    if (result.loopbackChecked) {
        ConsoleLine(std::format("  Loopback open      {} - {}",
                                result.loopbackInitializeOk ? "OK" : "FAILED",
                                WideToUtf8(result.loopbackDetail)));
    }
    if (result.microphoneChecked) {
        ConsoleLine(std::format("  Microphone open    {} - {}",
                                result.microphoneInitializeOk ? "OK" : "FAILED",
                                WideToUtf8(result.microphoneDetail)));
    }
}

}  // namespace srl
