; The installer's one screen. installer.nsi starts every install in Tauri's passive mode, so the only page
; that shows is the progress page; this turns it into a small window in the launcher's colors: the app's
; icon, its name, a line saying what is happening and the progress bar. Everything on it is a real
; control, so screen readers read it and it scales with the screen without pictures per size.

; The launcher's tokens: --bg, --ink, --muted; the bar is --online on --surface (as COLORREFs, BGR).
!define ME_TRACK 0x0028211E
!define ME_BAR 0x00FFB08F
!define ME_INK E8EAED
!define ME_MUTED 9AA1AC
!define ME_BGHEX 15171C
!define ME_W 440
!define ME_H 250

Var MEDpi
Var MEStatus

; Logical pixels to this screen's pixels.
!macro MEPX OUT N
  IntOp ${OUT} ${N} * $MEDpi
  IntOp ${OUT} ${OUT} / 96
!macroend

; What the status line says; the sections call it as they start. Harmless when nothing shows (silent).
!macro MESAY TEXT
  ${If} $MEStatus != ""
    SendMessage $MEStatus ${WM_SETTEXT} 0 "STR:${TEXT}"
  ${EndIf}
!macroend

!macro MEHIDE PARENT ID
  GetDlgItem $1 ${PARENT} ${ID}
  ShowWindow $1 ${SW_HIDE}
!macroend

Function MEInstShow
  System::Call 'user32::GetDpiForWindow(p $HWNDPARENT) i.r0'
  ${If} $0 = 0
    StrCpy $0 96
  ${EndIf}
  StrCpy $MEDpi $0
  ; A dark title bar, as the launcher has (Windows 10 20H1 and later; older ones ignore it).
  System::Call 'dwmapi::DwmSetWindowAttribute(p $HWNDPARENT, i 20, *i 1, i 4)'

  ; The wizard's frame goes: header, buttons, lines, branding.
  !insertmacro MEHIDE $HWNDPARENT 1
  !insertmacro MEHIDE $HWNDPARENT 2
  !insertmacro MEHIDE $HWNDPARENT 3
  !insertmacro MEHIDE $HWNDPARENT 1028
  !insertmacro MEHIDE $HWNDPARENT 1034
  !insertmacro MEHIDE $HWNDPARENT 1035
  !insertmacro MEHIDE $HWNDPARENT 1036
  !insertmacro MEHIDE $HWNDPARENT 1037
  !insertmacro MEHIDE $HWNDPARENT 1038
  !insertmacro MEHIDE $HWNDPARENT 1039
  !insertmacro MEHIDE $HWNDPARENT 1045
  !insertmacro MEHIDE $HWNDPARENT 1256

  ; A smaller window, kept where Windows centred the wizard.
  !insertmacro MEPX $6 ${ME_W}
  !insertmacro MEPX $7 ${ME_H}
  System::Call '*(i,i,i,i) p.r1'
  System::Call 'user32::GetWindowRect(p $HWNDPARENT, p r1)'
  System::Call '*$1(i.r2, i.r3, i.r4, i.r5)'
  System::Free $1
  System::Call '*(i 0, i 0, i r6, i r7) p.r1'
  System::Call 'user32::AdjustWindowRectExForDpi(p r1, i ${WS_CAPTION}|${WS_SYSMENU}, i 0, i 0, i $MEDpi)'
  System::Call '*$1(i.r8, i.r9, i.r6, i.r7)'
  System::Free $1
  IntOp $6 $6 - $8
  IntOp $7 $7 - $9
  IntOp $2 $2 + $4
  IntOp $2 $2 - $6
  IntOp $2 $2 / 2
  IntOp $3 $3 + $5
  IntOp $3 $3 - $7
  IntOp $3 $3 / 2
  System::Call 'user32::SetWindowPos(p $HWNDPARENT, p 0, i r2, i r3, i r6, i r7, i 0x14)'
  SetCtlColors $HWNDPARENT "" ${ME_BGHEX}

  ; The page itself fills the window; its list of details and Show details go.
  FindWindow $R0 "#32770" "" $HWNDPARENT
  !insertmacro MEPX $6 ${ME_W}
  !insertmacro MEPX $7 ${ME_H}
  System::Call 'user32::SetWindowPos(p $R0, p 0, i 0, i 0, i r6, i r7, i 0x14)'
  SetCtlColors $R0 "" ${ME_BGHEX}
  !insertmacro MEHIDE $R0 1006
  !insertmacro MEHIDE $R0 1016
  !insertmacro MEHIDE $R0 1027

  ; The app's icon (the installer carries it as its own icon, resource 103).
  !insertmacro MEPX $2 48
  !insertmacro MEPX $3 ${ME_W}
  IntOp $3 $3 - $2
  IntOp $3 $3 / 2
  !insertmacro MEPX $4 36
  System::Call 'user32::CreateWindowEx(i 0, w "STATIC", w "", i 0x50000043, i r3, i r4, i r2, i r2, p $R0, p 0, p 0, p 0) p.r5'
  System::Call 'kernel32::GetModuleHandle(p 0) p.r6'
  System::Call 'user32::LoadImage(p r6, p 103, i 1, i r2, i r2, i 0) p.r7'
  SendMessage $5 0x170 $7 0
  SetCtlColors $5 "" ${ME_BGHEX}

  ; The name, in the launcher's title face.
  !insertmacro MEPX $2 98
  !insertmacro MEPX $3 ${ME_W}
  !insertmacro MEPX $4 30
  System::Call 'user32::CreateWindowEx(i 0, w "STATIC", w "${PRODUCTNAME}", i 0x50000001, i 0, i r2, i r3, i r4, p $R0, p 0, p 0, p 0) p.r5'
  CreateFont $6 "Bahnschrift SemiBold" 15 400
  SendMessage $5 ${WM_SETFONT} $6 1
  SetCtlColors $5 ${ME_INK} ${ME_BGHEX}

  ; What is happening, under it.
  !insertmacro MEPX $2 134
  !insertmacro MEPX $4 22
  System::Call 'user32::CreateWindowEx(i 0, w "STATIC", w "Installing", i 0x50000001, i 0, i r2, i r3, i r4, p $R0, p 0, p 0, p 0) p.r5'
  StrCpy $MEStatus $5
  CreateFont $6 "Segoe UI" 10 400
  SendMessage $5 ${WM_SETFONT} $6 1
  SetCtlColors $5 ${ME_MUTED} ${ME_BGHEX}

  ; The installer's own progress bar, flat and in the launcher's blue.
  GetDlgItem $5 $R0 1004
  !insertmacro MEPX $2 60
  !insertmacro MEPX $3 178
  !insertmacro MEPX $4 320
  !insertmacro MEPX $6 6
  System::Call 'uxtheme::SetWindowTheme(p r5, w "", w "")'
  System::Call 'user32::GetWindowLong(p r5, i -20) i.r7'
  IntOp $7 $7 & 0xFFFDFDFF
  System::Call 'user32::SetWindowLong(p r5, i -20, i r7)'
  System::Call 'user32::GetWindowLong(p r5, i -16) i.r7'
  IntOp $7 $7 & 0xFF7FFFFF
  IntOp $7 $7 | 0x1
  System::Call 'user32::SetWindowLong(p r5, i -16, i r7)'
  System::Call 'user32::SetWindowPos(p r5, p 0, i r2, i r3, i r4, i r6, i 0x34)'
  SendMessage $5 0x409 0 ${ME_BAR}
  SendMessage $5 0x2001 0 ${ME_TRACK}
FunctionEnd
