#!/usr/bin/env bash
# package-release-bin.ps1 for Linux, macOS and WSL: builds the firmware and writes one flashable file per line and chip
# (bootloader + partition table + app merged at offset 0x0). See the .ps1 and FIRMWARE.md for the lines and chips.
#
# Needs ESP-IDF v6.1 (commit fff9895c82d744c7237be8847347bdd1b07c6643) installed for every chip it builds
# (./install.sh esp32,esp32s2,esp32c3,esp32c6,esp32s3), set up first:  . <ESP-IDF v6.1>/export.sh
#   ./package-release-bin.sh                      # everything: ../../release/esp32-uds-bridge-2.1-<line>-<chip>.bin
#   ./package-release-bin.sh uds                  # 2.1-uds (3DS side only: Azahar, PokeUDS) for the five chips
#   ./package-release-bin.sh uds esp32c6          # one image
#   ./package-release-bin.sh ldn                  # 2.1-ldn (3DS and Switch sides: mGBA-LDN) for the four chips (no S2)
#   ./package-release-bin.sh aio                  # 2.1-aio (esp32s3)
#   OUTPUT_DIR=/some/folder ./package-release-bin.sh
set -euo pipefail

line=${1:-all}
chip=${2:-all}
version=2.1
# 1 / gblink was the build with both sides (1.8, then 2.0-uds): the ldn line now. "uds" has meant the 3DS side alone since 2.1.
case "$line" in 1|gblink) line=ldn ;; 2|vcadapter) line=aio ;; all|uds|ldn|aio) ;; *) echo "line: all, uds, ldn or aio" >&2; exit 2 ;; esac
case "$chip" in all|esp32|esp32s2|esp32c3|esp32c6|esp32s3) ;; *) echo "chip: all, esp32, esp32s2, esp32c3, esp32c6 or esp32s3" >&2; exit 2 ;; esac
if [ "$line" = aio ] && [ "$chip" != all ] && [ "$chip" != esp32s3 ]; then
    echo "2.1-aio is for the ESP32-S3 only (the VC-Adapter board); there is no aio build for $chip" >&2; exit 2
fi
if [ "$line" = ldn ] && [ "$chip" = esp32s2 ]; then
    echo "the ESP32-S2 has no Switch side, so no ldn build: use uds" >&2; exit 2
fi
command -v idf.py > /dev/null || { echo "idf.py not found. Set up ESP-IDF v6.1 first: . <ESP-IDF v6.1>/export.sh" >&2; exit 1; }
idf_version=$(idf.py --version 2>&1)
case "$idf_version" in *v6.1*) ;; *) echo "this needs ESP-IDF v6.1 (found: $idf_version)" >&2; exit 1 ;; esac

here=$(cd "$(dirname "$0")" && pwd)
out_dir=${OUTPUT_DIR:-$here/../../release}
mkdir -p "$out_dir"
out_dir=$(cd "$out_dir" && pwd)

defaults_uds="sdkconfig.defaults;sdkconfig.uds";  chips_uds="esp32s3 esp32 esp32s2 esp32c3 esp32c6"
defaults_ldn="sdkconfig.defaults";                chips_ldn="esp32s3 esp32 esp32c3 esp32c6"
defaults_aio="sdkconfig.defaults;sdkconfig.aio";  chips_aio="esp32s3"
lines=$([ "$line" = all ] && echo "uds ldn aio" || echo "$line")

cd "$here"
for l in $lines; do
    defaults_var=defaults_$l; chips_var=chips_$l
    for c in ${!chips_var}; do
        [ "$chip" = all ] || [ "$chip" = "$c" ] || continue
        build_dir=build-$l-$c
        idf.py -B "$build_dir" -D "IDF_TARGET=$c" -D "SDKCONFIG=$build_dir/sdkconfig" -D "SDKCONFIG_DEFAULTS=${!defaults_var}" \
            -D "PROJECT_VER=$version-$l" build

        # What the firmware reports and is built for comes from its sdkconfig; it must be what the file is named for.
        config=$build_dir/sdkconfig
        built_version="$(sed -n 's/^CONFIG_UDS_FIRMWARE_MAJOR=//p' "$config").$(sed -n 's/^CONFIG_UDS_FIRMWARE_MINOR=//p' "$config")"
        built_line=$(sed -n 's/^CONFIG_UDS_FIRMWARE_LINE="\(.*\)"/\1/p' "$config")
        built_chip=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$config")
        [ "$built_version" = "$version" ] || { echo "the $l build for $c reports firmware $built_version, not $version" >&2; exit 1; }
        [ "$built_line" = "$l" ] || { echo "the $l build for $c came out as the '$built_line' line" >&2; exit 1; }
        [ "$built_chip" = "$c" ] || { echo "the $l build for $c is for $built_chip: remove $build_dir and build again" >&2; exit 1; }

        out=$out_dir/esp32-uds-bridge-$version-$l-$c.bin
        # The layout `idf.py flash` uses (flash_args), merged into one image written at 0x0. The classic ESP32 and S2 bootloaders sit at
        # 0x1000 in it, the other chips' at 0x0.
        (cd "$build_dir" && python -m esptool --chip "$c" merge-bin -o "$out" "@flash_args")
        status=""; [ "$c" = esp32s3 ] || [ "$c" = esp32s2 ] || status=" (alpha, not yet run on hardware)"
        echo "$out  ($(stat -c %s "$out" 2>/dev/null || stat -f %z "$out") bytes) - firmware $version-$l for $c$status, flash at offset 0x0"
    done
done
