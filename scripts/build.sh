#!/bin/bash
set -e

# Usage: ./build.sh [uf2|openocd] [board]
# Examples:
#   ./build.sh uf2                    # Build and flash via UF2 (nrf52840)
#   ./build.sh openocd                # Build and flash via OpenOCD (nrf52832)
#   ./build.sh openocd nrf52dk/nrf52832

METHOD=${1:-"openocd"}
BOARD=${2:-"nrf52dk/nrf52832"} # promicro_nrf52840/nrf52840"

source ../ncs/export_env.sh

echo "========================================"
echo "  Build Method: ${METHOD}"
echo "  Board: ${BOARD}"
echo "========================================"

cd ../ncs
west build -p always -b "${BOARD}" -s ..

  HEX_FILE="build/merged.hex"

if [ "${METHOD}" == "uf2" ]; then
  #############################################
  # UF2 Flashing (for nRF52840 with bootloader)
  #############################################
  UF2_FILE="build/zephyr.uf2"

  # Map board to UF2 family ID
  if [[ "${BOARD}" == *"nrf52840"* ]]; then
    FAMILY_ID="0xADA52840"
  else
    echo "Error: UF2 not supported for ${BOARD}"
    exit 1
  fi

  # Download uf2conv.py tool if not present
  if [ ! -f "uf2conv.py" ]; then
    echo "Downloading uf2conv.py..."
    curl -L -o uf2conv.py https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2conv.py
    chmod +x uf2conv.py
  fi

  # Download uf2families.json if not present
  if [ ! -f "uf2families.json" ]; then
    echo "Downloading uf2families.json..."
    curl -L -o uf2families.json https://raw.githubusercontent.com/microsoft/uf2/master/utils/uf2families.json
  fi

  # Convert hex to UF2
  echo "Converting ${HEX_FILE} to UF2..."
  python3 uf2conv.py "${HEX_FILE}" -c -f ${FAMILY_ID} -o "${UF2_FILE}"

  echo "UF2 file generated:"
  ls -lh "${UF2_FILE}"

  # Copy UF2 to mounted device
  if [ -d "/Volumes/NICENANO" ]; then
    cp -X "${UF2_FILE}" /Volumes/NICENANO/
    echo "Copied to /Volumes/NICENANO/"
    screen /dev/cu.usbmodem1101 115200
  else
    echo "Warning: /Volumes/NICENANO not found. Copy manually."
  fi

elif [ "${METHOD}" == "openocd" ]; then
  #############################################
  # OpenOCD Flashing (for nRF52832 via ST-Link)
  #############################################
  OPENOCD_CFG="../openocd.cfg"

  if [ ! -f "${OPENOCD_CFG}" ]; then
    echo "Error: ${OPENOCD_CFG} not found"
    exit 1
  fi

  echo "Flashing via OpenOCD..."
  openocd -f "${OPENOCD_CFG}" -c "init; halt; nrf51 mass_erase; program ${HEX_FILE} verify; reset; exit"

  echo "Flash complete!"

else
  echo "Error: Unknown method '${METHOD}'"
  echo "Usage: $0 [uf2|openocd] [board]"
  exit 1
fi