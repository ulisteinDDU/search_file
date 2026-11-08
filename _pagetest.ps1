# Paging GUI test: drives FileSearchTool.exe, verifies page-size switch + page nav.
# ASCII-only source (PS 5.1 reads BOM-less UTF-8 as ANSI).
$ErrorActionPreference = 'Stop'
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class W {
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, [MarshalAs(UnmanagedType.LPWStr)] string c, [MarshalAs(UnmanagedType.LPWStr)] string t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageLPtr(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageStr(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SetWindowTextW(IntPtr h, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern void PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@

function Get-Text($h) { $sb = New-Object System.Text.StringBuilder 256; [W]::GetWindowTextW($h, $sb, 256) | Out-Null; $sb.ToString() }
function Get-Class($h) { $sb = New-Object System.Text.StringBuilder 64; [W]::GetClassNameW($h, $sb, 64) | Out-Null; $sb.ToString() }
function Get-Nums($h)  { [regex]::Matches((Get-Text $h), '\d+') | ForEach-Object { [int]$_.Value } }

# find direct child of $parent with control id=$id and window class=$cls
function Resolve-Child($parent, $id, $cls) {
  $cur = [W]::GetWindow($parent, 5)          # GW_CHILD
  while ($cur -ne [IntPtr]::Zero) {
    if ([W]::GetDlgCtrlID($cur) -eq $id -and (Get-Class $cur) -eq $cls) { return $cur }
    $cur = [W]::GetWindow($cur, 2)           # GW_HWNDNEXT
  }
  return [IntPtr]::Zero
}

# auto-close any modal popup (#32770 = MessageBox/dialog) of the app
$script:targetPid = 0
function Close-Popups {
  $proc = [W+EnumWindowsProc]{ param($h, $l)
    $wp = 0; [W]::GetWindowThreadProcessId($h, [ref]$wp) | Out-Null
    if ($wp -eq $script:targetPid) {
      $c2 = New-Object System.Text.StringBuilder 64
      [W]::GetClassNameW($h, $c2, 64) | Out-Null
      if ($c2.ToString() -eq "#32770") { [W]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
    }
    return $true
  }
  [W]::EnumWindows($proc, [IntPtr]::Zero) | Out-Null
}

# 1) create 100 test files
$dir = "D:\data\qx\z.ai\search_tool\_pagetest"
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Path $dir | Out-Null
for ($i = 0; $i -lt 100; $i++) { Set-Content -Path (Join-Path $dir ("t{0:d3}.txt" -f $i)) -Value "x" }

# 2) start app
$p = Start-Process -FilePath "D:\data\qx\z.ai\search_tool\FileSearchTool.exe" -PassThru
try {
  $hMain = [IntPtr]::Zero
  $script:targetPid = $p.Id
  $deadline = (Get-Date).AddSeconds(10)
  while ((Get-Date) -lt $deadline -and $hMain -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 200
    $find = [W+EnumWindowsProc]{ param($h, $l)
      $wpid = 0; [W]::GetWindowThreadProcessId($h, [ref]$wpid) | Out-Null
      if ($wpid -eq $script:targetPid) {
        $cb = New-Object System.Text.StringBuilder 256
        [W]::GetClassNameW($h, $cb, 256) | Out-Null
        if ($cb.ToString() -eq "QxFileSearchMainWnd") { $script:hMain = $h; return $false }
      }
      return $true
    }
    [W]::EnumWindows($find, [IntPtr]::Zero) | Out-Null
    $hMain = $script:hMain
  }
  if ($hMain -eq [IntPtr]::Zero) { throw "main window not found" }

  $panel = [IntPtr]::Zero
  $cur = [IntPtr]::Zero
  while ($true) {
    $cur = [W]::FindWindowExW($hMain, $cur, "QxPageWnd", $null)
    if ($cur -eq [IntPtr]::Zero) { break }
    if ([W]::IsWindowVisible($cur)) { $panel = $cur }   # pick the VISIBLE page panel
  }
  if ($panel -eq [IntPtr]::Zero) { throw "visible panel not found" }

  $hDir    = Resolve-Child $panel 1001 "Edit"
  $hNeedle = Resolve-Child $panel 1003 "Edit"
  $hMatch  = Resolve-Child $panel 1005 "ComboBox"
  $hSearch = Resolve-Child $panel 1009 "Button"
  $hPagesz = Resolve-Child $panel 1015 "ComboBox"
  $hNext   = Resolve-Child $panel 1018 "Button"
  $hLast   = Resolve-Child $panel 1019 "Button"
  $hList   = Resolve-Child $panel 1021 "SysListView32"
  $hTotal  = Resolve-Child $panel 1022 "Static"
  $hPage   = Resolve-Child $panel 1023 "Static"

  function Sel-Combo($h, $idx) {
    [W]::SendMessageW($h, 0x014E, [IntPtr]$idx, [IntPtr]::Zero) | Out-Null   # CB_SETCURSEL
    $cid = [W]::GetDlgCtrlID($h)
    [W]::PostMessageW($panel, 0x0111, [IntPtr]($cid -bor (1 -shl 16)), $h)  # MAKEWPARAM(id, CBN_SELCHANGE) # WM_COMMAND CBN_SELCHANGE
  }

  # 3) fill form via WM_SETTEXT messages (SetWindowText API can't cross processes!)
  [W]::SendMessageStr($hDir, 0x000C, [IntPtr]::Zero, $dir) | Out-Null
  [W]::SendMessageStr($hNeedle, 0x000C, [IntPtr]::Zero, "t") | Out-Null
  Sel-Combo $hMatch 1
  [W]::PostMessageW($hSearch, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)   # BM_CLICK (async)

  $deadline = (Get-Date).AddSeconds(15); $tot = 0
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 300
    Close-Popups
    $n = @(Get-Nums $hTotal); if ($n.Count -gt 0 -and $n[0] -ge 100) { $tot = $n[0]; break }
  }
  $fail = 0
  if ($tot -ne 100) { Write-Output "FAIL: total=$tot (expect 100)"; $fail = 1 }

  # 4) default page size 30 -> expect page 1/4
  $pg = @(Get-Nums $hPage)
  if ($pg.Count -lt 2 -or $pg[0] -ne 1 -or $pg[1] -ne 4) { Write-Output "FAIL: pageinfo=$((Get-Text $hPage)) expect 1/4"; $fail = 1 }
  else { Write-Output "OK: initial 30/page -> $((Get-Text $hPage))" }

  # 5) switch page size to 100 (index 2) -> expect 1/1
  Sel-Combo $hPagesz 2
  Start-Sleep -Milliseconds 400
  Close-Popups
  $pg = @(Get-Nums $hPage)
  $rows = [W]::SendMessageLPtr($hList, 0x1000 + 4, [IntPtr]::Zero, [IntPtr]::Zero)  # LVM_GETITEMCOUNT
  if ($pg.Count -lt 2 -or $pg[0] -ne 1 -or $pg[1] -ne 1 -or [int]$rows -ne 100) {
    Write-Output "FAIL: after 100/page pageinfo=$((Get-Text $hPage)) rows=$rows"; $fail = 1
  } else { Write-Output "OK: switch to 100/page -> 1/1, rows=$rows" }

  # 6) back to 30 (index 0), click next -> 2/4, click last -> 4/4
  Sel-Combo $hPagesz 0
  Start-Sleep -Milliseconds 400
  [W]::PostMessageW($hNext, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 300
  $pg = @(Get-Nums $hPage)
  if ($pg.Count -lt 2 -or $pg[0] -ne 2 -or $pg[1] -ne 4) { Write-Output "FAIL: next -> $((Get-Text $hPage))"; $fail = 1 }
  else { Write-Output "OK: next -> 2/4" }
  [W]::PostMessageW($hLast, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 300
  $pg = @(Get-Nums $hPage)
  if ($pg.Count -lt 2 -or $pg[0] -ne 4 -or $pg[1] -ne 4) { Write-Output "FAIL: last -> $((Get-Text $hPage))"; $fail = 1 }
  else { Write-Output "OK: last -> 4/4" }

  # 7) manual typed page size "50" + CBN_KILLFOCUS -> expect 1/2
  #    type via WM_CHAR into the combo's inner Edit (cross-proc WM_SETTEXT only updates the cache)
  $psEdit = [W]::FindWindowExW($hPagesz, [IntPtr]::Zero, "Edit", $null)
  [W]::SendMessageLPtr($psEdit, 0x00B1, [IntPtr]0, [IntPtr]-1) | Out-Null   # EM_SETSEL select all
  foreach ($ch in "50".ToCharArray()) {
    [W]::SendMessageLPtr($psEdit, 0x0102, [IntPtr][int][char]$ch, [IntPtr]::Zero) | Out-Null  # WM_CHAR
  }
  $cid2 = [W]::GetDlgCtrlID($hPagesz)
  [W]::PostMessageW($script:panel, 0x0111, [IntPtr]($cid2 -bor (4 -shl 16)), $hPagesz)  # CBN_KILLFOCUS = 4
  Start-Sleep -Milliseconds 500
  $pg = @(Get-Nums $hPage)
  $rows = [W]::SendMessageLPtr($hList, 0x1004, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($pg.Count -lt 2 -or $pg[0] -ne 1 -or $pg[1] -ne 2 -or [int]$rows -ne 50) {
    Write-Output "FAIL: typed 50/page -> $((Get-Text $hPage)) rows=$rows"; $fail = 1
  } else { Write-Output "OK: typed 50/page (killfocus) -> 1/2 rows=$rows" }

  Write-Output ("RESULT: " + $(if ($fail -eq 0) { "PASS" } else { "FAIL" }))
}
finally {
  if (!$p.HasExited) { Stop-Process -Id $p.Id -Force }
  Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
}
