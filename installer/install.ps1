# MKX Access installer: finds Mortal Kombat X (Steam) and copies the mod into it.
# Run with -Uninstall to remove it again.
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$modFiles = @('MKXAccess.asi', 'nvdaControllerClient64.dll', 'LICENSE-NVDA.txt')
$loader = 'dinput8.dll'
$ourLoaderSize = 4608

function Show-Message($text, $title, $isError) {
    Write-Host ''
    Write-Host $text
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $icon = if ($isError) { [System.Windows.Forms.MessageBoxIcon]::Error } else { [System.Windows.Forms.MessageBoxIcon]::Information }
        [void][System.Windows.Forms.MessageBox]::Show($text, $title, [System.Windows.Forms.MessageBoxButtons]::OK, $icon)
    } catch { Read-Host 'Press Enter to close' | Out-Null }
}

function Find-GameFolder {
    $libraries = New-Object System.Collections.Generic.List[string]
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        try {
            $props = Get-ItemProperty -Path $key -ErrorAction Stop
            foreach ($name in @('SteamPath', 'InstallPath')) {
                $p = $props.$name
                if ($p) { $libraries.Add(($p -replace '/', '\')) }
            }
        } catch { }
    }
    # every Steam library folder listed in libraryfolders.vdf
    foreach ($steam in @($libraries)) {
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            $content = Get-Content -LiteralPath $vdf -Raw
            foreach ($m in [regex]::Matches($content, '"path"\s+"([^"]+)"')) {
                $libraries.Add(($m.Groups[1].Value -replace '\\\\', '\'))
            }
        }
    }
    # common places, in case the registry is missing
    foreach ($drive in [System.IO.DriveInfo]::GetDrives()) {
        if ($drive.DriveType -eq 'Fixed' -and $drive.IsReady) {
            $root = $drive.RootDirectory.FullName
            $libraries.Add((Join-Path $root 'SteamLibrary'))
            $libraries.Add((Join-Path $root 'Steam'))
            $libraries.Add((Join-Path $root 'Program Files (x86)\Steam'))
            $libraries.Add((Join-Path $root 'Program Files\Steam'))
            $libraries.Add((Join-Path $root 'Games\Steam'))
            $libraries.Add((Join-Path $root 'Games\SteamLibrary'))
        }
    }
    foreach ($lib in $libraries) {
        $retail = Join-Path $lib 'steamapps\common\MK10\Binaries\Retail'
        if (Test-Path -LiteralPath (Join-Path $retail 'MK10.exe')) { return $retail }
    }
    return $null
}

Write-Host 'MKX Access installer'
Write-Host 'Looking for Mortal Kombat X...'

if ($Uninstall) {
    if (Get-Process -Name 'MK10', 'MK10Game', 'MKXLauncher' -ErrorAction SilentlyContinue) {
        Show-Message 'Mortal Kombat X is running. Please close the game, then run Uninstall again.' 'MKX Access' $true
        exit 1
    }
    $target = Find-GameFolder
    if (-not $target) { Show-Message 'Could not find Mortal Kombat X, so there is nothing to remove.' 'MKX Access' $true; exit 1 }
    try {
        foreach ($f in $modFiles + @('MKXAccess.log', 'MKXAccess-README.txt')) {
            $p = Join-Path $target $f
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
        }
        $p = Join-Path $target $loader
        # only remove our own small loader, never another mod's
        if ((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p).Length -eq $ourLoaderSize)) { Remove-Item -LiteralPath $p -Force }
    } catch {
        Show-Message ("Could not remove the files: " + $_.Exception.Message) 'MKX Access' $true
        exit 1
    }
    Show-Message 'MKX Access has been removed. The game is back to normal.' 'MKX Access' $false
    exit 0
}

foreach ($f in $modFiles + $loader) {
    if (-not (Test-Path -LiteralPath (Join-Path $here $f))) {
        Show-Message "The file $f is missing. Please unzip the whole MKXAccess zip first, then run Install again from the unzipped folder." 'MKX Access' $true
        exit 1
    }
}

if (Get-Process -Name 'MK10', 'MK10Game', 'MKXLauncher' -ErrorAction SilentlyContinue) {
    Show-Message 'Mortal Kombat X is running. Please close the game, then run Install again.' 'MKX Access' $true
    exit 1
}

$target = Find-GameFolder
if (-not $target) {
    Show-Message ("Could not find Mortal Kombat X. It needs the Steam version installed.`n`n" +
                  "To install by hand: in Steam, right-click Mortal Kombat X, choose Manage, then Browse local files. " +
                  "Open Binaries, then Retail, and copy dinput8.dll, MKXAccess.asi, nvdaControllerClient64.dll and LICENSE-NVDA.txt there.") 'MKX Access' $true
    exit 1
}
Write-Host "Found the game in $target"

try {
    foreach ($f in $modFiles) { Copy-Item -LiteralPath (Join-Path $here $f) -Destination $target -Force }
    $existing = Join-Path $target $loader
    $keptOther = $false
    if ((Test-Path -LiteralPath $existing) -and ((Get-Item -LiteralPath $existing).Length -ne $ourLoaderSize)) {
        # a different dinput8.dll, most likely MKXHook's ASI loader, which also loads MKX Access
        $keptOther = $true
    } else {
        Copy-Item -LiteralPath (Join-Path $here $loader) -Destination $target -Force
    }
} catch {
    Show-Message ("Could not copy the files: " + $_.Exception.Message + "`n`nTry running Install again. If it keeps failing, right-click Install and choose Run as administrator.") 'MKX Access' $true
    exit 1
}

$msg = "MKX Access is installed.`n`nStart NVDA, then start Mortal Kombat X. You will hear MKX Access loaded once the game window appears."
if ($keptOther) { $msg += "`n`nYou already had another dinput8.dll, probably from MKXHook. It was kept, and it loads MKX Access too." }
Show-Message $msg 'MKX Access' $false
exit 0
