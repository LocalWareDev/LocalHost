# STAGE 2 -- still READ-ONLY on the VHD. Copies the UEFI BCD store OUT so it can be
# inspected (and later edited) locally.
#
# Why: bcdedit opens a store read-write even for /enum, so it cannot read a store on
# a read-only mount at all ("The media is write protected"). Copying the file out
# lets us inspect and edit a local copy, keeping the write-mount of the user's only
# installer down to one short copy-back later.
#
# We target EFI\Microsoft\Boot\BCD because the guest boots via OVMF (UEFI). The
# legacy boot\bcd is left alone.

$ErrorActionPreference = "Continue"
$sp  = "C:\Users\DELL\AppData\Local\Temp\claude\c--Users-DELL-OneDrive-Desktop-LocalHost-py\485e8e1b-539c-4b38-bfba-25580517575b\scratchpad"
$log = "$sp\vhd_stage2_out.txt"
$lines = New-Object System.Collections.ArrayList
function W($m) { [void]$lines.Add([string]$m); Write-Host $m }

$vhd = "C:\VHDs\win10_installer.vhd"
W "=== STAGE 2 (read-only copy-out) $(Get-Date -Format 'HH:mm:ss') ==="

try {
    $di = Mount-DiskImage -ImagePath $vhd -Access ReadOnly -StorageType VHD -PassThru -ErrorAction Stop
    Start-Sleep -Seconds 3
    $disk = $di | Get-DiskImage | Get-Disk
    $part = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Size -gt 1GB } | Select-Object -First 1
    $vol  = Get-Volume -Partition $part
    $root = $vol.UniqueId; if (-not $root.EndsWith("\")) { $root += "\" }
    W "volume: $($vol.FileSystemType) '$($vol.FileSystemLabel)'"

    $src = Join-Path $root "EFI\Microsoft\Boot\BCD"
    $dst = "$sp\BCD_uefi_original"
    Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop
    W "copied UEFI BCD out: $((Get-Item $dst).Length) bytes -> $dst"

    # Keep a pristine second copy so we can always diff/restore even if the working
    # copy gets edited.
    Copy-Item -LiteralPath $dst -Destination "$sp\BCD_uefi_pristine" -Force
    W "pristine reference saved alongside it"
} catch {
    W "FAILED: $($_.Exception.Message)"
} finally {
    Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    W "dismounted -- VHD unmodified"
}

# Now inspect the LOCAL copy, which bcdedit can open read-write freely.
$dst = "$sp\BCD_uefi_original"
if (Test-Path $dst) {
    W "--- bcdedit /enum all (local copy) ---"
    & bcdedit /store $dst /enum all 2>&1 | ForEach-Object { W "   $_" }
    W "--- bcdedit /dbgsettings (local copy) ---"
    & bcdedit /store $dst /dbgsettings 2>&1 | ForEach-Object { W "   $_" }
}

[System.IO.File]::WriteAllLines($log, $lines, [System.Text.Encoding]::ASCII)
