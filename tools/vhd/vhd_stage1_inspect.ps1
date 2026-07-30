# STAGE 1 -- backup + READ-ONLY inspection. Writes nothing to the VHD.
#
# Deliberately split from the edit: this proves the backup exists and that the BCD
# is where we expect, before anything mounts read-write. win10_installer.vhd is the
# only copy of the user's installer and cannot be backed up to the SD card (FAT32
# caps files at 4GB), so the backup is made on C: first.

$ErrorActionPreference = "Continue"
$log = "C:\Users\DELL\AppData\Local\Temp\claude\c--Users-DELL-OneDrive-Desktop-LocalHost-py\485e8e1b-539c-4b38-bfba-25580517575b\scratchpad\vhd_stage1_out.txt"
function W($m) { $m | Tee-Object -FilePath $log -Append }

"=== STAGE 1 started $(Get-Date -Format 'HH:mm:ss') ===" | Set-Content $log
W "elevated: $((New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"

$vhd = "C:\VHDs\win10_installer.vhd"
$bak = "C:\VHDs\win10_installer.backup.vhd"

# --- backup ---
if (Test-Path $bak) {
    W "backup already exists: $([math]::Round((Get-Item $bak).Length/1GB,1)) GB -- reusing, not overwriting"
} else {
    W "copying 8GB backup (this takes a minute)..."
    try {
        Copy-Item $vhd $bak -ErrorAction Stop
        W "backup OK: $([math]::Round((Get-Item $bak).Length/1GB,1)) GB"
    } catch {
        W "BACKUP FAILED: $($_.Exception.Message)"
        W "=== ABORTING: refusing to touch the VHD without a backup ==="
        exit 1
    }
}

# --- read-only mount ---
try {
    $di = Mount-DiskImage -ImagePath $vhd -Access ReadOnly -StorageType VHD -PassThru -ErrorAction Stop
    Start-Sleep -Seconds 3
    $disk = $di | Get-DiskImage | Get-Disk
    W "mounted READ-ONLY as disk $($disk.Number), style=$($disk.PartitionStyle)"

    W "--- partitions ---"
    Get-Partition -DiskNumber $disk.Number | ForEach-Object {
        W ("  part {0}  letter={1}  {2,8:N0} MB  gpt={3}" -f $_.PartitionNumber, $_.DriveLetter, ($_.Size/1MB), $_.GptType)
    }

    # --- locate the BCD on whichever volume has it ---
    W "--- searching for BCD ---"
    $found = $false
    Get-Partition -DiskNumber $disk.Number | Where-Object { $_.DriveLetter } | ForEach-Object {
        $dl = $_.DriveLetter
        foreach ($p in @("${dl}:\EFI\Microsoft\Boot\BCD", "${dl}:\boot\bcd")) {
            if (Test-Path $p) {
                W "  FOUND: $p  ($((Get-Item $p).Length) bytes)"
                $found = $true
                W "--- current boot entries / debug state ---"
                & bcdedit /store $p /enum all 2>&1 | ForEach-Object { W "    $_" }
                W "--- dbgsettings ---"
                & bcdedit /store $p /dbgsettings 2>&1 | ForEach-Object { W "    $_" }
            }
        }
        # also show what IS on the volume, to orient if the BCD is elsewhere
        W "  top-level of ${dl}: $((Get-ChildItem "${dl}:\" -Force -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name) -join ', ')"
    }
    if (-not $found) { W "  BCD NOT FOUND at the usual paths" }
} catch {
    W "MOUNT FAILED: $($_.Exception.Message)"
} finally {
    Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    W "dismounted"
}
W "=== STAGE 1 done -- nothing was written to the VHD ==="
