# Builds the firmware and writes ONE flashable file (bootloader + partition table + app merged at
# offset 0x0) for a GitHub release, so users can flash without installing ESP-IDF.
#
# Two board profiles, from the same code:
#   gblink     sdkconfig.defaults: a board on the PC's (or phone's) USB, with a GB-Link through uds-gblink-relay
#   vcadapter  sdkconfig.defaults + sdkconfig.vcadapter: the VC-Adapter board, the console's link port on the GPIOs
# Each builds in its own folder (build-<board>, with its own sdkconfig) and carries its name in the file and in the app's version,
# which uds-esp32-setup reads back.
#
# Run it from an "ESP-IDF 5.2 PowerShell" (Desktop/Start menu shortcut):
#   .\package-release-bin.ps1                       # both: ..\..\release\esp32-uds-bridge-fw<ver>-esp32s3-<board>.bin
#   .\package-release-bin.ps1 -Board vcadapter
#   .\package-release-bin.ps1 -OutputDir C:\some\folder

param(
    [ValidateSet("all", "gblink", "vcadapter")]
    [string]$Board = "all",
    [string]$OutputDir = (Join-Path $PSScriptRoot "..\..\release")
)

$ErrorActionPreference = "Stop"
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "idf.py not found. Run this from the 'ESP-IDF 5.2 PowerShell' shortcut."
}

$profiles = @{
    gblink    = "sdkconfig.defaults"
    vcadapter = "sdkconfig.defaults;sdkconfig.vcadapter"
}
$boards = if ($Board -eq "all") { @("gblink", "vcadapter") } else { @($Board) }

# Firmware version, from main.c (FW_MAJOR / FW_MINOR).
$main = Get-Content (Join-Path $PSScriptRoot "main\main.c") -Raw
$major = [regex]::Match($main, '#define FW_MAJOR (\d+)').Groups[1].Value
$minor = [regex]::Match($main, '#define FW_MINOR (\d+)').Groups[1].Value

New-Item -ItemType Directory -Force $OutputDir | Out-Null
$outDir = (Resolve-Path $OutputDir).Path

Push-Location $PSScriptRoot
try {
    foreach ($b in $boards) {
        $buildDir = "build-$b"
        # Windows PowerShell turns anything idf.py writes to stderr (CMake's warnings, for one) into an error under "Stop": the exit
        # code says whether the build worked.
        $ErrorActionPreference = "Continue"
        idf.py -B $buildDir -D "SDKCONFIG=$buildDir/sdkconfig" -D "SDKCONFIG_DEFAULTS=$($profiles[$b])" -D "PROJECT_VER=$major.$minor-$b" build
        $built = $LASTEXITCODE -eq 0
        $ErrorActionPreference = "Stop"
        if (-not $built) { throw "the $b build failed" }

        $out = Join-Path $outDir "esp32-uds-bridge-fw$major.$minor-esp32s3-$b.bin"
        Push-Location $buildDir
        try {
            # The same layout `idf.py flash` uses (see flash_args), merged into one image.
            $ErrorActionPreference = "Continue"
            python -m esptool --chip esp32s3 merge_bin -o $out --flash_mode dio --flash_freq 80m --flash_size 4MB "@flash_args"
            $merged = $LASTEXITCODE -eq 0
            $ErrorActionPreference = "Stop"
            if (-not $merged) { throw "merge_bin failed for $b" }
        } finally { Pop-Location }

        "{0}  ({1:N0} bytes) - flash at offset 0x0" -f $out, (Get-Item $out).Length
    }
} finally { Pop-Location }
