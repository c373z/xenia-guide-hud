param([int]$After = 25,
      [string]$Grep = 'Coverage|composite draw|draws/swap|scene=',
      [string]$Extra = '',
      [int]$Boot = 20,
      [string]$Title = '',
      [switch]$NoBuild,
      [switch]$BreakOnDebugBreak)
$dir = 'C:\Users\Xx_Bootyslayer_xX\Documents\Claude\XeniaGuide&Hud\xenia-canary\build\bin\Windows\Release'
$exe = Join-Path $dir 'xenia_canary.exe'
$dash = 'C:\Users\Xx_Bootyslayer_xX\Documents\Claude\XeniaGuide&Hud\dashroot\dash.xex'
$xex = if ($Title) { $Title } else { $dash }

# Xenia reports an unhandled exception with a MODAL dialog. The faulting
# thread sits in that dialog's message loop forever - and if it held the
# global critical region, every later kernel operation wedges behind it. The
# log simply stops, which looks exactly like a hang. Detect the dialog so a
# crashed run is reported as a crash instead of a mystery stall.
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class XW {
  public delegate bool Cb(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(Cb c, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, Cb c, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
}
"@

function Get-CrashDialogs([int]$ProcId) {
  $script:dlg = @()
  $cb = [XW+Cb]{ param($h,$l)
    $op = 0; [void][XW]::GetWindowThreadProcessId($h, [ref]$op)
    if ($op -eq $ProcId) {
      $c = New-Object Text.StringBuilder 256; [void][XW]::GetClassNameW($h,$c,256)
      if ($c.ToString() -eq '#32770') {
        $ccb = [XW+Cb]{ param($ch,$cl)
          $t = New-Object Text.StringBuilder 4096; [void][XW]::GetWindowTextW($ch,$t,4096)
          if ($t.Length -gt 8) { $script:dlg += $t.ToString() }
          return $true
        }
        [void][XW]::EnumChildWindows($h, $ccb, [IntPtr]::Zero)
      }
    }
    return $true
  }
  [void][XW]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:dlg
}

# Build before running. This script used to launch whatever binary happened to
# be sitting in buildin, which is a trap: the patch -> build -> run -> revert
# cycle used for one-off experiments restores the source but leaves the binary
# built from the patched source, so a clean tree gets tested with a dirty exe
# and the result looks like a regression in code nobody touched.
# cl.exe needs the MSVC environment, so go through vcvars64.
if (-not $NoBuild) {
  $vc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
  $cm = 'C:\Program Files\CMake\bin\cmake.exe'
  $bd = Join-Path (Split-Path (Split-Path (Split-Path $PSScriptRoot))) 'build'
  $bat = Join-Path $env:TEMP 'press-build.bat'
  @("@echo off", "call `"$vc`" >nul", "`"$cm`" --build `"$bd`" --config Release") |
    Set-Content -Encoding ASCII $bat
  $out = & cmd /c $bat 2>&1
  if ($LASTEXITCODE -ne 0) {
    Write-Host "ABORT: build failed"
    $out | Select-Object -Last 20 | ForEach-Object { Write-Host $_ }
    exit 1
  }
  $out | Select-String -Pattern '^\[\d+/\d+\]' | Select-Object -Last 3 | ForEach-Object { Write-Host $_.Line }
}

$log = Join-Path $dir 'xenia.log'
Remove-Item $log -ErrorAction SilentlyContinue

$argl = @()
# The guest's own tw/twi assert trap becomes a fatal modal dialog about a
# second into boot unless this is off, and then nothing reaches the Guide at
# all. Passed on the command line rather than saved to the config, because
# editing the config has broken plain launches before.
if (-not $BreakOnDebugBreak -and $Extra -notmatch 'break_on_debugbreak') {
  $argl += '--break_on_debugbreak=false'
}
if ($Extra) { $argl += $Extra.Split(' ') }
$argl += "`"$xex`""
$p = Start-Process $exe -ArgumentList $argl -WorkingDirectory $dir -PassThru

# Poll rather than sleep blind: a run that dies at boot costs seconds, not the
# whole window. A GUEST CRASH is NOT fatal - the faulting guest thread dies but
# the emulator keeps running, and the dashboard takes one at boot every run.
function Wait-Or-Die([int]$Seconds, [string]$Phase) {
  for ($i = 0; $i -lt $Seconds * 4; $i++) {
    Start-Sleep -Milliseconds 250
    if ($p.HasExited) {
      Write-Host "ABORT: process exited during $Phase (code $($p.ExitCode)) after ~$([int]($i/4))s"
      return $false
    }
    # Check once a second, not every tick - EnumWindows is not free.
    if ($i % 4 -eq 0) {
      $d = Get-CrashDialogs $p.Id
      if ($d.Count -gt 0) {
        Write-Host "ABORT: UNHANDLED EXCEPTION dialog during $Phase after ~$([int]($i/4))s"
        $d | ForEach-Object { Write-Host $_ }
        return $false
      }
    }
  }
  return $true
}

$ok = Wait-Or-Die $Boot 'boot'
if ($ok) { $ok = Wait-Or-Die $After 'run' }
try { if (-not $p.HasExited) { $p.Kill() } } catch {}
Start-Sleep -Seconds 2

$t = Get-Content $log -ErrorAction SilentlyContinue
"lines=$($t.Count)"
$crash = $t | Select-String -Pattern 'GUEST CRASH'
if ($crash) { "--- GUEST CRASH (first 6) ---"; $crash | Select-Object -First 6 | ForEach-Object { $_.Line } }
"--- matches ---"
$t | Select-String -Pattern $Grep | Select-Object -Last 25 | ForEach-Object { $_.Line }
if (-not $ok) { "--- log tail ---"; $t | Select-Object -Last 10 }
