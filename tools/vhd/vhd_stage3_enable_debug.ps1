# STAGE 3 -- enable kernel debugging, then write the store back into the VHD.
#
# Only ONE setting changes: {default} debug On. {dbgsettings} already reads
# debugtype=Serial, debugport=2, baudrate=115200, which is exactly COM2 (0x2F8) --
# the port our KD named pipe \\.\pipe\LocalHostKD is on and that the DBG2 table
# already advertises. Nothing else needs touching.
#
# Note `debug on` enables kernel debugging; it does NOT make the guest wait for a
# debugger (that would be `bootdebug`), so the VM still boots normally when nothing
# is attached.
#
# Safety: an 8GB backup exists at C:\VHDs\win10_installer.backup.vhd (stage 1), the
# store is edited as a LOCAL COPY first, and after writing back we copy it out again
# and compare bytes to prove the write landed intact. The read-write mount is held
# for one file copy only.

$ErrorActionPreference = "Continue"
$sp  = "C:\Users\DELL\AppData\Local\Temp\claude\c--Users-DELL-OneDrive-Desktop-LocalHost-py\485e8e1b-539c-4b38-bfba-25580517575b\scratchpad"
$log = "$sp\vhd_stage3_out.txt"
$lines = New-Object System.Collections.ArrayList
function W($m) { [void]$lines.Add([string]$m); Write-Host $m }
function Flush { [System.IO.File]::WriteAllLines($log, $lines, [System.Text.Encoding]::ASCII) }

$vhd  = "C:\VHDs\win10_installer.vhd"
$bak  = "C:\VHDs\win10_installer.backup.vhd"
$work = "$sp\BCD_uefi_modified"
$orig = "$sp\BCD_uefi_pristine"

W "=== STAGE 3 $(Get-Date -Format 'HH:mm:ss') ==="

if (-not (Test-Path $bak)) { W "ABORT: backup missing at $bak"; Flush; exit 1 }
W "backup present: $([math]::Round((Get-Item $bak).Length/1GB,1)) GB"
if (-not (Test-Path $orig)) { W "ABORT: pristine BCD copy missing"; Flush; exit 1 }

# --- edit a local copy ---
Copy-Item $orig $work -Force
& bcdedit /store $work /set "{default}" debug on 2>&1 | ForEach-Object { W "  set: $_" }
W "--- verify local copy ---"
$verify = & bcdedit /store $work /enum "{default}" 2>&1
$verify | ForEach-Object { W "   $_" }
if (-not ($verify -match 'debug\s+Yes')) {
    W "ABORT: local copy does not show 'debug Yes' -- not writing anything back"
    Flush; exit 1
}
W "local copy verified: debug Yes"

# --- write back, read-write mount held for just this copy ---
try {
    $di = Mount-DiskImage -ImagePath $vhd -StorageType VHD -PassThru -ErrorAction Stop
    Start-Sleep -Seconds 3
    $disk = $di | Get-DiskImage | Get-Disk
    $part = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Size -gt 1GB } | Select-Object -First 1
    $vol  = Get-Volume -Partition $part
    $root = $vol.UniqueId; if (-not $root.EndsWith("\")) { $root += "\" }
    W "mounted READ-WRITE: $($vol.FileSystemType) '$($vol.FileSystemLabel)'"

    $target = Join-Path $root "EFI\Microsoft\Boot\BCD"
    Copy-Item -LiteralPath $work -Destination $target -Force -ErrorAction Stop
    W "wrote BCD back ($((Get-Item -LiteralPath $target).Length) bytes)"

    # Read it back out and compare bytes to prove the write landed.
    $check = "$sp\BCD_uefi_readback"
    Copy-Item -LiteralPath $target -Destination $check -Force
    $a = [System.IO.File]::ReadAllBytes($work)
    $b = [System.IO.File]::ReadAllBytes($check)
    $same = ($a.Length -eq $b.Length)
    if ($same) { for ($i=0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { $same = $false; break } } }
    W "readback byte-identical: $same"
} catch {
    W "WRITE FAILED: $($_.Exception.Message)"
} finally {
    Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    W "dismounted"
}
Flush
