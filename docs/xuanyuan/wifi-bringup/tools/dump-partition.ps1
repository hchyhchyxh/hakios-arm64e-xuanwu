# Dump a GPT partition through the loader ("oem read" + "get_staged") in 2 MB chunks.
param(
  [string]$Fastboot = "X:\air6\ui-functional-test-20261107\adb35\platform-tools\fastboot.exe",
  [string]$Partition,
  [string]$OutFile,
  [long]$Bytes = 4194304
)
$chunk = 2097152
$dir = Split-Path -Parent $OutFile
$tmp = Join-Path $dir ("_chunk_" + $Partition + ".bin")
if (Test-Path $OutFile) { Remove-Item $OutFile -Force }
$fs = [IO.File]::Create($OutFile)
$n = 0
try {
  for ($off = 0; $off -lt $Bytes; $off += $chunk) {
    $len = [Math]::Min($chunk, $Bytes - $off)
    & $Fastboot oem read $Partition ("{0:x}" -f $off) ("{0:x}" -f $len) 2>&1 | Out-Null
    & $Fastboot get_staged $tmp 2>&1 | Out-Null
    if (-not (Test-Path $tmp)) { Write-Output ("chunk missing at " + $off); break }
    $b = [IO.File]::ReadAllBytes($tmp)
    $fs.Write($b, 0, $b.Length)
    Remove-Item $tmp -Force
    $n++
    if (($n % 20) -eq 0) { Write-Output ("  " + ($off + $len) + " / " + $Bytes) }
  }
} finally { $fs.Close() }
Write-Output ("wrote " + (Get-Item $OutFile).Length + " bytes to " + $OutFile)
