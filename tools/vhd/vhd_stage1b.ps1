# STAGE 1b -- still READ-ONLY. Stage 1 mounted fine but neither partition received a
# drive letter (a read-only mount does not auto-assign), so every Test-Path missed.
# Reach the volume by its GUID path instead, which needs no letter and no write.
#
# Layout found in stage 1:
#   part 1   16 MB    {e3c9e316-0b5c-4db8-817d-f92df00215ae}  Microsoft Reserved
#   part 2   8000 MB  {c12a7328-f81f-11d2-ba4b-00a0c93ec93b}  EFI System Partition
# So the whole 8GB is a FAT32 ESP carrying the installer.

$ErrorActionPreference = "Continue"
$log = "C:\Users\DELL\AppData\Local\Temp\claude\c--Users-DELL-OneDrive-Desktop-LocalHost-py\485e8e1b-539c-4b38-bfba-25580517575b\scratchpad\vhd_stage1b_out.txt"
$lines = New-Object System.Collections.ArrayList
function W($m) { [void]$lines.Add([string]$m); Write-Host $m }

$vhd = "C:\VHDs\win10_installer.vhd"
W "=== STAGE 1b (read-only) $(Get-Date -Format 'HH:mm:ss') ==="

try {
    $di = Mount-DiskImage -ImagePath $vhd -Access ReadOnly -StorageType VHD -PassThru -ErrorAction Stop
    Start-Sleep -Seconds 3
    $disk = $di | Get-DiskImage | Get-Disk
    W "mounted read-only as disk $($disk.Number)"

    foreach ($part in (Get-Partition -DiskNumber $disk.Number)) {
        W "--- partition $($part.PartitionNumber) ($([math]::Round($part.Size/1MB)) MB) ---"
        $vol = $null
        try { $vol = Get-Volume -Partition $part -ErrorAction Stop } catch { }
        if (-not $vol) { W "  no volume object (unformatted or unreadable)"; continue }
        W "  fs=$($vol.FileSystemType) label='$($vol.FileSystemLabel)' uniqueId=$($vol.UniqueId)"

        # UniqueId is the \\?\Volume{guid}\ path; usable directly with no drive letter.
        $root = $vol.UniqueId
        if (-not $root.EndsWith("\")) { $root += "\" }
        try {
            $top = Get-ChildItem -LiteralPath $root -Force -ErrorAction Stop
            W "  top level: $(($top | Select-Object -ExpandProperty Name) -join ', ')"
        } catch { W "  cannot list root: $($_.Exception.Message)"; continue }

        foreach ($rel in @("EFI\Microsoft\Boot\BCD", "boot\bcd", "EFI\Boot\BCD")) {
            $p = Join-Path $root $rel
            if (Test-Path -LiteralPath $p) {
                W "  *** BCD FOUND: $rel ($((Get-Item -LiteralPath $p).Length) bytes)"
                W "  --- bcdedit /enum all ---"
                & bcdedit /store $p /enum all 2>&1 | ForEach-Object { W "     $_" }
                W "  --- bcdedit /dbgsettings ---"
                & bcdedit /store $p /dbgsettings 2>&1 | ForEach-Object { W "     $_" }
            } else {
                W "  (no $rel)"
            }
        }
    }
} catch {
    W "FAILED: $($_.Exception.Message)"
} finally {
    Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    W "dismounted -- nothing written"
}

# Plain ASCII so the parent process reads it cleanly (stage 1 came back UTF-16 mangled).
[System.IO.File]::WriteAllLines($log, $lines, [System.Text.Encoding]::ASCII)
