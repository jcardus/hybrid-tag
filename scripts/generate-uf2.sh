#!/bin/bash
set -e

BOARD=${1:-""}
HEX_FILE=${2:-"ncs/build/merged.hex"}
UF2_FILE=${3:-"ncs/build/zephyr.uf2"}

if [ -z "$BOARD" ]; then
  echo "Usage: $0 <board> [hex_file] [uf2_file]"
  exit 1
fi

# Map board to UF2 family ID
if [[ "${BOARD}" == *"nrf52840"* ]]; then
  FAMILY_ID="0xADA52840"
else
  echo "Skipping UF2 generation - no family ID mapped for ${BOARD}"
  exit 0
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
echo "Converting ${HEX_FILE} to UF2 for ${BOARD}..."
python3 uf2conv.py "${HEX_FILE}" -c -f ${FAMILY_ID} -o "${UF2_FILE}"

echo "UF2 file generated for ${BOARD}:"
ls -lh "${UF2_FILE}"
