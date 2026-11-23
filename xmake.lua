set_project("puck-kiosk")
set_version("0.1.0")
set_languages("c++17")
set_encodings("utf-8")

add_rules("mode.debug", "mode.release")

add_requires("ixwebsocket", {configs = {ssl = "openssl", use_tls = true}})

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
    add_includedirs("src", "third_party")
    add_packages("ixwebsocket")
    add_links("EGL", "GLESv2", "m", "dl", "pthread", "curl", "qrencode", "ssl", "crypto")
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
    add_files("tests/**.cpp")
    add_files("src/puck/puck_v2.cpp", "src/puck/puck_rig.cpp")
    add_includedirs("src", "third_party")
    add_links("m")
    set_warnings("all", "extra")
