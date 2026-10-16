Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!ifndef VERSION
  !error "VERSION is required"
!endif
Name "本地归档 ${VERSION}"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\LocalArchiver"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
BrandingText "Local Archiver"
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\local-archiver.exe"
!define MUI_FINISHPAGE_RUN_TEXT "启动本地归档"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "SimpChinese"

Var Integrate
Function .onInit
  StrCpy $Integrate "1"
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/NOINTEGRATION" $1
  ${IfNot} ${Errors}
    StrCpy $Integrate "0"
  ${EndIf}
FunctionEnd

Section "本地归档" Main
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteINIStr "$INSTDIR\install-state.ini" "Setup" "Integration" "$Integrate"
  ${If} $Integrate == "1"
    SetShellVarContext current
    CreateDirectory "$SMPROGRAMS\本地归档"
    CreateShortCut "$SMPROGRAMS\本地归档\本地归档.lnk" "$INSTDIR\local-archiver.exe"
    CreateShortCut "$SMPROGRAMS\本地归档\卸载.lnk" "$INSTDIR\uninstall.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver" "DisplayName" "本地归档"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver" "DisplayIcon" "$INSTDIR\local-archiver.exe"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver" "NoRepair" 1
  ${EndIf}
SectionEnd

Section "Uninstall"
  ReadINIStr $Integrate "$INSTDIR\install-state.ini" "Setup" "Integration"
  ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver" "InstallLocation"
  ${If} $Integrate == "1"
  ${AndIf} $0 == $INSTDIR
    SetShellVarContext current
    Delete "$SMPROGRAMS\本地归档\本地归档.lnk"
    Delete "$SMPROGRAMS\本地归档\卸载.lnk"
    RMDir "$SMPROGRAMS\本地归档"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalArchiver"
  ${EndIf}
  !include "${UNINSTALL_LIST}"
  Delete "$INSTDIR\install-state.ini"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
