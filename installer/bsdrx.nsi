; bsdrX — Bigscreen Remote Desktop agent — Windows installer (NSIS).
; Build with: makensis -DVERSION=0.950.2 installer/bsdrx.nsi
; The build-installer.sh script stages bsdr_agent.exe + FFmpeg DLLs + assets
; into installer/stage/ and (if present) VB-CABLE into installer/stage/vbcable/.

Unicode true
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.950.2"
!endif
!define APPNAME   "bsdrX Remote Desktop"
!define COMPANY   "Nexlab"
!define EXENAME   "bsdr_agent.exe"
!define STAGE     "stage"

Name "${APPNAME}"
OutFile "bsdrX-Setup-${VERSION}.exe"
InstallDir "$PROGRAMFILES64\bsdrX"
InstallDirRegKey HKLM "Software\bsdrX" "InstallDir"
RequestExecutionLevel admin

VIProductVersion "0.9.50.2"
VIAddVersionKey  "ProductName"   "${APPNAME}"
VIAddVersionKey  "CompanyName"   "${COMPANY}"
VIAddVersionKey  "FileVersion"   "${VERSION}"
VIAddVersionKey  "FileDescription" "bsdrX Remote Desktop installer"
VIAddVersionKey  "LegalCopyright" "(C) 2026 Stefy Lanza. GPLv3."

!define MUI_ICON   "..\assets\bsdrx.ico"
!define MUI_UNICON "..\assets\bsdrx.ico"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch bsdrX (opens the control panel in your browser)"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------- install ---
Section "bsdrX agent (required)" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${STAGE}\${EXENAME}"
  File "${STAGE}\bsdrx.ico"
  File "${STAGE}\rename-mic.ps1"
  ; FFmpeg runtime DLLs (capture + H.264 encode)
  File "${STAGE}\*.dll"

  WriteRegStr HKLM "Software\bsdrX" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\bsdrX" "Version" "${VERSION}"

  ; Start Menu + Desktop shortcuts (with our icon)
  CreateDirectory "$SMPROGRAMS\bsdrX"
  CreateShortcut "$SMPROGRAMS\bsdrX\bsdrX Remote Desktop.lnk" "$INSTDIR\${EXENAME}" "" "$INSTDIR\bsdrx.ico"
  CreateShortcut "$DESKTOP\bsdrX Remote Desktop.lnk" "$INSTDIR\${EXENAME}" "" "$INSTDIR\bsdrx.ico"

  ; Allow the agent through the Windows firewall (LAN discovery/control/media)
  nsExec::Exec 'netsh advfirewall firewall add rule name="bsdrX" dir=in action=allow program="$INSTDIR\${EXENAME}" enable=yes'

  ; Uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\bsdrX" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\bsdrX" "DisplayIcon" "$INSTDIR\bsdrx.ico"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\bsdrX" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\bsdrX" "Publisher" "${COMPANY}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\bsdrX" "UninstallString" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Virtual microphone — BSRD_Mic (VB-CABLE)" SecMic
  ; Bundled VB-CABLE driver (staged only when vendor files are present).
!ifdef VBCABLE
  SetOutPath "$INSTDIR\vbcable"
  File /r "${STAGE}\vbcable\*.*"
  DetailPrint "Installing VB-CABLE virtual audio driver..."
  ; -i auto-installs the driver (a Windows driver-trust prompt may appear).
  ExecWait '"$INSTDIR\vbcable\VBCABLE_Setup_x64.exe" -i' $0
  DetailPrint "VB-CABLE installer returned $0"
!else
  DetailPrint "VB-CABLE not bundled; skipping. Drop VBCABLE_Setup_x64.exe into installer/vendor/vbcable and rebuild."
!endif
  ; Rename the VB-CABLE endpoints to BSRD_Mic (works whether bundled here or
  ; pre-installed). PowerShell, elevated.
  DetailPrint "Naming the virtual microphone 'BSRD_Mic'..."
  nsExec::ExecToLog 'powershell -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\rename-mic.ps1" -NewName "BSRD_Mic"'
SectionEnd

LangString DESC_SecCore ${LANG_ENGLISH} "The bsdrX agent, FFmpeg runtime, and shortcuts."
LangString DESC_SecMic  ${LANG_ENGLISH} "Install the VB-CABLE virtual audio driver and name it BSRD_Mic so the Quest microphone appears as a system mic."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} $(DESC_SecCore)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMic}  $(DESC_SecMic)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; -------------------------------------------------------------- uninstall ---
Section "Uninstall"
  nsExec::Exec 'netsh advfirewall firewall delete rule name="bsdrX"'
  Delete "$SMPROGRAMS\bsdrX\bsdrX Remote Desktop.lnk"
  RMDir  "$SMPROGRAMS\bsdrX"
  Delete "$DESKTOP\bsdrX Remote Desktop.lnk"
  Delete "$INSTDIR\${EXENAME}"
  Delete "$INSTDIR\bsdrx.ico"
  Delete "$INSTDIR\rename-mic.ps1"
  Delete "$INSTDIR\*.dll"
  RMDir /r "$INSTDIR\vbcable"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\bsdrX"
  DeleteRegKey HKLM "Software\bsdrX"
  ; Note: VB-CABLE is left installed (shared driver); remove it from
  ; "Apps & features" if desired.
SectionEnd
