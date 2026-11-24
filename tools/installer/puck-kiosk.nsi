; Puck Kiosk per-user Windows installer.
;
; Compiled by the xpack("puck-kiosk") package script in xmake.lua, which
; replaces the ${PACKAGE_*} placeholders and runs makensis. Do not compile it
; by hand: the placeholders have no defaults.
;
; Design constraints:
;   - Per-user install, no administrator rights and no UAC prompt.
;   - Start Menu shortcut always, desktop shortcut as an installer option.
;   - Shortcuts start puck-kiosk.exe with no arguments and $INSTDIR as the
;     working directory (NSIS stores $OUTDIR in the shortcut).
;   - Uninstaller registered under HKCU for Settings > Apps.
;   - The app keeps its cookie jar and saved car session in
;     %LOCALAPPDATA%\puck-kiosk, outside the install directory; uninstall
;     leaves that user data alone.
;   - The executable, the MIT license text, and (when the sprite has been
;     fetched) the icon are installed. The icon is also compiled into
;     puck-kiosk.exe at build time; no models, textures, or fonts are
;     installed. Uninstall removes the re-downloadable assets/cache.

; The icon comes from the same sprite-derived .ico as the executable. It is
; optional: a build without tools/fetch-assets falls back to the executable's
; default icon and the stock NSIS installer icon.
!if "${PACKAGE_ICONFILE}" != ""
    !define MUI_ICON "${PACKAGE_ICONFILE}"
    !define MUI_UNICON "${PACKAGE_ICONFILE}"
    !define APP_ICON "$INSTDIR\puck-kiosk.ico"
!else
    !define APP_ICON "$INSTDIR\puck-kiosk.exe"
!endif

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

Unicode true

!define APP_ID       "puck-kiosk"
!define APP_EXE      "puck-kiosk.exe"
!define APP_NAME     "Puck Kiosk"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
!define SETTINGS_KEY  "Software\${APP_ID}"

Name "${PACKAGE_TITLE}"
OutFile "${PACKAGE_OUTPUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_ID}"
InstallDirRegKey HKCU "${SETTINGS_KEY}" "InstallDir"

; Per-user only: this manifest also covers the uninstaller.
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
SetDatablockOptimize ON
ManifestDPIAware true
ManifestSupportedOS all

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Install ${PACKAGE_TITLE}"
!define MUI_WELCOMEPAGE_TEXT "This installs ${PACKAGE_TITLE} for the current user. Administrator rights are not required.$\r$\n$\r$\nThe installer adds a Start Menu shortcut, an optional desktop shortcut, and an uninstaller."
!define MUI_FINISHPAGE_TITLE "${PACKAGE_TITLE} is installed"
!define MUI_FINISHPAGE_TEXT "${PACKAGE_TITLE} is ready. Start it from the Start Menu.$\r$\n$\r$\nUninstall it from Settings > Apps, or from the Start Menu folder."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PACKAGE_LICENSEFILE}"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${PACKAGE_VERSION_MAJOR}.${PACKAGE_VERSION_MINOR}.${PACKAGE_VERSION_ALTER}.0"
VIFileVersion    "${PACKAGE_VERSION_MAJOR}.${PACKAGE_VERSION_MINOR}.${PACKAGE_VERSION_ALTER}.0"
VIAddVersionKey /LANG=0 "ProductName"      "${PACKAGE_TITLE}"
VIAddVersionKey /LANG=0 "ProductVersion"   "${PACKAGE_VERSION}"
VIAddVersionKey /LANG=0 "FileDescription"  "${PACKAGE_TITLE} installer"
VIAddVersionKey /LANG=0 "FileVersion"      "${PACKAGE_VERSION}"
VIAddVersionKey /LANG=0 "CompanyName"      "${PACKAGE_COMPANY}"
VIAddVersionKey /LANG=0 "LegalCopyright"   "${PACKAGE_COPYRIGHT}"
VIAddVersionKey /LANG=0 "OriginalFilename" "${PACKAGE_FILENAME}"

; Keep the directory page honest: a folder the user cannot write to would fail
; later with a cryptic error, because the installer never elevates.
Function .onVerifyInstDir
    ClearErrors
    CreateDirectory "$INSTDIR"
    FileOpen $0 "$INSTDIR\.write-test" w
    IfErrors unwritable
    FileClose $0
    Delete "$INSTDIR\.write-test"
    Return
unwritable:
    MessageBox MB_ICONSTOP|MB_OK "${PACKAGE_TITLE} installs for the current user without administrator rights. Choose a folder you can write to, such as $LOCALAPPDATA\Programs\${APP_ID}."
    Abort
FunctionEnd

Section "${APP_NAME}" SecApp
    SectionIn RO
    SetShellVarContext current
    SetOutPath "$INSTDIR"
    SetOverwrite on

    File "/oname=${APP_EXE}" "${PACKAGE_EXEFILE}"
    File "/oname=LICENSE.txt" "${PACKAGE_LICENSEFILE}"
!if "${PACKAGE_ICONFILE}" != ""
    File "/oname=puck-kiosk.ico" "${PACKAGE_ICONFILE}"
!endif
    ; The manifest is URL/hash metadata only; the art itself is fetched on
    ; first run into assets/cache under the install directory.
    SetOutPath "$INSTDIR\assets"
    File "/oname=manifest.json" "${PACKAGE_MANIFESTFILE}"
!if "${PACKAGE_ASSETSDIR}" != ""
    ; Fetched art present at pack time ships with the installer so the first
    ; launch needs no download; the in-app fetcher remains the fallback.
    SetOutPath "$INSTDIR\assets\cache"
    File /r "${PACKAGE_ASSETSDIR}\*.*"
!endif
    SetOutPath "$INSTDIR"

    WriteUninstaller "$INSTDIR\uninstall.exe"

    ; CreateShortCut stores $OUTDIR as the shortcut's working directory, so the
    ; shortcuts start the app inside its install directory with no arguments.
    SetOutPath "$INSTDIR"
    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "${APP_ICON}" 0 SW_SHOWNORMAL "" "Start ${APP_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"

    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName"     "${PACKAGE_TITLE}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion"  "${PACKAGE_VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher"       "${PACKAGE_COMPANY}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon"     "${APP_ICON}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "HelpLink"        "${PACKAGE_HOMEPAGE}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout"    "${PACKAGE_HOMEPAGE}"
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" $0

    WriteRegStr HKCU "${SETTINGS_KEY}" "InstallDir" "$INSTDIR"
SectionEnd

Section "Desktop shortcut" SecDesktop
    SetShellVarContext current
    SetOutPath "$INSTDIR"
    CreateShortCut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "${APP_ICON}" 0 SW_SHOWNORMAL "" "Start ${APP_NAME}"
SectionEnd

LangString DESC_SecApp     ${LANG_ENGLISH} "${PACKAGE_TITLE} application, Start Menu shortcut, and uninstaller."
LangString DESC_SecDesktop ${LANG_ENGLISH} "Add a ${PACKAGE_TITLE} shortcut to the desktop."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} $(DESC_SecApp)
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_SecDesktop)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    SetShellVarContext current

    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"
    Delete "$DESKTOP\${APP_NAME}.lnk"

    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\puck-kiosk.ico"
    Delete "$INSTDIR\LICENSE.txt"
    Delete "$INSTDIR\assets\manifest.json"
    ; The art is re-downloadable, so the cache goes with the program. The
    ; session in %LOCALAPPDATA%\puck-kiosk is user data and stays.
    RMDir /r "$INSTDIR\assets\cache"
    RMDir "$INSTDIR\assets"
    ; Older builds kept the session store in the working directory.
    RMDir /r "$INSTDIR\build\puck-kiosk"
    RMDir "$INSTDIR\build"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"

    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    DeleteRegKey HKCU "${SETTINGS_KEY}"
SectionEnd
