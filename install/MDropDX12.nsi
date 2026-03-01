; MDropDX12.nsi
; NSIS installer script

!include "MUI2.nsh"

!define MUI_ABORTWARNING

!define MUI_ICON "..\resources\icons\MDropDX12.ico"
!define MUI_UNICON "..\resources\icons\MDropDX12.ico"

!define VERSION "1.1"
!define VER_MAJOR 1
!define VER_MINOR 1

!define RELDIR "..\Release\"

!define REG_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\MDropDX12"

Name "MDropDX12 ${VERSION}"
OutFile "MDropDX12-${VERSION}-Installer.exe"
InstallDir "C:\Tools\MDropDX12"

RequestExecutionLevel user

; Page defines
!define MUI_COMPONENTSPAGE_NODESC

!define MUI_DIRECTORYPAGE_TEXT_TOP "MDropDX12 needs FULL WRITE ACCESS to its directory! Do NOT install into $\"Program Files$\" or a similar protected location."

!define MUI_FINISHPAGE_RUN "$INSTDIR\MDropDX12.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run MDropDX12 now!"

!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Show README.txt"

; Installer Pages
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; Unistaller Pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

; Languages
!insertmacro MUI_LANGUAGE "English"

; Installer Sections
Section "MDropDX12" SecMDropDX12

  SectionIn RO ;Make it read-only

  CopyFiles $INSTDIR\*.ini $INSTDIR\backup

  SetOverwrite try

  SetOutPath "$INSTDIR\resources\data\"
  File /r "${RELDIR}resources\data\*"

  SetOutPath "$INSTDIR\resources\docs\"
  File /r "${RELDIR}resources\docs\*"

  SetOutPath "$INSTDIR\resources\presets\MDropDX12\"
  File "${RELDIR}resources\presets\MDropDX12\*"

  SetOutPath "$INSTDIR\resources\presets\MDropDX12\Shader\"
  File "${RELDIR}resources\presets\MDropDX12\Shader\*"

  SetOutPath "$INSTDIR\resources\sprites\"
  File /r "${RELDIR}resources\sprites\*"

  SetOutPath "$INSTDIR\resources\textures\"
  File /r "${RELDIR}resources\textures\*"

  SetOutPath "$INSTDIR"
  File "${RELDIR}MDropDX12.exe"
  File "${RELDIR}README.txt"
  SetOverwrite off
  File "${RELDIR}script-default.txt"
  File "${RELDIR}midi-default.txt"
  File "${RELDIR}messages.ini"
  File "${RELDIR}settings.ini"
  File "${RELDIR}sprites.ini"
  File "${RELDIR}precompile.txt"
  SetOverwrite on

  SetOutPath $INSTDIR

  ;Store installation folder
  WriteRegStr HKCU "Software\MDropDX12" "" $INSTDIR

  WriteRegStr HKCU "${REG_UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${REG_UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr HKCU "${REG_UNINST_KEY}" "InstallLocation" "$INSTDIR"

  WriteRegStr HKCU "${REG_UNINST_KEY}" "DisplayName" "MDropDX12 Visualizer"
  WriteRegStr HKCU "${REG_UNINST_KEY}" "DisplayIcon" "$INSTDIR\MDropDX12.exe"

  WriteRegStr HKCU "${REG_UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegDWORD HKCU "${REG_UNINST_KEY}" "VersionMajor" "${VER_MAJOR}"
  WriteRegDWORD HKCU "${REG_UNINST_KEY}" "VersionMinor" "${VER_MINOR}"

  WriteRegStr HKCU "${REG_UNINST_KEY}" "Publisher" "IkeC and Contributors"
  WriteRegStr HKCU "${REG_UNINST_KEY}" "URLInfoAbout" "https://github.com/shanevbg/MDropDX12"

  WriteRegDWORD HKCU "${REG_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${REG_UNINST_KEY}" "NoRepair" 1
  ${MakeARPInstallDate} $1
  WriteRegStr HKCU "${REG_UNINST_KEY}" "InstallDate" $1

  ;Create uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Additional Presets" SecPresets

  SetOutPath "$INSTDIR"
  SetOverwrite try

  SetOutPath "$INSTDIR\resources\presets\BeatDrop"
  File /r "${RELDIR}resources\presets\BeatDrop\*.*"

  SetOutPath "$INSTDIR\resources\presets\Butterchurn"
  File /r "${RELDIR}resources\presets\Butterchurn\*.*"

  SetOutPath "$INSTDIR\resources\presets\Incubo_"
  File /r "${RELDIR}resources\presets\Incubo_\*.*"

  SetOutPath "$INSTDIR\resources\presets\Incubo_ Picks"
  File /r "${RELDIR}resources\presets\Incubo_ Picks\*.*"

  SetOutPath "$INSTDIR\resources\presets\Milkdrop2077"
  File /r "${RELDIR}resources\presets\Milkdrop2077\*.*"

  SetOutPath "$INSTDIR"
SectionEnd

Section "Start menu items"
  ;Create shortcuts
  CreateShortcut "$SMPROGRAMS\MDropDX12 Visualizer.lnk" "$INSTDIR\MDropDX12.exe"
  CreateShortcut "$SMPROGRAMS\MDropDX12 Uninstall.lnk" "$INSTDIR\Uninstall.exe"
SectionEnd

Section "Desktop shortcuts"
  CreateShortcut "$Desktop\MDropDX12 Visualizer.lnk" "$INSTDIR\MDropDX12.exe"
SectionEnd

; Uninstaller
Section Uninstall

  RMDir /r "$INSTDIR\resources"
  RMDir /r "$INSTDIR\backup"
  RMDir /r "$INSTDIR\log"

  Delete "$INSTDIR\MDropDX12.exe"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\script-default.txt"
  Delete "$INSTDIR\midi-default.txt"
  Delete "$INSTDIR\messages.ini"
  Delete "$INSTDIR\settings.ini"
  Delete "$INSTDIR\sprites.ini"
  Delete "$INSTDIR\precompile.txt"

  Delete "$INSTDIR\Uninstall.exe"

  RMDir $INSTDIR

  Delete "$SMPROGRAMS\MDropDX12*.lnk"
  Delete "$Desktop\MDropDX12 Visualizer.lnk"


  DeleteRegKey HKCU "${REG_UNINST_KEY}"
  DeleteRegKey HKCU "Software\MDropDX12"

SectionEnd
