set_project("puck-kiosk")
set_version("0.1.0")
set_languages("c++17")
set_encodings("utf-8")

add_rules("mode.debug", "mode.release")

option("backend")
    set_default("headless")
    set_values("headless", "drm", "wayland", "x11", "win32")
    set_description("Display/input backend to build")
option_end()

option("puck3d")
    set_default(true)
    set_description("Enable the Puck 3D renderer (falls back to 2D when unavailable)")
option_end()

local backend = get_config("backend") or "headless"
local win32 = backend == "win32"
if win32 and not is_plat("windows", "mingw") then
    raise("--backend=win32 needs a Windows target: xmake f -p mingw -a x64 --backend=win32")
end

local puck3d = get_config("puck3d")
if puck3d == nil then puck3d = not win32 end
if win32 then
    -- Puck3D is a GLES 3.0 renderer. The Windows backend uses desktop GL 3.3
    -- for the 2D canvas, so the 3D pass stays disabled until it is ported.
    puck3d = false
end

if win32 then
    -- The mingw cross build and the native MSVC build share this list.
    -- libcurl uses Schannel. IXWebSocket and libdatachannel both use OpenSSL 3
    -- so the process links one TLS implementation (OpenSSL 3 is what the
    -- libdatachannel port requires).
    add_repositories("puck-kiosk-packages third_party/xmake-repo")
    add_requires("libcurl")
    add_requires("libwebp", {configs = {shared = false}})
    add_requires("libpng")
    add_requires("libjpeg")
    add_requires("libqrencode")
    add_requires("libopus")
    add_requires("ixwebsocket v12.1.0", {configs = {ssl = "openssl3", use_tls = true}})
    add_requires("libdatachannel 0.24.6", {configs = {media = true}})
else
    add_requires("ixwebsocket v12.1.0", {configs = {ssl = "openssl", use_tls = true}})
    add_requires("libdatachannel 0.24.6", {configs = {media = true}})
    add_requires("pkgconfig::webrtc-audio-processing-1")
    add_requires("pkgconfig::alsa")
    add_requires("pkgconfig::opus")
end

if backend == "drm" then
    add_requires("pkgconfig::libdrm")
    add_requires("pkgconfig::gbm")
    add_requires("pkgconfig::libudev")
    -- Optional: the evdev fallback covers builds without libinput.
    add_requires("pkgconfig::libinput", {optional = true})
end

target("nanovg")
    set_kind("static")
    if win32 then
        add_files("third_party/nanovg/nanovg.c", "third_party/nanovg/nanovg_gl3.c")
        add_syslinks("opengl32")
        add_includedirs("third_party/gl-headers", { public = true })
    else
        add_files("third_party/nanovg/nanovg.c", "third_party/nanovg/nanovg_gles3.c")
    end
    add_includedirs("third_party/nanovg", { public = true })
    set_warnings("none")

target("puck-kiosk")
    set_kind("binary")
    set_default(true)
    set_rundir("$(projectdir)")
    add_deps("nanovg")
    add_files("src/**.cpp")
    add_includedirs("src", "third_party")
    if win32 then
        remove_files("src/platform/drm/**.cpp")
        remove_files("src/platform/headless/**.cpp")
        remove_files("src/platform/audio_alsa.cpp")
        remove_files("src/puck/puck_3d*.cpp")
        add_packages("libcurl", "libwebp", "libpng", "libjpeg", "libqrencode",
                     "libopus", "ixwebsocket", "libdatachannel")
        add_includedirs("third_party/gl-headers")
        add_links("opengl32", "gdi32", "user32", "ole32", "winmm", "uuid", "ws2_32", "crypt32",
                  "iphlpapi", "bcrypt", "secur32", "advapi32", "wldap32", "mmdevapi")
        if is_plat("mingw") then
            add_syslinks("winpthread")
        end
        add_defines("PUCK_KIOSK_BACKEND_WIN32=1")
        add_defines("PUCK_KIOSK_CANVAS_NANOVG_GL3=1")
        add_defines("SCTP_STDINT_INCLUDE=<stdint.h>")
        if is_plat("mingw") then
            -- mingw's printf otherwise rejects the C99 %zu used by the logger.
            add_defines("__USE_MINGW_ANSI_STDIO=1")
            -- Keep the .exe self-contained: no libstdc++/libgcc/winpthread DLLs.
            add_ldflags("-static", "-static-libgcc", "-static-libstdc++")
            add_cxflags("-Wno-cast-function-type")
        end
    else
        remove_files("src/platform/win32/**.cpp")
        if backend == "drm" then
            add_files("src/platform/drm/*.cpp")
            if not has_package("pkgconfig::libinput") then
                remove_files("src/platform/drm/libinput_input.cpp")
            end
        else
            remove_files("src/platform/drm/**.cpp")
        end
        add_packages("ixwebsocket", "libdatachannel", "pkgconfig::webrtc-audio-processing-1",
                     "pkgconfig::alsa", "pkgconfig::opus")
        if backend == "drm" then
            add_packages("pkgconfig::libdrm", "pkgconfig::gbm", "pkgconfig::libudev")
            if has_package("pkgconfig::libinput") then
                add_packages("pkgconfig::libinput")
                add_defines("PUCK_KIOSK_HAVE_LIBINPUT=1")
            end
        end
        add_links("EGL", "GLESv2", "m", "dl", "pthread", "png", "webp", "jpeg", "curl", "qrencode",
                  "ssl", "crypto")
        add_defines("PUCK_KIOSK_ENABLE_AEC=1")
        add_defines("PUCK_KIOSK_ENABLE_ALSA=1")
    end
    add_defines("PUCK_KIOSK_BACKEND_" .. backend:upper() .. "=1")
    if puck3d then
        add_defines("PUCK_KIOSK_ENABLE_3D=1")
    end
    set_warnings("all", "extra")
    add_cxflags("-Wno-unused-parameter")
    if is_mode("release") then
        add_cxflags("-fno-omit-frame-pointer")
    end

target("puck-tests")
    set_kind("binary")
    set_default(false)
    set_rundir("$(projectdir)")
    add_files("tests/puck_tests.cpp")
    add_files("src/puck/puck_v2.cpp", "src/puck/puck_rig.cpp", "src/puck/puck_3d_pose.cpp")
    add_includedirs("src", "third_party")
    add_links("m")
    set_warnings("all", "extra")

target("media-tests")
    set_kind("binary")
    set_default(false)
    set_rundir("$(projectdir)")
    add_files("tests/media_tests.cpp", "src/app/media_overlay.cpp", "src/core/log.cpp")
    add_includedirs("src", "third_party")
    add_links("m", "pthread", "png", "webp", "jpeg")
    set_warnings("all", "extra")

target("ui-tests")
    set_kind("binary")
    set_default(false)
    set_rundir("$(projectdir)")
    add_files("tests/ui_tests.cpp", "src/app/car_ui.cpp", "src/app/ui_input.cpp",
              "src/app/ui_focus.cpp", "src/app/plugin_ui.cpp", "src/app/passkey.cpp",
              "src/app/diagnostics.cpp", "src/app/media_overlay.cpp", "src/core/log.cpp")
    add_includedirs("src", "third_party")
    add_links("m", "pthread", "png", "webp", "jpeg")
    set_warnings("all", "extra")

target("ui-render-tests")
    set_kind("binary")
    set_default(false)
    set_rundir("$(projectdir)")
    add_deps("nanovg")
    add_files("tests/ui_render_tests.cpp", "src/app/car_ui.cpp", "src/app/media_overlay.cpp",
              "src/app/ui_input.cpp", "src/core/log.cpp", "src/render/renderer.cpp",
              "src/render/nanovg_canvas.cpp", "src/platform/headless/headless_display.cpp")
    add_includedirs("src", "third_party")
    add_links("EGL", "GLESv2", "m", "pthread", "png", "webp", "jpeg")
    set_warnings("all", "extra")

target("voice-tests")
    set_kind("binary")
    set_default(false)
    set_rundir("$(projectdir)")
    add_files("tests/voice_tests.cpp", "src/voice/rtp.cpp", "src/voice/jitter_buffer.cpp", "src/voice/opus_codec.cpp")
    add_includedirs("src")
    if win32 then
        add_packages("libopus")
    else
        add_packages("pkgconfig::opus")
    end
    add_links("m", "pthread")
    set_warnings("all", "extra")
