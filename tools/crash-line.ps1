# Turns a line from an OBS crash log into a source line.
#
# OBS's crash handler prints frames as function+offset, never with line numbers,
# even when the PDB is installed beside the plugin (cmake/windows/helpers.cmake
# installs it). This reads that PDB through Windows' own dbghelp.dll, so it needs
# no Visual Studio. The DLL and PDB must be the pair that crashed.
#
# For "plasmastream-multistream.dll!plasmastream::(anonymous namespace)::worker_main+0xb53":
#
#   powershell -File tools\crash-line.ps1 `
#     -Dll "$env:ProgramData\obs-studio\plugins\plasmastream-multistream\bin\64bit\plasmastream-multistream.dll" `
#     -Func worker_main -Offsets 0xb53
#
# A frame's offset is the return address, so the line printed "minus 1" is the
# call that was running.

param([string]$Dll, [string]$Func, [int[]]$Offsets)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class Dbg {
  [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern bool SymInitializeW(IntPtr h, string path, bool invade);
  [DllImport("dbghelp.dll", SetLastError=true)]
  public static extern uint SymSetOptions(uint o);
  [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern ulong SymLoadModuleExW(IntPtr h, IntPtr f, string img, string mod, ulong bas, uint size, IntPtr data, uint flags);
  public delegate bool EnumCb(IntPtr info, uint size, IntPtr ctx);
  [DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern bool SymEnumSymbolsW(IntPtr h, ulong bas, string mask, EnumCb cb, IntPtr ctx);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  public struct LINE64 { public uint SizeOfStruct; public IntPtr Key; public uint LineNumber; public IntPtr FileName; public ulong Address; }
  [DllImport("dbghelp.dll", SetLastError=true)]
  public static extern bool SymGetLineFromAddrW64(IntPtr h, ulong addr, out uint disp, ref LINE64 line);
}
"@

$h = [IntPtr]42
[Dbg]::SymSetOptions(0x2 -bor 0x10 -bor 0x200) | Out-Null   # undname, load lines, fail-critical-errors
[Dbg]::SymInitializeW($h, (Split-Path $Dll), $false) | Out-Null
$base = [Dbg]::SymLoadModuleExW($h, [IntPtr]::Zero, $Dll, $null, 0x10000000, 0, [IntPtr]::Zero, 0)
if ($base -eq 0) { "load failed"; exit 1 }

$found = New-Object System.Collections.ArrayList
$cb = [Dbg+EnumCb]{
  param($info, $size, $ctx)
  # SYMBOL_INFOW: Address at offset 56, NameLen at 76, Name at 84 (x64)
  $addr = [System.Runtime.InteropServices.Marshal]::ReadInt64($info, 56)
  $len = [System.Runtime.InteropServices.Marshal]::ReadInt32($info, 76)
  $name = [System.Runtime.InteropServices.Marshal]::PtrToStringUni([IntPtr]($info.ToInt64() + 84), $len)
  [void]$found.Add([pscustomobject]@{ Addr = [uint64]$addr; Name = $name })
  return $true
}
[Dbg]::SymEnumSymbolsW($h, $base, "*!*$Func*", $cb, [IntPtr]::Zero) | Out-Null
foreach ($f in $found) { "symbol {0} at +0x{1:x}" -f $f.Name, ($f.Addr - $base) }

$target = $found | Where-Object { $_.Name.Trim([char]0, ' ') -match "(^|::)$Func$" } | Select-Object -First 1
if (-not $target) { "function not found"; exit 1 }
foreach ($o in $Offsets) {
  $line = New-Object Dbg+LINE64
  $line.SizeOfStruct = [System.Runtime.InteropServices.Marshal]::SizeOf([type][Dbg+LINE64])
  $disp = 0
  # The return address points after the call, so look one byte back for the call's own line.
  foreach ($back in 0, 1) {
    $ok = [Dbg]::SymGetLineFromAddrW64($h, $target.Addr + $o - $back, [ref]$disp, [ref]$line)
    if ($ok) {
      $file = [System.Runtime.InteropServices.Marshal]::PtrToStringUni($line.FileName)
      "+0x{0:x} (minus {1}) -> {2}:{3}" -f $o, $back, (Split-Path $file -Leaf), $line.LineNumber
    } else { "+0x{0:x} (minus {1}) -> no line (error {2})" -f $o, $back, [System.Runtime.InteropServices.Marshal]::GetLastWin32Error() }
  }
}
