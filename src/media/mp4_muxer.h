#pragma once

#include <string>

namespace srl {

[[nodiscard]] bool CheckMp4MuxerAvailable(std::wstring* error);

[[nodiscard]] bool MuxMp4WithAudioTracks(const std::wstring& sourceMp4,
                                         const std::wstring& systemAudioM4a,
                                         const std::wstring& microphoneM4a,
                                         const std::wstring& outputMp4,
                                         std::wstring* error);

}  // namespace srl
