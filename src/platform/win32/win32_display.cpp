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
#include <dwmapi.h>
#include <GL/gl.h>
#include <GL/wglext.h>

#include "platform/win32/gl3_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

namespace pk {
namespace {

constexpr wchar_t kWindowClass[] = L"PuckKioskWin32Window";

// Keep the supersample target inside the 512 MB budget on large or high-DPI
// panels. 24M pixels is 192 MB for RGBA8 color plus DEPTH24_STENCIL8; the
// 1920x1080 default at 2x uses 8.3M pixels.
constexpr double kMaxSupersamplePixels = 24.0 * 1024.0 * 1024.0;

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
    float render_scale = 1.0f;
    float effective_scale = 1.0f;
    UINT dpi = 96;
    GLuint supersample_fbo = 0;
    GLuint supersample_color = 0;
    GLuint supersample_depth = 0;
    int supersample_width = 0;
    int supersample_height = 0;
    bool supersample_disabled = false;
    bool visible = true;
    bool fullscreen = false;
    bool quit = false;
    bool pointer_api = false;
    ClientCaption caption;
    bool maximized = false;
    RECT restore_rect = {};
    bool dragging = false;
    POINT drag_cursor = {};
    RECT drag_origin = {};
    std::string output_path;
    std::deque<InputEvent> events;

    GetPointerInfoFn get_pointer_info = nullptr;

    ~Impl() { destroy(); }

    void destroy_supersample() {
        if (gl) {
            if (supersample_depth) {
                glDeleteRenderbuffers(1, &supersample_depth);
            }
            if (supersample_color) {
                glDeleteRenderbuffers(1, &supersample_color);
            }
            if (supersample_fbo) {
                glDeleteFramebuffers(1, &supersample_fbo);
            }
        }
        supersample_depth = 0;
        supersample_color = 0;
        supersample_fbo = 0;
        supersample_width = 0;
        supersample_height = 0;
    }

    // The window is per-monitor DPI aware, so the client size is already in
    // physical pixels. dpi/96 only raises the supersample density on a
    // high-DPI panel; the blit still maps back to the client size. The pixel
    // budget keeps a 4K or high-DPI surface inside the memory target.
    void update_effective_scale() {
        if (supersample_disabled) {
            effective_scale = 1.0f;
            return;
        }
        const float requested = render_scale * static_cast<float>(dpi) / 96.0f;
        const double pixels = static_cast<double>(width) * static_cast<double>(height);
        const double max_scale =
            std::sqrt(kMaxSupersamplePixels / std::max(1.0, pixels));
        float scale = std::max(1.0f, requested);
        if (scale > max_scale) {
            scale = std::max(1.0f, static_cast<float>(max_scale));
            if (std::abs(scale - effective_scale) > 0.001f) {
                log::warn("win32: supersample scale %.2f exceeds the %dx%d pixel budget; using %.2f",
                          requested, width, height, scale);
            }
        }
        effective_scale = scale;
    }

    bool ensure_supersample() {
        if (supersample_disabled || effective_scale <= 1.0f + 1e-4f) {
            destroy_supersample();
            return true;
        }
        const int wanted_width =
            std::max(1, static_cast<int>(std::lround(width * effective_scale)));
        const int wanted_height =
            std::max(1, static_cast<int>(std::lround(height * effective_scale)));
        if (supersample_fbo && wanted_width == supersample_width &&
            wanted_height == supersample_height) {
            return true;
        }
        destroy_supersample();

        glGenFramebuffers(1, &supersample_fbo);
        glGenRenderbuffers(1, &supersample_color);
        glGenRenderbuffers(1, &supersample_depth);
        if (!supersample_fbo || !supersample_color || !supersample_depth) {
            log::error("win32: cannot allocate the %dx%d supersample target; rendering 1:1",
                       wanted_width, wanted_height);
            destroy_supersample();
            supersample_disabled = true;
            effective_scale = 1.0f;
            return false;
        }
        glBindRenderbuffer(GL_RENDERBUFFER, supersample_color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, wanted_width, wanted_height);
        glBindRenderbuffer(GL_RENDERBUFFER, supersample_depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, wanted_width, wanted_height);
        glBindFramebuffer(GL_FRAMEBUFFER, supersample_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  supersample_color);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  supersample_depth);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            log::error("win32: supersample target is incomplete (0x%x); rendering 1:1", status);
            destroy_supersample();
            supersample_disabled = true;
            effective_scale = 1.0f;
            return false;
        }
        supersample_width = wanted_width;
        supersample_height = wanted_height;
        const double color_mb =
            static_cast<double>(wanted_width) * wanted_height * 4.0 / 1e6;
        log::info("win32: supersample %dx%d (%.1f MB color, %.1f MB with depth-stencil), "
                  "scale %.2f",
                  wanted_width, wanted_height, color_mb, color_mb * 2.0, effective_scale);
        return true;
    }

    void destroy() {
        if (gl && dc) {
            wglMakeCurrent(dc, gl);
        }
        destroy_supersample();
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
        // App input lives below the client caption. The strip itself is hit
        // tested with raw client coordinates in the mouse handlers.
        event.y = static_cast<float>(client.y) * static_cast<float>(height) /
                      static_cast<float>(client_height) -
                  caption.height;
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

    bool in_caption_drag_region(POINT client) const {
        if (caption.height <= 0.0f) {
            return false;
        }
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        const int client_width = std::max(1, static_cast<int>(rect.right - rect.left));
        return client.y >= 0 && static_cast<float>(client.y) < caption.height &&
               static_cast<float>(client.x) <
                   static_cast<float>(client_width) - caption.buttons_width;
    }

    void log_window_state(const char* label) const {
        RECT outer = {};
        RECT client = {};
        GetWindowRect(hwnd, &outer);
        GetClientRect(hwnd, &client);
        log::info("win32: %s; window %dx%d at %d,%d, client %dx%d", label,
                  static_cast<int>(outer.right - outer.left),
                  static_cast<int>(outer.bottom - outer.top), static_cast<int>(outer.left),
                  static_cast<int>(outer.top),
                  static_cast<int>(client.right - client.left),
                  static_cast<int>(client.bottom - client.top));
    }

    void begin_drag() {
        if (dragging) {
            return;
        }
        dragging = true;
        GetCursorPos(&drag_cursor);
        GetWindowRect(hwnd, &drag_origin);
        SetCapture(hwnd);
        log::info("win32: caption drag start at %ld,%ld", static_cast<long>(drag_cursor.x),
                  static_cast<long>(drag_cursor.y));
        log_window_state("caption drag origin");
    }

    void update_drag() {
        if (!dragging) {
            return;
        }
        POINT cursor = {};
        if (!GetCursorPos(&cursor)) {
            return;
        }
        const int dx = cursor.x - drag_cursor.x;
        const int dy = cursor.y - drag_cursor.y;
        if (dx == 0 && dy == 0) {
            return;
        }
        SetWindowPos(hwnd, nullptr, drag_origin.left + dx, drag_origin.top + dy, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void end_drag() {
        if (!dragging) {
            return;
        }
        dragging = false;
        ReleaseCapture();
        log_window_state("caption drag end");
    }

    void minimize() {
        if (!hwnd) {
            return;
        }
        log::info("win32: minimize: ShowWindow(SW_MINIMIZE)");
        ShowWindow(hwnd, SW_MINIMIZE);
        log::info("win32: minimize: iconic=%d", IsIconic(hwnd) ? 1 : 0);
    }

    void toggle_maximize() {
        if (!hwnd) {
            return;
        }
        if (IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }
        if (!maximized) {
            GetWindowRect(hwnd, &restore_rect);
            MONITORINFO info = {};
            info.cbSize = sizeof(info);
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (!GetMonitorInfoW(monitor, &info)) {
                log::warn("win32: cannot read the monitor work area for maximize");
                return;
            }
            const int work_width = static_cast<int>(info.rcWork.right - info.rcWork.left);
            const int work_height = static_cast<int>(info.rcWork.bottom - info.rcWork.top);
            SetWindowPos(hwnd, nullptr, info.rcWork.left, info.rcWork.top, work_width,
                         work_height, SWP_NOZORDER | SWP_NOACTIVATE);
            maximized = true;
            log::info("win32: maximize to work area %dx%d at %d,%d (restore %dx%d at %d,%d)",
                      work_width, work_height, static_cast<int>(info.rcWork.left),
                      static_cast<int>(info.rcWork.top),
                      static_cast<int>(restore_rect.right - restore_rect.left),
                      static_cast<int>(restore_rect.bottom - restore_rect.top),
                      static_cast<int>(restore_rect.left),
                      static_cast<int>(restore_rect.top));
        } else {
            SetWindowPos(hwnd, nullptr, restore_rect.left, restore_rect.top,
                         restore_rect.right - restore_rect.left,
                         restore_rect.bottom - restore_rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            maximized = false;
            log::info("win32: restore to %dx%d at %d,%d",
                      static_cast<int>(restore_rect.right - restore_rect.left),
                      static_cast<int>(restore_rect.bottom - restore_rect.top),
                      static_cast<int>(restore_rect.left),
                      static_cast<int>(restore_rect.top));
        }
        log_window_state(maximized ? "maximized" : "restored");
        apply_corner_style();
    }

    // Win11 DWM rounds borderless windows with antialiased corners. Older
    // Windows rejects the attribute, so fall back to a round-rect region.
    // Maximized and fullscreen windows stay square.
    void apply_corner_style() {
        if (!hwnd || !visible) {
            return;
        }
        const bool square = fullscreen || maximized;
        constexpr DWORD kDwmWindowCornerPreference = 33;
        DWORD preference = square ? 1 : 2;  // DWMWCP_DONOTROUND / DWMWCP_ROUND
        const HRESULT hr =
            DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &preference, sizeof(preference));
        if (SUCCEEDED(hr)) {
            SetWindowRgn(hwnd, nullptr, TRUE);
            log::info("win32: corners %s (dwm)", square ? "square" : "round");
            return;
        }
        if (square) {
            SetWindowRgn(hwnd, nullptr, TRUE);
            return;
        }
        RECT rect = {};
        GetClientRect(hwnd, &rect);
        const HRGN region = CreateRoundRectRgn(0, 0, rect.right + 1, rect.bottom + 1, 9, 9);
        SetWindowRgn(hwnd, region, TRUE);
        log::info("win32: corners round (region fallback)");
    }

    void handle_mouse(UINT message, LPARAM lparam) {
        POINT client = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        switch (message) {
            case WM_MOUSEMOVE:
                update_drag();
                events.push_back(pointer_event(InputEvent::Type::PointerMove, client));
                break;
            case WM_LBUTTONDOWN:
                events.push_back(pointer_event(InputEvent::Type::PointerDown, client));
                if (in_caption_drag_region(client)) {
                    begin_drag();
                }
                break;
            case WM_LBUTTONUP:
                end_drag();
                events.push_back(pointer_event(InputEvent::Type::PointerUp, client));
                break;
            case WM_LBUTTONDBLCLK:
                if (in_caption_drag_region(client)) {
                    end_drag();
                    toggle_maximize();
                }
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
            case WM_CAPTURECHANGED:
                dragging = false;
                return 0;
            case WM_SYSCOMMAND: {
                // Keyboard and shell commands still reach a borderless window;
                // route the window ones through the same custom controls.
                const WPARAM command = wparam & 0xFFF0;
                if (command == SC_MINIMIZE) {
                    minimize();
                    return 0;
                }
                if (command == SC_MAXIMIZE) {
                    if (!maximized) {
                        toggle_maximize();
                    }
                    return 0;
                }
                if (command == SC_RESTORE) {
                    if (maximized) {
                        toggle_maximize();
                    } else {
                        ShowWindow(hwnd, SW_RESTORE);
                    }
                    return 0;
                }
                if (command == SC_CLOSE) {
                    request_quit();
                    return 0;
                }
                break;
            }
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
            case WM_LBUTTONDBLCLK:
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

Win32Display::Win32Display(int width, int height, float render_scale, std::string output_path,
                           int frames, bool visible, bool fullscreen)
    : impl_(std::make_unique<Impl>()) {
    impl_->width = width > 0 ? width : 1280;
    impl_->height = height > 0 ? height : 720;
    impl_->render_scale = std::clamp(render_scale, 1.0f, 3.0f);
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
    window_class.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = Impl::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return set_error(error, "RegisterClassExW failed: " + hresult_text(HRESULT_FROM_WIN32(GetLastError())));
    }

    // Windowed mode is borderless too: the app draws its own caption strip in
    // the client area, so there is no WS_CAPTION or WS_THICKFRAME to hide.
    const DWORD style = WS_POPUP;
    const DWORD ex_style =
        impl_->visible && !impl_->fullscreen ? WS_EX_APPWINDOW : 0;
    int window_width = impl_->width;
    int window_height = impl_->height;
    int window_x = CW_USEDEFAULT;
    int window_y = CW_USEDEFAULT;
    bool cover_taskbar = false;
    if (impl_->fullscreen) {
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
    impl_->hwnd = CreateWindowExW(ex_style, kWindowClass, L"Puck Kiosk", style, window_x,
                                  window_y, window_width, window_height, nullptr, nullptr,
                                  instance, impl_.get());
    log::info("win32: style 0x%08lx (WS_POPUP, no OS caption), ex 0x%08lx", style,
              ex_style);
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
        impl_->apply_corner_style();
    }

    UINT dpi = 96;
    if (user32) {
        auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (get_dpi) {
            dpi = get_dpi(impl_->hwnd);
        }
    }
    impl_->dpi = dpi;
    impl_->update_effective_scale();
    const char* mode =
        !impl_->visible ? "hidden" : (impl_->fullscreen ? "fullscreen" : "window");
    log::info("win32: WGL %s, GL %s, renderer %s, dpi %u, render scale %.2f%s", mode,
              reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)), dpi,
              impl_->effective_scale,
              dpi == 96 ? "" : " (layout is pixel-based; size the window with --width/--height)");
    RECT outer = {};
    GetWindowRect(impl_->hwnd, &outer);
    log::info("win32: client %dx%d, window %dx%d (%s)", impl_->width, impl_->height,
              static_cast<int>(outer.right - outer.left),
              static_cast<int>(outer.bottom - outer.top),
              impl_->fullscreen ? "fullscreen" : "windowed");
    impl_->ensure_supersample();
    return true;
}

Size Win32Display::size() const {
    return Size{impl_->width, impl_->height};
}

float Win32Display::dpi_scale() const {
    // Layout coordinates stay in client pixels; this factor maps them to the
    // supersample target, so NanoVG and Puck3D render at the higher density.
    // It is 1.0 when the window renders 1:1.
    return impl_->effective_scale;
}

void Win32Display::make_current() {
    if (!impl_->gl || !impl_->dc) {
        return;
    }
    wglMakeCurrent(impl_->dc, impl_->gl);
    RECT client = {};
    GetClientRect(impl_->hwnd, &client);
    impl_->width = std::max(1, static_cast<int>(client.right - client.left));
    impl_->height = std::max(1, static_cast<int>(client.bottom - client.top));
    impl_->update_effective_scale();
    if (impl_->ensure_supersample() && impl_->supersample_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, impl_->supersample_fbo);
        glViewport(0, 0, impl_->supersample_width, impl_->supersample_height);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, impl_->width, impl_->height);
    }
}

void Win32Display::swap() {
    if (!impl_->gl) {
        return;
    }
    glFinish();
    if (impl_->supersample_fbo) {
        // Downsample the whole frame in one linear blit. The 2D canvas and the
        // 3D pass share this target, so their edges filter the same way.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, impl_->supersample_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, impl_->supersample_width, impl_->supersample_height, 0, 0,
                          impl_->width, impl_->height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
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

bool Win32Display::needs_client_caption() const {
    return impl_->visible && !impl_->fullscreen;
}

void Win32Display::set_client_caption(const ClientCaption& caption) {
    impl_->caption = caption;
    log::info("win32: client caption %.0f px, %.0f px buttons", caption.height,
              caption.buttons_width);
}

void Win32Display::minimize_window() {
    impl_->minimize();
}

void Win32Display::toggle_maximize_window() {
    impl_->toggle_maximize();
}

void Win32Display::request_close() {
    log::info("win32: close requested by the client caption");
    impl_->request_quit();
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
