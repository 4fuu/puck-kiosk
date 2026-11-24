-- Builds the Windows icon and its resource script from the fetched 2D Puck
-- sprite. tools/make-icon renders the icon; the sprite (assets/cache) and the
-- rendered files (.cache) are gitignored, so no Amp art is committed.
--
-- Called by the puck-kiosk.win32.icon rule in xmake.lua. Returns nil when the
-- sprite, Python, or ImageMagick is unavailable; the build then has no icon
-- resource and still succeeds.

import("lib.detect.find_tool")

function main()
    local root = os.projectdir()
    local sprites = os.files(path.join(root, "assets", "cache", "avatars", "classic-*.png"))
    local sprite = sprites[1]
    if not sprite then
        cprint("${color.warning}note:${clear} no fetched 2D sprite, building without an icon (run tools/fetch-assets)")
        return nil
    end

    local outdir = path.join(root, ".cache", "win32")
    local icofile = path.join(outdir, "puck-kiosk.ico")
    local rcfile = path.join(outdir, "puck-kiosk.rc")
    os.mkdir(outdir)

    local script = path.join(root, "tools", "make-icon")
    if not os.isfile(icofile) or os.mtime(icofile) < os.mtime(sprite) or os.mtime(icofile) < os.mtime(script) then
        local python = find_tool("python3") or find_tool("python")
        if not python then
            cprint("${color.warning}note:${clear} Python not found, building without an icon")
            return nil
        end
        local ok = try {
            function ()
                os.vrunv(python.program, {script, "--sprite", sprite, "--output", icofile})
                return true
            end
        }
        if not ok or not os.isfile(icofile) then
            cprint("${color.warning}note:${clear} cannot render the app icon, building without one")
            return nil
        end
    end

    local iconpath = path.absolute(icofile)
    if is_host("windows") then
        iconpath = path.translate(iconpath, "\\")
    end
    -- The comment carries the icon timestamp so a re-rendered icon changes the
    -- resource script and windres/rc rebuilds the resource.
    local content = string.format('// icon timestamp: %d\nIDI_ICON1 ICON "%s"\n',
                                  math.floor(os.mtime(icofile)), iconpath)
    if not os.isfile(rcfile) or io.readfile(rcfile) ~= content then
        io.writefile(rcfile, content)
    end
    return {rcfile = rcfile, icofile = icofile}
end
