param([string[]]$Rvas = @('0x223EFD'))
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class S {
  [DllImport("dbghelp.dll", SetLastError=true)] public static extern bool SymInitialize(IntPtr p, string path, bool invade);
  [DllImport("dbghelp.dll")] public static extern uint SymSetOptions(uint o);
  [DllImport("dbghelp.dll", CharSet=CharSet.Unicode)] public static extern ulong SymLoadModuleExW(IntPtr p, IntPtr f, string img, string mod, ulong baseAddr, uint size, IntPtr d, uint flags);
  [DllImport("dbghelp.dll", CharSet=CharSet.Unicode)] public static extern bool SymFromAddrW(IntPtr p, ulong addr, out ulong disp, IntPtr info);
  [DllImport("dbghelp.dll", CharSet=CharSet.Unicode)] public static extern bool SymGetLineFromAddrW64(IntPtr p, ulong addr, out uint disp, IntPtr line);
}
"@
$exe = 'C:\Users\Xx_Bootyslayer_xX\Documents\Claude\XeniaGuide&Hud\xenia-canary\build\bin\Windows\Release\xenia_canary.exe'
$dir = Split-Path $exe
$h = [IntPtr]::new(-1)
[void][S]::SymSetOptions(0x00000002 -bor 0x00000010)  # UNDNAME|LOAD_LINES
if (-not [S]::SymInitialize($h, $dir, $false)) { "SymInitialize failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"; exit 1 }
$base = [uint64]0x10000000
$sz = [uint32](Get-Item $exe).Length
$got = [S]::SymLoadModuleExW($h, [IntPtr]::Zero, $exe, $null, $base, $sz, [IntPtr]::Zero, 0)
if ($got -eq 0) { "SymLoadModuleEx failed (err $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"; exit 1 }
$modbase = $got
"module loaded at 0x{0:X}" -f $got
foreach ($r in $Rvas) {
  $rva = [Convert]::ToUInt64($r.Replace('0x',''), 16)
  $addr = $modbase + $rva
  $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(88 + 2048)
  [Runtime.InteropServices.Marshal]::WriteInt32($buf, 0, 88)      # SizeOfStruct
  [Runtime.InteropServices.Marshal]::WriteInt32($buf, 80, 1024)   # MaxNameLen
  $disp = [uint64]0
  if ([S]::SymFromAddrW($h, $addr, [ref]$disp, $buf)) {
    $nameLen = [Runtime.InteropServices.Marshal]::ReadInt32($buf, 76)
    $name = [Runtime.InteropServices.Marshal]::PtrToStringUni([IntPtr]::Add($buf, 84), $nameLen)
    "{0}  ->  {1}+0x{2:X}" -f $r, $name, $disp
  } else {
    "{0}  ->  (no symbol, err {1})" -f $r, [Runtime.InteropServices.Marshal]::GetLastWin32Error()
  }
  $lbuf = [Runtime.InteropServices.Marshal]::AllocHGlobal(40)
  [Runtime.InteropServices.Marshal]::WriteInt32($lbuf, 0, 40)
  $ld = 0
  if ([S]::SymGetLineFromAddrW64($h, $addr, [ref]$ld, $lbuf)) {
    $fn = [Runtime.InteropServices.Marshal]::PtrToStringUni([Runtime.InteropServices.Marshal]::ReadIntPtr($lbuf, 16))
    $ln = [Runtime.InteropServices.Marshal]::ReadInt32($lbuf, 12)
    "        at {0}:{1}" -f $fn, $ln
  }
  [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
  [Runtime.InteropServices.Marshal]::FreeHGlobal($lbuf)
}
