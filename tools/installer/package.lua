-- Compiles the per-user NSIS installer for the win32 target.
--
-- xmake's built-in nsis plugin only runs on a Windows host, so this module
-- substitutes the ${PACKAGE_*} spec variables in tools/installer/puck-kiosk.nsi
-- and invokes makensis itself. The same template and command work on Windows
-- and on the Linux mingw cross build.

-- The NSIS installer does not put makensis.exe on PATH.
function _find_makensis()
    local windows = is_host("windows")
    local name = windows and "makensis.exe" or "makensis"
    local candidates = {}
    for _, dir in ipairs(path.splitenv(os.getenv("PATH") or "")) do
        dir = dir:gsub('^"(.*)"$', "%1")
        table.insert(candidates, path.join(dir, name))
    end
    if windows then
        for _, root in ipairs({os.getenv("ProgramFiles(x86)"), os.getenv("ProgramFiles")}) do
            if root then
                table.insert(candidates, path.join(root, "NSIS", name))
            end
        end
    end
    for _, candidate in ipairs(candidates) do
        if os.isfile(candidate) and (windows or os.isexec(candidate)) then
            return candidate
        end
    end
end

-- Replace ${NAME} with the xpack spec variables. Unknown placeholders such as
-- ${If} or ${GetSize} are left for NSIS.
function _resolve_specfile(template, specvars, output)
    os.cp(template, output)
    -- xmake's Lua passes only the first capture to a gsub callback.
    local names = {}
    io.gsub(output, "%${([^\n]-)}", function (name)
        table.insert(names, name)
    end)
    local values = {}
    for _, name in ipairs(names) do
        name = name:trim()
        if values[name] == nil then
            local value = specvars[name]
            if type(value) == "function" then
                value = value()
            end
            values[name] = value
        end
    end
    io.gsub(output, "%${([^\n]-)}", function (name)
        return values[name:trim()]
    end)
end

function main(package)
    if not package:is_plat("windows", "mingw") then
        raise("the nsis installer needs a Windows target: xmake f -p mingw -a x64 -m release --backend=win32")
    end

    local target = package:target("puck-kiosk")
    local exefile = target and target:targetfile()
    if not exefile or not os.isfile(exefile) then
        raise("puck-kiosk.exe is missing; run xmake build first")
    end

    local makensis = _find_makensis()
    if not makensis then
        raise("makensis not found: install NSIS (Debian/Ubuntu: sudo apt install nsis) or add it to PATH")
    end

    -- Optional: the icon exists only when the 2D sprite has been fetched. The
    -- template falls back to the stock NSIS icon when it is empty.
    local icon = import("tools.installer.icon", {rootdir = os.projectdir()})()
    local iconfile = icon and path.absolute(icon.icofile) or ""

    local qrencode_dll = ""
    if package:is_plat("windows") then
        local qrencode = target:pkg("libqrencode")
        qrencode_dll = os.files(path.join(qrencode:installdir(), "*", "qrencode.dll"))[1]
        if not qrencode_dll then
            raise("qrencode.dll is missing from the native Windows libqrencode package")
        end
    end

    local specvars = package:specvars()
    specvars.PACKAGE_EXEFILE = path.absolute(exefile)
    specvars.PACKAGE_OUTPUTFILE = path.absolute(package:outputfile())
    specvars.PACKAGE_ICONFILE = iconfile
    specvars.PACKAGE_LICENSEFILE = path.absolute(package:get("licensefile"))
    specvars.PACKAGE_SPEEX_LICENSEFILE =
        path.absolute(path.join(os.projectdir(), "third_party", "speexdsp", "COPYING"))
    specvars.PACKAGE_QRENCODEFILE = qrencode_dll
    specvars.PACKAGE_QRENCODE_LICENSEFILE =
        path.absolute(path.join(os.projectdir(), "third_party", "libqrencode", "COPYING"))
    specvars.PACKAGE_MANIFESTFILE = path.absolute(path.join(os.projectdir(), "assets", "manifest.json"))
    -- Bundled art is optional: pack whatever tools/fetch-assets has cached so
    -- an installed copy needs no first-run download. The repo never commits it.
    local cachedir = path.join(os.projectdir(), "assets", "cache")
    specvars.PACKAGE_ASSETSDIR = os.isdir(cachedir) and path.absolute(cachedir) or ""

    os.mkdir(package:builddir())
    os.mkdir(package:outputdir())
    local specfile = path.join(package:builddir(), package:basename() .. ".nsi")
    _resolve_specfile(path.absolute(package:get("specfile")), specvars, specfile)

    cprint("packing %s", package:outputfile())
    os.vrunv(makensis, {specfile})
end
