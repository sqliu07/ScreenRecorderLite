#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <string>

namespace srl {

class MediaFoundationRuntime {
public:
    MediaFoundationRuntime();
    ~MediaFoundationRuntime();

    MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
    MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;

    [[nodiscard]] bool started() const noexcept { return started_; }

private:
    bool started_ = false;
};

[[nodiscard]] std::wstring GuidToWString(REFGUID guid);
[[nodiscard]] std::string MediaSubtypeName(REFGUID guid);
[[nodiscard]] std::string MediaMajorTypeName(REFGUID guid);
[[nodiscard]] std::string MftCategoryName(REFGUID guid);
[[nodiscard]] std::wstring AttributeString(IMFAttributes* attributes, REFGUID key);
[[nodiscard]] std::wstring ActivateFriendlyName(IMFActivate* activate);
[[nodiscard]] std::wstring TransformFriendlyName(IMFTransform* transform);
[[nodiscard]] bool ActivateIsHardware(IMFActivate* activate);
[[nodiscard]] bool TransformSupportsD3D11(IMFTransform* transform);
[[nodiscard]] bool TransformIsAsync(IMFTransform* transform);
[[nodiscard]] std::wstring MediaTypeSummary(IMFMediaType* mediaType);

}  // namespace srl
