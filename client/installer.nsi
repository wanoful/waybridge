Unicode true
Name "Waybridge"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Waybridge"
RequestExecutionLevel user
SetCompressor /SOLID lzma
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Waybridge"
VIAddVersionKey "FileDescription" "Waybridge Setup (${ARCH})"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Waybridge contributors"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

LangString WrongSystem ${LANG_ENGLISH} "This installer requires 64-bit Windows 10 or later on a native ${ARCH} system."
LangString WrongSystem ${LANG_SIMPCHINESE} "此安装包需要 ${ARCH} 架构的 64 位 Windows 10 或更新版本。"
LangString AppRunning ${LANG_ENGLISH} "Exit Waybridge from its tray menu and click Retry. Your Linux session will stay running."
LangString AppRunning ${LANG_SIMPCHINESE} "请从托盘菜单退出 Waybridge，再点击重试。Linux 会话将继续运行。"
LangString InstallFailed ${LANG_ENGLISH} "Setup could not replace the application files. Check available disk space and file permissions, then try again."
LangString InstallFailed ${LANG_SIMPCHINESE} "安装程序无法替换应用文件。请检查磁盘空间和文件权限后重试。"
LangString RedistPrompt ${LANG_ENGLISH} "Install the Microsoft Visual C++ Runtime now? It may require administrator approval."
LangString RedistPrompt ${LANG_SIMPCHINESE} "现在安装 Microsoft Visual C++ 运行库吗？此操作可能需要管理员批准。"
LangString RedistFailed ${LANG_ENGLISH} "Could not start the Microsoft Visual C++ installer. You can run vc_redist.${ARCH}.exe from the Waybridge installation folder."
LangString RedistFailed ${LANG_SIMPCHINESE} "无法启动 Microsoft Visual C++ 安装程序。你可以从 Waybridge 的安装目录运行 vc_redist.${ARCH}.exe。"

!define UNINSTALL "Software\Microsoft\Windows\CurrentVersion\Uninstall\Waybridge"
!define PROTOCOL "Software\Classes\waybridge"
!define APP "$INSTDIR\app\Waybridge.Control.exe"
!define HANDLER '$\"${APP}$\" --launch-link $\"%1$\"'

Var Stage

!macro EnsureClosed
  check_running:
    System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "Local\Waybridge.ControlCenter") p.r0'
    StrCmp $0 0 closed
    System::Call 'kernel32::CloseHandle(p r0)'
    IfSilent cancel_running
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(AppRunning)" IDRETRY check_running
  cancel_running:
    SetErrorLevel 2
    Abort
  closed:
!macroend

Function .onInit
  SetShellVarContext current
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\Waybridge"
  SetRegView 64
  !if "${ARCH}" == "arm64"
    ${Unless} ${AtLeastWin10}
    ${OrUnless} ${IsNativeARM64}
  !else
    ${Unless} ${AtLeastWin10}
    ${OrUnless} ${IsNativeAMD64}
  !endif
    IfSilent +2
    MessageBox MB_OK|MB_ICONSTOP "$(WrongSystem)"
    SetErrorLevel 2
    Abort
  ${EndUnless}
FunctionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\Waybridge"
FunctionEnd

Section "Waybridge"
  !insertmacro EnsureClosed

  CreateDirectory "$INSTDIR"
  ClearErrors
  GetTempFileName $Stage "$INSTDIR"
  IfErrors install_error
  Delete "$Stage"
  !include "${PAYLOAD}"
  IfErrors install_error
  WriteUninstaller "$Stage\Uninstall.exe"
  IfErrors install_error
  SetOutPath "$INSTDIR"

  ; Keep the installed files usable until the full new payload has been extracted.
  IfFileExists "$INSTDIR\previous\Uninstall.exe" 0 swap
    IfFileExists "${APP}" 0 restore
    RMDir /r "$INSTDIR\previous"
    Goto swap
  restore:
    Rename "$INSTDIR\previous" "$INSTDIR\app"
  swap:
    ClearErrors
    IfFileExists "${APP}" 0 promote
    IfFileExists "$INSTDIR\app\Uninstall.exe" 0 install_error
    Rename "$INSTDIR\app" "$INSTDIR\previous"
    IfErrors install_error
  promote:
    Rename "$Stage" "$INSTDIR\app"
    IfErrors restore_previous
    RMDir /r "$INSTDIR\previous"

  CreateDirectory "$SMPROGRAMS\Waybridge"
  SetOutPath "$INSTDIR\app"
  CreateShortcut "$SMPROGRAMS\Waybridge\Waybridge.lnk" "${APP}"
  WriteRegStr HKCU "${PROTOCOL}" "" "URL:Waybridge app link"
  WriteRegStr HKCU "${PROTOCOL}" "URL Protocol" ""
  WriteRegStr HKCU "${PROTOCOL}\DefaultIcon" "" '$\"${APP}$\",0'
  WriteRegStr HKCU "${PROTOCOL}\shell\open\command" "" '${HANDLER}'
  WriteRegStr HKCU "${UNINSTALL}" "DisplayName" "Waybridge"
  WriteRegStr HKCU "${UNINSTALL}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL}" "Publisher" "Waybridge contributors"
  WriteRegStr HKCU "${UNINSTALL}" "InstallLocation" "$INSTDIR\app"
  WriteRegStr HKCU "${UNINSTALL}" "DisplayIcon" "${APP}"
  WriteRegStr HKCU "${UNINSTALL}" "UninstallString" '$\"$INSTDIR\app\Uninstall.exe$\"'
  WriteRegStr HKCU "${UNINSTALL}" "QuietUninstallString" '$\"$INSTDIR\app\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "${UNINSTALL}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL}" "NoRepair" 1
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  IfSilent installed
  ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\${ARCH}" "Installed"
  StrCmp $0 1 installed
  MessageBox MB_YESNO|MB_ICONQUESTION "$(RedistPrompt)" IDNO installed
  ClearErrors
  ExecShell "runas" "$INSTDIR\app\vc_redist.${ARCH}.exe" "/passive /norestart"
  IfErrors 0 installed
  MessageBox MB_OK|MB_ICONEXCLAMATION "$(RedistFailed)"
  Goto installed

  restore_previous:
    Rename "$INSTDIR\previous" "$INSTDIR\app"
  install_error:
    SetOutPath "$INSTDIR"
    StrCmp $Stage "" +2
    RMDir /r "$Stage"
    IfSilent +2
    MessageBox MB_OK|MB_ICONSTOP "$(InstallFailed)"
    SetErrorLevel 2
    Abort
  installed:
SectionEnd

Section "Uninstall"
  !insertmacro EnsureClosed
  SetOutPath "$TEMP"
  RMDir /r "$INSTDIR\app"
  IfFileExists "${APP}" 0 removed
    SetErrorLevel 2
    Abort
  removed:
  IfFileExists "$INSTDIR\previous\Uninstall.exe" 0 +2
  RMDir /r "$INSTDIR\previous"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Waybridge\Waybridge.lnk"
  RMDir "$SMPROGRAMS\Waybridge"
  Delete "$LOCALAPPDATA\Microsoft\Windows Terminal\Fragments\Waybridge\profiles.json"
  RMDir "$LOCALAPPDATA\Microsoft\Windows Terminal\Fragments\Waybridge"
  ReadRegStr $0 HKCU "${PROTOCOL}\shell\open\command" ""
  StrCmp $0 '${HANDLER}' 0 +2
  DeleteRegKey HKCU "${PROTOCOL}"
  DeleteRegKey HKCU "${UNINSTALL}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
