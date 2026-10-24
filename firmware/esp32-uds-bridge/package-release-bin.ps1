# Builds the firmware and writes ONE flashable file per profile (bootloader + partition table + app merged at offset 0x0) for a
# GitHub release, so users can flash without installing ESP-IDF.
#
# Two build profiles, from the same code:
#   1, gblink     firmware 1.8 (sdkconfig.defaults): the 3DS side and GB-Link Switch LDN's Switch side, switching by what the host
#                 speaks. For Azahar, mGBA, and a GB-Link through uds-gblink-relay or GB-Link's own clients.
#   2, vcadapter  firmware 2.0 (sdkconfig.defaults + sdkconfig.vcadapter): the VC-Adapter board, the console's link port on the
#                 GPIOs, on its own: the 3DS side for Game Boy games, the Switch side for GBA games (the Wireless Adapter emulated
#                 on the port, and Ruby and Sapphire's cable). Its image is esp32-uds-bridge-fw2.0-esp32s3.bin, with no profile in
#                 the name; 1.9, its Game Boy only first build, was ...-fw1.9-esp32s3-vcadapter.bin.
# Each builds in its own folder (build-<profile name>, with its own sdkconfig) and carries its version and name in the file and in
# the app's version, which uds-esp32-setup reads back.
#
# Needs ESP-IDF v6.1 (GB-Link Switch LDN pins commit fff9895c82d744c7237be8847347bdd1b07c6643). In PowerShell, set it up first:
#   . <ESP-IDF v6.1>\export.ps1
# then, from this folder:
#   .\package-release-bin.ps1                  # both, into ..\..\release
#   .\package-release-bin.ps1 -Profile 1       # firmware 1.8, the GB-Link build: esp32-uds-bridge-fw1.8-esp32s3-gblink.bin
#   .\package-release-bin.ps1 -Profile 2       # firmware 2.0, the VC-Adapter board build: esp32-uds-bridge-fw2.0-esp32s3.bin
#   .\package-release-bin.ps1 -OutputDir C:\some\folder

param(
    # -Profile (or -Board): the parameter itself is named $Target, since $Profile is one of PowerShell's own variables.
    [Alias("Profile", "Board")]
    [ValidateSet("all", "1", "2", "gblink", "vcadapter")]
    [string]$Target = "all",
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

# Each profile: its sdkconfig defaults, its firmware version (main/Kconfig.projbuild, UDS_FIRMWARE_MAJOR and _MINOR, which are
# checked below) and its image's name. The app's version always carries the profile (2.0-vcadapter), which uds-esp32-setup reads.
$profiles = @{
    gblink    = @{ defaults = "sdkconfig.defaults"; version = "1.8"; file = "esp32-uds-bridge-fw1.8-esp32s3-gblink.bin" }
    vcadapter = @{ defaults = "sdkconfig.defaults;sdkconfig.vcadapter"; version = "2.0"; file = "esp32-uds-bridge-fw2.0-esp32s3.bin" }
}
$names = @{ "1" = "gblink"; "2" = "vcadapter" }
$chosen = if ($Target -eq "all") { @("gblink", "vcadapter") } elseif ($names.ContainsKey($Target)) { @($names[$Target]) } else { @($Target) }

New-Item -ItemType Directory -Force $OutputDir | Out-Null
$outDir = (Resolve-Path $OutputDir).Path

Push-Location $PSScriptRoot
try {
    foreach ($p in $chosen) {
        $buildDir = "build-$p"
        $version = $profiles[$p].version
        # Windows PowerShell turns anything idf.py writes to stderr (CMake's warnings, for one) into an error under "Stop": the exit
        # code says whether the build worked.
        $ErrorActionPreference = "Continue"
        idf.py -B $buildDir -D "SDKCONFIG=$buildDir/sdkconfig" -D "SDKCONFIG_DEFAULTS=$($profiles[$p].defaults)" -D "PROJECT_VER=$version-$p" build
        $built = $LASTEXITCODE -eq 0
        $ErrorActionPreference = "Stop"
        if (-not $built) { throw "the $p build failed" }

        # The version the firmware reports (HELLO_ACK) comes from its sdkconfig; it must be the one the file is named for.
        $config = Get-Content "$buildDir\sdkconfig" -Raw
        $major = [regex]::Match($config, 'CONFIG_UDS_FIRMWARE_MAJOR=(\d+)').Groups[1].Value
        $minor = [regex]::Match($config, 'CONFIG_UDS_FIRMWARE_MINOR=(\d+)').Groups[1].Value
        if ("$major.$minor" -ne $version) { throw "the $p build reports firmware $major.$minor, not ${version}: check main/Kconfig.projbuild" }

        $out = Join-Path $outDir $profiles[$p].file
        Push-Location $buildDir
        try {
            # The same layout `idf.py flash` uses (flash_args carries the flash mode, speed and size), merged into one image.
            $ErrorActionPreference = "Continue"
            python -m esptool --chip esp32s3 merge-bin -o $out "@flash_args"
            $merged = $LASTEXITCODE -eq 0
            $ErrorActionPreference = "Stop"
            if (-not $merged) { throw "merge-bin failed for $p" }
        } finally { Pop-Location }

        "{0}  ({1:N0} bytes) - firmware {2}, flash at offset 0x0" -f $out, (Get-Item $out).Length, $version
    }
} finally { Pop-Location }
