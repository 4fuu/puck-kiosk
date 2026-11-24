#include "platform/win32/win32_display.h"

#include "core/log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#include <GL/wglext.h>

#include "nanovg_gl3_functions.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

namespace pk {
namespace {

constexpr wchar_t kWindowClass[] = L"PuckKioskWin32Window";

using GetPointerInfoFn = BOOL(WINAPI*)(UINT32, POINTER_INFO*);
using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

std::string hresult_text(HRESULT result) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(result));
    return buffer;
}

bool set_error(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

}  // namespace

struct Win32Display::Impl {
    HWND hwnd = nullptr;
    HDC dc = nullptr;
    HGLRC gl = nullptr;
    int width = 1280;
    int height = 720;
    int frames = 1;
    int frame = 0;
    bool visible = true;
    bool fullscreen = false;
    bool quit = false;
    bool pointer_api = false;
    std::string output_path;
    std::deque<InputEvent> events;

    GetPointerInfoFn get_pointer_info = nullptr;

    ~Impl() { destroy(); }

    void destroy() {
        if (gl) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(gl);
            gl = nullptr;
        }
        if (dc && hwnd) {
            ReleaseDC(hwnd, dc);
            dc = nullptr;
        }
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }

    void request_quit() {
        quit = true;
        InputEvent event;
        event.type = InputEvent::Type::Quit;
        events.push_back(event);
    }

    InputEvent pointer_event(InputEvent::Type type, POINT client) const {
        InputEvent event;
        event.type = type;
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        const int client_width = std::max(1, static_cast<int>(rect.right - rect.left));
        const int client_height = std::max(1, static_cast<int>(rect.bottom - rect.top));
        event.x = static_cast<float>(client.x) * static_cast<float>(width) /
                  static_cast<float>(client_width);
        event.y = static_cast<float>(client.y) * static_cast<float>(height) /
                  static_cast<float>(client_height);
        return event;
    }

    void push_pointer(InputEvent::Type type, POINT screen) {
        POINT client = screen;
        ScreenToClient(hwnd, &client);
        events.push_back(pointer_event(type, client));
    }

    void push_key(InputEvent::Type type, int key) {
        InputEvent event;
        event.type = type;
        event.key = key;
        events.push_back(event);
    }

    void handle_pointer_message(UINT message, WPARAM wparam) {
        if (!get_pointer_info) {
            return;
        }
        POINTER_INFO info = {};
        if (!get_pointer_info(GET_POINTERID_WPARAM(wparam), &info)) {
            return;
        }
        // Mouse is handled through WM_MOUSE*; WM_POINTER is for touch and pen.
        if (info.pointerType == PT_MOUSE) {
            return;
        }
        POINT screen = {info.ptPixelLocation.x, info.ptPixelLocation.y};
        switch (message) {
            case WM_POINTERDOWN:
                push_pointer(InputEvent::Type::PointerDown, screen);
                break;
            case WM_POINTERUP:
                push_pointer(InputEvent::Type::PointerUp, screen);
                break;
            case WM_POINTERUPDATE:
                push_pointer(InputEvent::Type::PointerMove, screen);
                break;
            default:
                break;
        }
    }

    void handle_touch(WPARAM wparam, LPARAM lparam) {
        const UINT count = LOWORD(wparam);
        if (count == 0) {
            return;
        }
        std::vector<TOUCHINPUT> inputs(count);
        if (!GetTouchInputInfo(reinterpret_cast<HTOUCHINPUT>(lparam), count, inputs.data(),
                               sizeof(TOUCHINPUT))) {
            return;
        }
        for (const TOUCHINPUT& input : inputs) {
            POINT screen = {TOUCH_COORD_TO_PIXEL(input.x), TOUCH_COORD_TO_PIXEL(input.y)};
            if (input.dwFlags & TOUCHEVENTF_DOWN) {
                push_pointer(InputEvent::Type::PointerDown, screen);
            } else if (input.dwFlags & TOUCHEVENTF_UP) {
                push_pointer(InputEvent::Type::PointerUp, screen);
            } else if (input.dwFlags & TOUCHEVENTF_MOVE) {
                push_pointer(InputEvent::Type::PointerMove, screen);
            }
        }
        CloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(lparam));
    }

    void handle_mouse(UINT message, LPARAM lparam) {
        POINT client = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        switch (message) {
            case WM_MOUSEMOVE:
                events.push_back(pointer_event(InputEvent::Type::PointerMove, client));
                break;
            case WM_LBUTTONDOWN:
                events.push_back(pointer_event(InputEvent::Type::PointerDown, client));
                break;
            case WM_LBUTTONUP:
                events.push_back(pointer_event(InputEvent::Type::PointerUp, client));
                break;
            default:
                break;
        }
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CLOSE:
                request_quit();
                return 0;
            case WM_DESTROY:
                request_quit();
                PostQuitMessage(0);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint = {};
                BeginPaint(hwnd, &paint);
                EndPaint(hwnd, &paint);
                return 0;
            }
            case WM_SIZE: {
                if (wparam != SIZE_MINIMIZED) {
                    width = std::max(1, static_cast<int>(LOWORD(lparam)));
                    height = std::max(1, static_cast<int>(HIWORD(lparam)));
                }
                return 0;
            }
            case WM_POINTERDOWN:
            case WM_POINTERUPDATE:
            case WM_POINTERUP:
                handle_pointer_message(message, wparam);
                return 0;
            case WM_TOUCH:
                if (!pointer_api) {
                    handle_touch(wparam, lparam);
                    return 0;
                }
                break;
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
                handle_mouse(message, lparam);
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (wparam == VK_ESCAPE) {
                    push_key(InputEvent::Type::KeyDown, 27);
                } else if (wparam >= VK_F1 && wparam <= VK_F24) {
                    push_key(InputEvent::Type::KeyDown, static_cast<int>(wparam));
                }
                return 0;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                push_key(InputEvent::Type::KeyUp, static_cast<int>(wparam));
                return 0;
            case WM_CHAR:
                // Printable characters arrive here; Escape and other control
                // keys are handled by WM_KEYDOWN so they are not delivered twice.
                if (wparam >= 32 && wparam != 127) {
                    push_key(InputEvent::Type::KeyDown, static_cast<int>(wparam));
                }
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (impl) {
            impl->hwnd = hwnd;
            return impl->handle(message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

Win32Display::Win32Display(int width, int height, std::string output_path, int frames, bool visible,
                           bool fullscreen)
    : impl_(std::make_unique<Impl>()) {
    impl_->width = width > 0 ? width : 1280;
    impl_->height = height > 0 ? height : 720;
    impl_->frames = frames < 0 ? 0 : frames;
    impl_->visible = visible;
    impl_->fullscreen = fullscreen;
    impl_->output_path = std::move(output_path);
}

Win32Display::~Win32Display() = default;

bool Win32Display::initialize(std::string* error) {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        auto set_awareness = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_awareness) {
            set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
        impl_->get_pointer_info = reinterpret_cast<GetPointerInfoFn>(
            GetProcAddress(user32, "GetPointerInfo"));
    }
    impl_->pointer_api = impl_->get_pointer_info != nullptr;

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = Impl::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return set_error(error, "RegisterClassExW failed: " + hresult_text(HRESULT_FROM_WIN32(GetLastError())));
    }

    const DWORD style = impl_->fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    int window_width = impl_->width;
    int window_height = impl_->height;
    int window_x = CW_USEDEFAULT;
    int window_y = CW_USEDEFAULT;
    bool cover_taskbar = false;
    if (!impl_->fullscreen) {
        RECT rect = {0, 0, impl_->width, impl_->height};
        AdjustWindowRectEx(&rect, style, FALSE, 0);
        window_width = rect.right - rect.left;
        window_height = rect.bottom - rect.top;
    } else {
        // Borderless popup: the window is the client area, centered in the
        // primary monitor's work area. At the monitor's native size it also
        // covers the taskbar, so raise it above the shell.
        POINT origin = {0, 0};
        MONITORINFO monitor_info = {};
        monitor_info.cbSize = sizeof(monitor_info);
        const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfoW(monitor, &monitor_info)) {
            const int work_width = monitor_info.rcWork.right - monitor_info.rcWork.left;
            const int work_height = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
            window_x = monitor_info.rcWork.left + std::max(0, (work_width - impl_->width) / 2);
            window_y = monitor_info.rcWork.top + std::max(0, (work_height - impl_->height) / 2);
            cover_taskbar =
                impl_->width >= (monitor_info.rcMonitor.right - monitor_info.rcMonitor.left) &&
                impl_->height >= (monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top);
        } else {
            window_x = 0;
            window_y = 0;
        }
    }
    impl_->hwnd = CreateWindowExW(0, kWindowClass, L"Puck Kiosk", style, window_x, window_y,
                                  window_width, window_height, nullptr, nullptr, instance,
                                  impl_.get());
    if (!impl_->hwnd) {
        return set_error(error, "CreateWindowExW failed: " +
                                    hresult_text(HRESULT_FROM_WIN32(GetLastError())));
    }

    if (!impl_->pointer_api) {
        RegisterTouchWindow(impl_->hwnd, 0);
    }

    impl_->dc = GetDC(impl_->hwnd);
    if (!impl_->dc) {
        return set_error(error, "GetDC failed");
    }

    PIXELFORMATDESCRIPTOR format = {};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    format.cStencilBits = 8;
    const int pixel_format = ChoosePixelFormat(impl_->dc, &format);
    if (pixel_format == 0 || !SetPixelFormat(impl_->dc, pixel_format, &format)) {
        return set_error(error, "no OpenGL pixel format: " +
                                    hresult_text(HRESULT_FROM_WIN32(GetLastError())));
    }

    HGLRC temporary = wglCreateContext(impl_->dc);
    if (!temporary || !wglMakeCurrent(impl_->dc, temporary)) {
        return set_error(error, "wglCreateContext failed: " +
                                    hresult_text(HRESULT_FROM_WIN32(GetLastError())));
    }
    auto create_context = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!create_context) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(temporary);
        return set_error(error, "WGL_ARB_create_context is not available");
    }
    const int attributes[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0,
    };
    impl_->gl = create_context(impl_->dc, nullptr, attributes);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(temporary);
    if (!impl_->gl) {
        return set_error(error, "cannot create an OpenGL 3.3 core context: " +
                                    hresult_text(HRESULT_FROM_WIN32(GetLastError())));
    }
    if (!wglMakeCurrent(impl_->dc, impl_->gl)) {
        return set_error(error, "wglMakeCurrent failed");
    }

    const char* missing = nullptr;
    if (!nvg_gl3_load_functions(&missing)) {
        return set_error(error, std::string("missing OpenGL 3.3 entry point: ") +
                                    (missing ? missing : "unknown"));
    }

    auto swap_interval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
        wglGetProcAddress("wglSwapIntervalEXT"));
    if (swap_interval && impl_->visible) {
        swap_interval(1);
    }

    RECT client = {};
    GetClientRect(impl_->hwnd, &client);
    const int client_width = static_cast<int>(client.right - client.left);
    const int client_height = static_cast<int>(client.bottom - client.top);
    if (client_width != impl_->width || client_height != impl_->height) {
        // Wine and some themes report a different non-client area than
        // AdjustWindowRectEx assumed; resize once so the client area matches
        // --width/--height exactly.
        RECT outer = {};
        GetWindowRect(impl_->hwnd, &outer);
        SetWindowPos(impl_->hwnd, nullptr, 0, 0,
                     (outer.right - outer.left) + (impl_->width - client_width),
                     (outer.bottom - outer.top) + (impl_->height - client_height),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetClientRect(impl_->hwnd, &client);
    }
    impl_->width = std::max(1, static_cast<int>(client.right - client.left));
    impl_->height = std::max(1, static_cast<int>(client.bottom - client.top));

    if (impl_->visible) {
        ShowWindow(impl_->hwnd, SW_SHOW);
        UpdateWindow(impl_->hwnd);
        if (impl_->fullscreen && cover_taskbar) {
            SetWindowPos(impl_->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
        SetForegroundWindow(impl_->hwnd);
        SetFocus(impl_->hwnd);
    }

    UINT dpi = 96;
    if (user32) {
        auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (get_dpi) {
            dpi = get_dpi(impl_->hwnd);
        }
    }
    const char* mode =
        !impl_->visible ? "hidden" : (impl_->fullscreen ? "fullscreen" : "window");
    log::info("win32: WGL %s, GL %s, renderer %s, dpi %u%s", mode,
              reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)), dpi,
              dpi == 96 ? "" : " (layout is pixel-based; size the window with --width/--height)");
    RECT outer = {};
    GetWindowRect(impl_->hwnd, &outer);
    log::info("win32: client %dx%d, window %dx%d (%s)", impl_->width, impl_->height,
              static_cast<int>(outer.right - outer.left),
              static_cast<int>(outer.bottom - outer.top),
              impl_->fullscreen ? "fullscreen" : "windowed");
    return true;
}

Size Win32Display::size() const {
    return Size{impl_->width, impl_->height};
}

float Win32Display::dpi_scale() const {
    // The window is per-monitor DPI aware, so its client area is already in
    // physical pixels. Layout coordinates are pixels too; NanoVG must not
    // scale them a second time.
    return 1.0f;
}

void Win32Display::make_current() {
    if (!impl_->gl || !impl_->dc) {
        return;
    }
    wglMakeCurrent(impl_->dc, impl_->gl);
    RECT client = {};
    GetClientRect(impl_->hwnd, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    if (width != impl_->width || height != impl_->height) {
        impl_->width = width;
        impl_->height = height;
    }
    glViewport(0, 0, impl_->width, impl_->height);
}

void Win32Display::swap() {
    if (!impl_->gl) {
        return;
    }
    glFinish();
    if (impl_->frames > 0 && impl_->frame + 1 >= impl_->frames && !impl_->output_path.empty()) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(impl_->width) * impl_->height * 4);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, impl_->width, impl_->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        std::ofstream output(impl_->output_path, std::ios::binary);
        if (!output) {
            log::error("cannot open %s", impl_->output_path.c_str());
        } else {
            output << "P6\n" << impl_->width << " " << impl_->height << "\n255\n";
            for (int y = impl_->height - 1; y >= 0; --y) {
                const auto* row = pixels.data() + static_cast<std::size_t>(y) * impl_->width * 4;
                for (int x = 0; x < impl_->width; ++x) {
                    output.put(static_cast<char>(row[x * 4 + 0]));
                    output.put(static_cast<char>(row[x * 4 + 1]));
                    output.put(static_cast<char>(row[x * 4 + 2]));
                }
            }
            log::info("wrote %s (%dx%d)", impl_->output_path.c_str(), impl_->width, impl_->height);
        }
    }
    if (impl_->dc) {
        SwapBuffers(impl_->dc);
    }
    ++impl_->frame;
}

bool Win32Display::should_close() const {
    if (impl_->quit) {
        return true;
    }
    return impl_->frames > 0 && impl_->frame >= impl_->frames;
}

const char* Win32Display::backend_name() const {
    return impl_->visible ? "win32" : "win32-headless";
}

void Win32Display::pump_messages() {
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            impl_->quit = true;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool Win32Display::next_event(InputEvent* event) {
    if (!event || impl_->events.empty()) {
        return false;
    }
    *event = impl_->events.front();
    impl_->events.pop_front();
    return true;
}

bool Win32Input::poll(InputEvent& event) {
    if (display_.next_event(&event)) {
        return true;
    }
    display_.pump_messages();
    return display_.next_event(&event);
}

}  // namespace pk
