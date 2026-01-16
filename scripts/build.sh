#!/bin/bash
set -e

# Usage: ./build.sh [uf2|openocd|rtt] [board]
# Examples:
#   ./build.sh uf2                    # Build and flash via UF2 with USB (nrf52840)
#   ./build.sh openocd                # Build and flash via OpenOCD (nrf52832)
#   ./build.sh rtt                    # Build, flash, and monitor RTT logs (nrf52832)

METHOD=${1:-"openocd"}
BOARD=${2:-"promicro_nrf52840/nrf52840"} # promicro_nrf52840/nrf52840"

source ../ncs/export_env.sh

echo "========================================"
echo "  Build Method: ${METHOD}"
echo "  Board: ${BOARD}"
echo "========================================"

cd ../ncs

# Add config overlays based on method
if [ "${METHOD}" == "rtt" ]; then
  west build -p always -b "${BOARD}" -s .. -- -DEXTRA_CONF_FILE=prj.rtt.conf
elif [ "${METHOD}" == "uf2" ]; then
  west build -p always -b "${BOARD}" -s .. -- -DEXTRA_CONF_FILE=prj.usb.conf
else
  west build -p always -b "${BOARD}" -s ..
fi

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

    # Find the USB serial port
    USB_PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1)
    if [ -n "$USB_PORT" ]; then
      echo "Connecting to $USB_PORT..."
      screen "$USB_PORT" 115200
    else
      echo "Warning: No USB serial port found (/dev/cu.usbmodem*)"
    fi
  else
    echo "Warning: /Volumes/NICENANO not found. Copy manually."
  fi

elif [ "${METHOD}" == "openocd" ] || [ "${METHOD}" == "rtt" ]; then
  #############################################
  # OpenOCD Flashing (for nRF52832 via ST-Link)
  #############################################
  OPENOCD_CFG="../openocd.cfg"

  if [ ! -f "${OPENOCD_CFG}" ]; then
    echo "Error: ${OPENOCD_CFG} not found"
    exit 1
  fi

  # Kill any existing OpenOCD processes
  pkill -9 openocd 2>/dev/null || true

  echo "Flashing via OpenOCD..."
  openocd -f "${OPENOCD_CFG}" -c "init; halt; nrf5 mass_erase; program ${HEX_FILE} verify; reset; exit"
  echo "Flash complete!"

  # If RTT mode, start RTT monitor
  if [ "${METHOD}" == "rtt" ]; then
    echo ""
    echo "Starting RTT monitor..."
    echo "Press Ctrl+C to exit"
    echo ""
    openocd -f "${OPENOCD_CFG}" \
      -c "init" \
      -c "rtt setup 0x20000000 0x10000 \"SEGGER RTT\"" \
      -c "rtt start" \
      -c "rtt server start 9090 0" &
    OPENOCD_PID=$!
    sleep 2
    echo "=== RTT Output ==="
    nc -v localhost 9090
    kill $OPENOCD_PID 2>/dev/null || true
  fi
else
  echo "Error: Unknown method '${METHOD}'"
  echo "Usage: $0 [uf2|openocd|rtt] [board]"
  exit 1
fi