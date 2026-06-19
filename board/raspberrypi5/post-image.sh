#!/bin/bash

set -e

# $1 is the first argument passed by Buildroot, which is the images directory (output/images)
BINARIES_DIR="$1"
BOARD_DIR="$(dirname $0)"
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# 2. Copy wpa_supplicant-wlan0.conf template into BINARIES_DIR
# This allows genimage to find it and put it on the FAT partition.
cp -f "${BOARD_DIR}/wpa_supplicant-wlan0.conf" \
      "${BINARIES_DIR}/wpa_supplicant-wlan0.conf"

# 3. Generate the SD Card Image
# We use a temporary directory for rootpath to keep the image generation clean.
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"

rm -rf "${GENIMAGE_TMP}"

genimage \
    --rootpath "${ROOTPATH_TMP}"   \
    --tmppath "${GENIMAGE_TMP}"    \
    --inputpath "${BINARIES_DIR}"  \
    --outputpath "${BINARIES_DIR}" \
    --config "${GENIMAGE_CFG}"

exit $?