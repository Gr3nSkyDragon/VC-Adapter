# Builds the firmware and writes ONE flashable file per line and chip (bootloader + partition table + app merged at offset 0x0) for
# a GitHub release, so users can flash without installing ESP-IDF. package-release-bin.sh does the same on Linux and WSL.
#
# Two firmware lines, from the same code (FIRMWARE.md):
#   uds  firmware 2.0-uds (was 1.8, profile 1, "gblink"; sdkconfig.defaults): the bridge for a board on a PC's or phone's USB, for
#        mGBA, Azahar and PokeUDS. The 3DS side and GB-Link Switch LDN's Switch side, switching by what the host speaks.
#        Every chip: esp32s3, and esp32, esp32c3 and esp32c6 (alpha: built, not yet run on hardware).
#   aio  firmware 2.0-aio, beta (was 1.9, profile 2, "vcadapter"; sdkconfig.defaults + sdkconfig.aio): the standalone VC-Adapter board
#        for the GB/GBA games, the console's link port on the GPIOs. Game Boy side only for now. esp32s3 only.
# Each builds in its own folder (build-<line>-<chip>, with its own sdkconfig) and carries its version, line and chip in the file name
# and in the app's version (2.0-uds, 2.0-aio), which uds-esp32-setup reads back.
#
# Needs ESP-IDF v6.1 (GB-Link Switch LDN pins commit fff9895c82d744c7237be8847347bdd1b07c6643), installed for every chip it builds
# (install.ps1 esp32,esp32c3,esp32c6,esp32s3). In PowerShell, set it up first:
#   . <ESP-IDF v6.1>\export.ps1
# then, from this folder:
#   .\package-release-bin.ps1                       # everything: ..\..\release\esp32-uds-bridge-2.0-<line>-<chip>.bin
#   .\package-release-bin.ps1 -Line uds             # 2.0-uds for the four chips
#   .\package-release-bin.ps1 -Line uds -Chip esp32s3
#   .\package-release-bin.ps1 -Line aio             # 2.0-aio (esp32s3)
#   .\package-release-bin.ps1 -OutputDir C:\some\folder
# -Profile 1 / gblink and -Profile 2 / vcadapter still work, for uds and aio.

param(
    # -Line (or -Profile, -Board): the parameter itself is named $Target, since $Profile is one of PowerShell's own variables.
    [Alias("Line", "Profile", "Board")]
    [ValidateSet("all", "uds", "aio", "1", "2", "gblink", "vcadapter")]
    [string]$Target = "all",
    [ValidateSet("all", "esp32", "esp32c3", "esp32c6", "esp32s3")]
    [string]$Chip = "all",
    [string]$OutputDir = (Join-Path $PSScriptRoot "..\..\release")
)

$ErrorActionPreference = "Stop"
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "idf.py not found. Set up ESP-IDF v6.1 first: . <ESP-IDF v6.1>\export.ps1"
}
$ErrorActionPreference = "Continue"
$idfVersion = (idf.py --version 2>&1 | Out-String)
$ErrorActionPreference = "Stop"
if ($idfVersion -notmatch "v6\.1") {
    throw "this needs ESP-IDF v6.1 (found: $($idfVersion.Trim())). Releases carry the Switch side, which only builds with v6.1."
}

# The version both lines report (HELLO_ACK; main/Kconfig.projbuild's UDS_FIRMWARE_MAJOR and _MINOR, checked below).
$version = "2.0"
# Each line: its sdkconfig defaults and the chips it is built for.
$lines = [ordered]@{
    uds = @{ defaults = "sdkconfig.defaults"; chips = @("esp32s3", "esp32", "esp32c3", "esp32c6") }
    aio = @{ defaults = "sdkconfig.defaults;sdkconfig.aio"; chips = @("esp32s3") }
}
$aliases = @{ "1" = "uds"; "gblink" = "uds"; "2" = "aio"; "vcadapter" = "aio" }
$line = if ($aliases.ContainsKey($Target)) { $aliases[$Target] } else { $Target }
$chosen = if ($line -eq "all") { @($lines.Keys) } else { @($line) }
if ($Chip -ne "all" -and $line -eq "aio" -and $Chip -ne "esp32s3") {
    throw "2.0-aio is for the ESP32-S3 only (the VC-Adapter board); -Chip $Chip has no aio build"
}

New-Item -ItemType Directory -Force $OutputDir | Out-Null
$outDir = (Resolve-Path $OutputDir).Path

Push-Location $PSScriptRoot
try {
    foreach ($l in $chosen) {
        $chips = if ($Chip -eq "all") { $lines[$l].chips } else { @($Chip) | Where-Object { $lines[$l].chips -contains $_ } }
        foreach ($c in $chips) {
            $buildDir = "build-$l-$c"
            # Windows PowerShell turns anything idf.py writes to stderr (CMake's warnings, for one) into an error under "Stop": the
            # exit code says whether the build worked.
            $ErrorActionPreference = "Continue"
            idf.py -B $buildDir -D "IDF_TARGET=$c" -D "SDKCONFIG=$buildDir/sdkconfig" -D "SDKCONFIG_DEFAULTS=$($lines[$l].defaults)" -D "PROJECT_VER=$version-$l" build
            $built = $LASTEXITCODE -eq 0
            $ErrorActionPreference = "Stop"
            if (-not $built) { throw "the $l build for $c failed" }

            # What the firmware reports and is built for comes from its sdkconfig; it must be what the file is named for.
            $config = Get-Content "$buildDir\sdkconfig" -Raw
            $major = [regex]::Match($config, 'CONFIG_UDS_FIRMWARE_MAJOR=(\d+)').Groups[1].Value
            $minor = [regex]::Match($config, 'CONFIG_UDS_FIRMWARE_MINOR=(\d+)').Groups[1].Value
            $builtLine = [regex]::Match($config, 'CONFIG_UDS_FIRMWARE_LINE="([a-z]+)"').Groups[1].Value
            $builtChip = [regex]::Match($config, 'CONFIG_IDF_TARGET="([a-z0-9]+)"').Groups[1].Value
            if ("$major.$minor" -ne $version) { throw "the $l build for $c reports firmware $major.$minor, not ${version}: check main/Kconfig.projbuild" }
            if ($builtLine -ne $l) { throw "the $l build for $c came out as the '$builtLine' line: check sdkconfig.aio and main/Kconfig.projbuild" }
            if ($builtChip -ne $c) { throw "the $l build for $c is for ${builtChip}: remove $buildDir and build again" }

            $out = Join-Path $outDir "esp32-uds-bridge-$version-$l-$c.bin"
            Push-Location $buildDir
            try {
                # The same layout `idf.py flash` uses (flash_args carries the offsets, and the flash mode, speed and size), merged into
                # one image written at 0x0. The classic ESP32's bootloader sits at 0x1000 in it, the other chips' at 0x0.
                $ErrorActionPreference = "Continue"
                python -m esptool --chip $c merge-bin -o $out "@flash_args"
                $merged = $LASTEXITCODE -eq 0
                $ErrorActionPreference = "Stop"
                if (-not $merged) { throw "merge-bin failed for $l on $c" }
            } finally { Pop-Location }

            $status = if ($c -eq "esp32s3") { "" } else { " (alpha, not yet run on hardware)" }
            "{0}  ({1:N0} bytes) - firmware {2}-{3} for {4}{5}, flash at offset 0x0" -f $out, (Get-Item $out).Length, $version, $l, $c, $status
        }
    }
} finally { Pop-Location }
