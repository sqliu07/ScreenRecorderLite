#include "app/screenshot.h"

#include <dwmapi.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <winrt/base.h>

#include "common/win32.h"
#include "image/png_writer.h"
#include "platform/displays.h"
#include "recording/screen_capture.h"

namespace srl {
namespace {

std::wstring MakeScreenshotPath(const std::wstring& outputDirectory) {
    const std::filesystem::path directory =
        std::filesystem::path(outputDirectory) / L"Screenshots";
    std::string directoryError;
    if (!EnsureDirectory(directory, &directoryError)) {
        return {};
    }
    const std::wstring base = L"Shot_" + CurrentTimestampForFileName();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::wstring name =
            base + (attempt == 0 ? L"" : L"_" + std::to_wstring(attempt)) + L".png";
        const std::filesystem::path candidate = directory / name;
        std::error_code existsError;
        const bool finalExists = std::filesystem::exists(candidate, existsError);
        if (existsError) {
            return {};
        }
        const bool temporaryExists =
            std::filesystem::exists(candidate.wstring() + L".tmp", existsError);
        if (existsError) {
            return {};
        }
        if (!finalExists && !temporaryExists) {
            return candidate.wstring();
        }
    }
    return {};
}

bool ValidFrame(const ScreenshotFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.width > std::numeric_limits<int>::max() / 4) {
        return false;
    }
    const auto bytes = static_cast<std::uint64_t>(frame.width) *
        static_cast<unsigned>(frame.height) * 4;
    return bytes <= std::numeric_limits<UINT>::max() && bytes == frame.bgra.size();
}

bool CaptureFrame(HMONITOR monitor, const RECT& region, ScreenshotFrame& snapshot,
                  std::wstring& error) {
    if (monitor == nullptr) {
        error = L"未选择可用显示器";
        return false;
    }
    const auto displays = EnumerateDisplays();
    const auto target = std::find_if(displays.begin(), displays.end(),
        [monitor](const MonitorInfo& display) { return display.handle == monitor; });
    if (target == displays.end()) {
        error = L"所选显示器已不可用，请重新选择";
        return false;
    }
    if (IsAdvancedColorActive(*target)) {
        error = L"目标显示器已启用 HDR/高级色彩。首版只支持 SDR，请切换到 SDR 后重试。";
        return false;
    }
    const int monitorWidth = target->virtualRect.right - target->virtualRect.left;
    const int monitorHeight = target->virtualRect.bottom - target->virtualRect.top;
    if (region.left < 0 || region.top < 0 || region.right <= region.left ||
        region.bottom <= region.top || region.right > monitorWidth ||
        region.bottom > monitorHeight) {
        error = L"截图区域无效或超出目标显示器，请重新选择";
        return false;
    }

    // The caller hid its windows before submitting. Wait for that desktop
    // update before starting the one-shot capture session.
    ::DwmFlush();
    ScreenCapture capture;
    std::wstring captureError;
    if (!capture.Start(monitor, 30, &captureError)) {
        error = L"截图捕获失败：" + captureError;
        return false;
    }
    if (!capture.WaitForFirstFrame(3000, &captureError)) {
        capture.Stop();
        error = L"等待截图首帧失败：" + captureError;
        return false;
    }

    const bool copied = capture.CopySnapshot(region, snapshot, &captureError);
    capture.Stop();
    if (!copied) {
        error = L"截图画面复制失败：" + captureError;
        return false;
    }
    return true;
}

ScreenshotResult SaveFrame(ScreenshotFrame& frame, const std::wstring& outputDirectory) {
    ScreenshotResult result;
    if (!ValidFrame(frame)) {
        result.error = L"截图画面数据无效";
        return result;
    }
    if (outputDirectory.empty()) {
        result.error = L"请选择截图保存位置";
        return result;
    }
    result.path = MakeScreenshotPath(outputDirectory);
    if (result.path.empty()) {
        result.error = L"无法创建截图目录或生成文件名，请检查保存位置";
        return result;
    }
    for (std::size_t alpha = 3; alpha < frame.bgra.size(); alpha += 4) {
        frame.bgra[alpha] = 255;
    }
    if (!SaveBgraPng(result.path, frame.bgra.data(), frame.width, frame.height,
                     frame.width * 4, &result.error)) {
        result.path.clear();
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace

ScreenshotResult CaptureScreenshotPng(HMONITOR monitor, const RECT& region,
                                      const std::wstring& outputDirectory) {
    ScreenshotFrame frame;
    ScreenshotResult result;
    if (outputDirectory.empty()) {
        result.error = L"请选择截图保存位置";
    } else if (CaptureFrame(monitor, region, frame, result.error)) {
        return SaveFrame(frame, outputDirectory);
    }
    return result;
}

struct ScreenshotWorker::Impl {
    struct Job {
        bool idleCapture = false;
        HMONITOR monitor = nullptr;
        RECT region{};
        std::wstring outputDirectory;
        ScreenshotFrame frame;
    };

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Job> jobs;
    std::deque<ScreenshotEvent> events;
    std::thread thread;
    std::size_t pending = 0;
    bool idleCapturePending = false;
    bool closing = false;

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            closing = true;
        }
        condition.notify_one();
        if (thread.joinable()) {
            thread.join();
        }
    }

    bool Submit(Job job, std::wstring* error) {
        const auto fail = [error](const wchar_t* message) {
            if (error != nullptr) {
                *error = message;
            }
            return false;
        };
        if (job.outputDirectory.empty()) {
            return fail(L"请选择截图保存位置");
        }
        std::lock_guard lock(mutex);
        if (closing) {
            return fail(L"截图保存已结束");
        }
        if (pending >= 2 || (job.idleCapture && idleCapturePending)) {
            return fail(L"截图正在保存，请稍后");
        }
        const bool idleCapture = job.idleCapture;
        jobs.push_back(std::move(job));
        try {
            if (!thread.joinable()) {
                thread = std::thread([this] { Run(); });
            }
        } catch (const std::exception&) {
            jobs.pop_back();
            return fail(L"无法启动截图保存任务，请稍后重试");
        }
        ++pending;
        idleCapturePending = idleCapturePending || idleCapture;
        condition.notify_one();
        return true;
    }

    void Publish(ScreenshotEvent event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }

    void Run() {
        bool apartmentInitialized = false;
        std::wstring initializationError;
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartmentInitialized = true;
        } catch (const winrt::hresult_error& error) {
            initializationError = L"截图组件初始化失败：" + FormatHResultMessage(error.code());
        } catch (const std::exception& error) {
            initializationError = L"截图组件初始化失败：" + Utf8ToWide(error.what());
        } catch (...) {
            initializationError = L"截图组件初始化失败，请稍后重试";
        }
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return closing || !jobs.empty(); });
                if (jobs.empty()) {
                    break;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            ScreenshotResult result;
            try {
                if (!initializationError.empty()) {
                    result.error = initializationError;
                } else if (!job.idleCapture ||
                           CaptureFrame(job.monitor, job.region, job.frame, result.error)) {
                    if (job.idleCapture) {
                        Publish(ScreenshotEvent{ScreenshotEvent::Type::Captured, true, {}});
                    }
                    result = SaveFrame(job.frame, job.outputDirectory);
                }
            } catch (const winrt::hresult_error& error) {
                result.error = L"截图失败：" + FormatHResultMessage(error.code());
            } catch (const std::exception& error) {
                result.error = L"截图失败：" + Utf8ToWide(error.what());
            } catch (...) {
                result.error = L"截图失败，请稍后重试";
            }
            Publish(ScreenshotEvent{ScreenshotEvent::Type::Completed, job.idleCapture,
                                    std::move(result)});
        }
        if (apartmentInitialized) {
            winrt::uninit_apartment();
        }
    }
};

ScreenshotWorker::ScreenshotWorker() : impl_(std::make_unique<Impl>()) {}
ScreenshotWorker::~ScreenshotWorker() = default;

bool ScreenshotWorker::SubmitCapture(HMONITOR monitor, const RECT& region,
                                     const std::wstring& outputDirectory,
                                     std::wstring* error) {
    return impl_->Submit(Impl::Job{true, monitor, region, outputDirectory, {}}, error);
}

bool ScreenshotWorker::SubmitSnapshot(ScreenshotFrame frame,
                                      const std::wstring& outputDirectory,
                                      std::wstring* error) {
    if (!ValidFrame(frame)) {
        if (error != nullptr) {
            *error = L"截图画面数据无效";
        }
        return false;
    }
    return impl_->Submit(Impl::Job{false, nullptr, {}, outputDirectory, std::move(frame)},
                         error);
}

bool ScreenshotWorker::TryPop(ScreenshotEvent& event) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->events.empty()) {
        return false;
    }
    event = std::move(impl_->events.front());
    impl_->events.pop_front();
    if (event.type == ScreenshotEvent::Type::Completed) {
        --impl_->pending;
        if (event.idleCapture) {
            impl_->idleCapturePending = false;
        }
    }
    return true;
}

std::size_t ScreenshotWorker::pending() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->pending;
}

}  // namespace srl
