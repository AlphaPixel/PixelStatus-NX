#include "win32_output_driver.hpp"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <utility>

namespace pixelstatus::simulator {
namespace {

constexpr wchar_t window_class_name[] = L"PixelStatusNxSimulatorWindow";
constexpr int initial_client_size = 640;
constexpr int matrix_padding = 24;

}  // namespace

struct Win32OutputDriverImpl {
    std::size_t width{};
    std::size_t height{};
    std::wstring title;
    HWND window{};
    mutable std::mutex mutex;
    std::optional<Frame> latest_frame;
    bool frame_pending{};
    bool open{};
    DriverState driver_state;
};

namespace {

void paint_matrix(Win32OutputDriverImpl& impl, HDC device_context) {
    RECT client{};
    GetClientRect(impl.window, &client);

    const auto background = CreateSolidBrush(RGB(20, 22, 27));
    FillRect(device_context, &client, background);
    DeleteObject(background);

    std::optional<Frame> frame;
    {
        std::scoped_lock lock(impl.mutex);
        frame = impl.latest_frame;
        impl.frame_pending = false;
    }
    if (!frame || frame->width() == 0 || frame->height() == 0) {
        return;
    }

    const auto available_width = std::max(1L, client.right - client.left - 2L * matrix_padding);
    const auto available_height = std::max(1L, client.bottom - client.top - 2L * matrix_padding);
    const auto cell_width = static_cast<double>(available_width) / static_cast<double>(frame->width());
    const auto cell_height = static_cast<double>(available_height) / static_cast<double>(frame->height());
    const auto cell_size = std::max(1.0, std::min(cell_width, cell_height));
    const auto matrix_width = static_cast<int>(cell_size * static_cast<double>(frame->width()));
    const auto matrix_height = static_cast<int>(cell_size * static_cast<double>(frame->height()));
    const auto left = (client.right - matrix_width) / 2;
    const auto top = (client.bottom - matrix_height) / 2;
    const auto gap = std::max(1, static_cast<int>(cell_size / 10.0));

    for (std::size_t y = 0; y < frame->height(); ++y) {
        for (std::size_t x = 0; x < frame->width(); ++x) {
            const auto* color = frame->pixel(x, y);
            if (color == nullptr) {
                continue;
            }

            RECT pixel{
                left + static_cast<int>(static_cast<double>(x) * cell_size) + gap,
                top + static_cast<int>(static_cast<double>(y) * cell_size) + gap,
                left + static_cast<int>(static_cast<double>(x + 1U) * cell_size) - gap,
                top + static_cast<int>(static_cast<double>(y + 1U) * cell_size) - gap,
            };
            const auto brush = CreateSolidBrush(RGB(color->r, color->g, color->b));
            FillRect(device_context, &pixel, brush);
            DeleteObject(brush);
        }
    }
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* impl = reinterpret_cast<Win32OutputDriverImpl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        impl = static_cast<Win32OutputDriverImpl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
        impl->window = window;
    }

    if (impl == nullptr) {
        return DefWindowProcW(window, message, w_param, l_param);
    }

    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const auto context = BeginPaint(window, &paint);
            paint_matrix(*impl, context);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY: {
            std::scoped_lock lock(impl->mutex);
            impl->open = false;
            impl->driver_state.connection = DriverConnectionState::stopped;
            impl->driver_state.detail = "Simulator window closed";
            PostQuitMessage(0);
            return 0;
        }
        default:
            return DefWindowProcW(window, message, w_param, l_param);
    }
}

}  // namespace

Win32OutputDriver::Win32OutputDriver(
    std::size_t width,
    std::size_t height,
    std::wstring title)
    : impl_(std::make_unique<Win32OutputDriverImpl>()) {
    impl_->width = width;
    impl_->height = height;
    impl_->title = std::move(title);
}

Win32OutputDriver::~Win32OutputDriver() {
    if (impl_->window != nullptr && IsWindow(impl_->window)) {
        DestroyWindow(impl_->window);
    }
}

bool Win32OutputDriver::begin() {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = window_class_name;

    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::scoped_lock lock(impl_->mutex);
        impl_->driver_state.connection = DriverConnectionState::failed;
        impl_->driver_state.detail = "Unable to register the simulator window class";
        return false;
    }

    RECT desired{0, 0, initial_client_size, initial_client_size};
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
    const auto window = CreateWindowExW(
        0,
        window_class_name,
        impl_->title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        desired.right - desired.left,
        desired.bottom - desired.top,
        nullptr,
        nullptr,
        instance,
        impl_.get());
    if (window == nullptr) {
        std::scoped_lock lock(impl_->mutex);
        impl_->driver_state.connection = DriverConnectionState::failed;
        impl_->driver_state.detail = "Unable to create the simulator window";
        return false;
    }

    {
        std::scoped_lock lock(impl_->mutex);
        impl_->open = true;
        impl_->driver_state.connection = DriverConnectionState::ready;
        impl_->driver_state.detail = "Win32 simulator ready";
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    return true;
}

FrameSubmitResult Win32OutputDriver::submit_frame(const Frame& frame) {
    FrameSubmitResult result = FrameSubmitResult::accepted;
    {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->open || frame.width() != impl_->width || frame.height() != impl_->height) {
            return FrameSubmitResult::unavailable;
        }
        if (impl_->frame_pending) {
            result = FrameSubmitResult::coalesced;
            ++impl_->driver_state.coalesced_frames;
        }
        impl_->latest_frame = frame;
        impl_->frame_pending = true;
        ++impl_->driver_state.submitted_frames;
    }
    InvalidateRect(impl_->window, nullptr, FALSE);
    return result;
}

DriverState Win32OutputDriver::state() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->driver_state;
}

bool Win32OutputDriver::process_events() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    std::scoped_lock lock(impl_->mutex);
    return impl_->open;
}

}  // namespace pixelstatus::simulator
