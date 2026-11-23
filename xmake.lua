set_project("puck-kiosk")
set_version("0.1.0")
set_languages("c++17")
set_encodings("utf-8")

add_rules("mode.debug", "mode.release")

add_requires("ixwebsocket v12.1.0", {configs = {ssl = "openssl", use_tls = true}})
add_requires("libdatachannel 0.24.6", {configs = {media = true}})
add_requires("pkgconfig::webrtc-audio-processing-1")
add_requires("pkgconfig::alsa")
add_requires("pkgconfig::opus")

option("backend")
    set_default("headless")
    set_values("headless", "drm", "wayland", "x11")
    set_description("Display/input backend to build")
option_end()

option("puck3d")
    set_default(true)
    set_description("Enable the Puck 3D renderer (falls back to 2D when unavailable)")
option_end()

local backend = get_config("backend") or "headless"
local puck3d = get_config("puck3d")
if puck3d == nil then puck3d = true end

if backend == "drm" then
    add_requires("pkgconfig::libdrm")
    add_requires("pkgconfig::gbm")
    add_requires("pkgconfig::libudev")
    -- Optional: the evdev fallback covers builds without libinput.
    add_requires("pkgconfig::libinput", {optional = true})
end

target("nanovg")
    set_kind("static")
    add_files("third_party/nanovg/nanovg.c", "third_party/nanovg/nanovg_gles3.c")
    add_includedirs("third_party/nanovg", { public = true })
    set_warnings("none")

target("puck-kiosk")
    set_kind("binary")
    set_default(true)
    set_rundir("$(projectdir)")
    add_deps("nanovg")
    add_files("src/**.cpp")
    if backend == "drm" then
        add_files("src/platform/drm/*.cpp")
        if not has_package("pkgconfig::libinput") then
            remove_files("src/platform/drm/libinput_input.cpp")
        end
    else
        remove_files("src/platform/drm/**.cpp")
    end
    add_includedirs("src", "third_party")
    add_packages("ixwebsocket", "libdatachannel", "pkgconfig::webrtc-audio-processing-1",
                 "pkgconfig::alsa", "pkgconfig::opus")
    if backend == "drm" then
        add_packages("pkgconfig::libdrm", "pkgconfig::gbm", "pkgconfig::libudev")
        if has_package("pkgconfig::libinput") then
            add_packages("pkgconfig::libinput")
            add_defines("PUCK_KIOSK_HAVE_LIBINPUT=1")
        end
    end
    add_links("EGL", "GLESv2", "m", "dl", "pthread", "png", "webp", "jpeg", "curl", "qrencode", "ssl",
              "crypto")
    add_defines("PUCK_KIOSK_BACKEND_" .. backend:upper() .. "=1")
    add_defines("PUCK_KIOSK_ENABLE_AEC=1")
    add_defines("PUCK_KIOSK_ENABLE_ALSA=1")
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
              "src/app/plugin_ui.cpp", "src/app/passkey.cpp", "src/app/diagnostics.cpp",
              "src/app/media_overlay.cpp", "src/core/log.cpp")
    add_includedirs("src", "third_party")
    add_links("m", "pthread", "png", "webp", "jpeg")
    set_warnings("all", "extra")

target("voice-tests")
    set_kind("binary")
    set_default(false)
    set_rundir("$(projectdir)")
    add_files("tests/voice_tests.cpp", "src/voice/rtp.cpp", "src/voice/jitter_buffer.cpp", "src/voice/opus_codec.cpp")
    add_includedirs("src")
    add_packages("pkgconfig::opus")
    add_links("m", "pthread")
    set_warnings("all", "extra")
