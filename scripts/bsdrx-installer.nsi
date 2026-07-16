; bsdrX Windows installer (NSIS). Built by scripts/build-win-bundle.sh from the staged portable payload:
;   makensis -DVERSION=<v> -DPAYLOAD=<stage-dir> -DOUTFILE=<setup.exe> scripts/bsdrx-installer.nsi
; Produces a self-contained Setup .exe: installs bsdr_agent.exe + its DLLs into Program Files, adds a
; Start-Menu shortcut + uninstaller, and FACILITATES the optional external pieces (Npcap for the owner-mic
; sniffer / packet capture, VB-CABLE for the virtual microphone) by opening their official download pages —
; we don't redistribute those third-party installers.
Unicode true
!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef PAYLOAD
  !define PAYLOAD "bsdrX"
!endif
!ifndef OUTFILE
  !define OUTFILE "bsdrX-Setup.exe"
!endif

Name "bsdrX ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\bsdrX"
InstallDirRegKey HKLM "Software\bsdrX" "InstallDir"
RequestExecutionLevel admin
BrandingText "bsdrX ${VERSION}"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PAYLOAD}\LICENSE.md"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\bsdr_agent.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch bsdrX now"
!define MUI_FINISHPAGE_NOAUTOCLOSE
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "bsdrX (required)" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${PAYLOAD}\*.*"
  WriteRegStr HKLM "Software\bsdrX" "InstallDir" "$INSTDIR"
  CreateDirectory "$SMPROGRAMS\bsdrX"
  CreateShortcut "$SMPROGRAMS\bsdrX\bsdrX.lnk" "$INSTDIR\bsdr_agent.exe"
  CreateShortcut "$SMPROGRAMS\bsdrX\Uninstall bsdrX.lnk" "$INSTDIR\uninstall.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  !define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\bsdrX"
  WriteRegStr HKLM "${UNKEY}" "DisplayName" "bsdrX ${VERSION}"
  WriteRegStr HKLM "${UNKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNKEY}" "Publisher" "nexlab"
  WriteRegStr HKLM "${UNKEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegDWORD HKLM "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNKEY}" "NoRepair" 1
SectionEnd

; Optional external pieces. These OPEN the vendor download page (checked to run at the end of install);
; the agent's own control panel also explains them. Not bundled — third-party redistribution terms.
Section "Get Npcap  (owner-mic sniffer / packet capture)" SecNpcap
  ExecShell "open" "https://npcap.com/#download"
SectionEnd
Section "Get VB-CABLE  (virtual microphone)" SecVBCable
  ExecShell "open" "https://vb-audio.com/Cable/"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "The bsdrX agent and the libraries it needs. Required."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecNpcap} "Opens the Npcap download page. Optional: the agent runs (remote desktop + relay owner mic) without it; Npcap adds the promiscuous owner-mic sniffer + ARP MITM (wpcap.dll, loaded at runtime)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecVBCable} "Opens the VB-CABLE download page. Provides the virtual microphone the owner-mic routing and voice computer-control feed."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR"
  Delete "$SMPROGRAMS\bsdrX\bsdrX.lnk"
  Delete "$SMPROGRAMS\bsdrX\Uninstall bsdrX.lnk"
  RMDir "$SMPROGRAMS\bsdrX"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\bsdrX"
  DeleteRegKey HKLM "Software\bsdrX"
SectionEnd
