#!/usr/bin/env bash
set -euo pipefail

: "${NCS_REV:=v2.8.0}"

mkdir -p ncs
cd ncs || exit
west init -m https://github.com/nrfconnect/sdk-nrf --mr ${NCS_REV}
west update
west zephyr-export
python -m pip install -r zephyr/scripts/requirements.txt
