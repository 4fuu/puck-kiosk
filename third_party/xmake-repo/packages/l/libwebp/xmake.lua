package("libwebp")
    set_homepage("https://developers.google.com/speed/webp")
    set_description("WebP image codec")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/webmproject/libwebp/archive/refs/tags/$(version).tar.gz")
    add_versions("v1.6.0", "93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136")
    add_deps("cmake")

    on_load(function (package)
        local prefix = package:is_plat("windows") and "lib" or ""
        package:add("links", prefix .. "webp", prefix .. "sharpyuv")
    end)

    on_install("windows", "mingw", function (package)
        local configs = {"-DBUILD_SHARED_LIBS=OFF"}
        for _, tool in ipairs({"ANIM_UTILS", "CWEBP", "DWEBP", "GIF2WEBP", "IMG2WEBP",
                               "VWEBP", "WEBPINFO", "LIBWEBPMUX", "WEBPMUX", "EXTRAS"}) do
            table.insert(configs, "-DWEBP_BUILD_" .. tool .. "=OFF")
        end
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("WebPGetDecoderVersion", {includes = "webp/decode.h"}))
    end)
