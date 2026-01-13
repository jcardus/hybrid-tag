#!/usr/bin/env python3
"""Scan for AirTags and display their MAC address and public key."""

import argparse
import time
import simplepyble


def parse_apple_data(company_id: int, data: bytes) -> dict:
    """Parse Apple manufacturer data to extract Find My info."""
    result = {}

    # Check if it's Apple (0x004C)
    if company_id != 0x004C:
        return result

    result["company"] = "Apple"

    if len(data) < 1:
        return result

    # Byte 0 is the advertisement type
    adv_type = data[0]
    result["type"] = f"0x{adv_type:02X}"

    # Type 0x12 is Find My / Offline Finding
    if adv_type == 0x12:
        result["protocol"] = "Find My / Offline Finding"

        if len(data) >= 3:
            # Byte 1: Length
            length = data[1]
            result["length"] = length

            # Byte 2: Status
            status = data[2]
            result["status"] = f"0x{status:02X}"

            # Remaining bytes contain public key and hint
            # Format: [type][length][status][public_key (22 bytes)][hint][reserved]
            if len(data) >= 25:
                # Extract what appears to be the public key portion
                # Bytes 3 to end-2 are typically the public key data
                key_data = data[3:-2] if len(data) > 5 else data[3:]
                result["public_key"] = key_data.hex()
                result["public_key_len"] = len(key_data)

                if len(data) >= 26:
                    hint = data[-2]
                    result["hint"] = f"0x{hint:02X}"

    # Type 0x07 is Nearby/Continuity
    elif adv_type == 0x07:
        result["protocol"] = "Apple Continuity"

    # Type 0x09 is AirPlay
    elif adv_type == 0x09:
        result["protocol"] = "AirPlay"

    # Type 0x0C is Handoff
    elif adv_type == 0x0C:
        result["protocol"] = "Handoff"

    # Type 0x10 is Nearby Info
    elif adv_type == 0x10:
        result["protocol"] = "Nearby Info"

    # Store raw data
    result["raw_data"] = data.hex()
    result["raw_data_len"] = len(data)

    return result


def main() -> None:
    parser = argparse.ArgumentParser(description="Scan for AirTags and display MAC/public key.")
    parser.add_argument("--duration", type=int, default=10000, help="Scan duration in ms (default: 8000)")
    parser.add_argument("--rssi", type=int, default=-70, help="Minimum RSSI filter (default: -50)")
    parser.add_argument("--continuous", action="store_true", help="Scan continuously")
    args = parser.parse_args()

    adapters = simplepyble.Adapter.get_adapters()
    if not adapters:
        raise SystemExit("No BLE adapters found")
    adapter = adapters[0]

    print(f"Scanning for AirTags (RSSI > {args.rssi} dBm)...")
    print("=" * 80)

    seen_devices = set()

    try:
        while True:
            adapter.scan_for(args.duration)
            results = adapter.scan_get_results()

            for p in results:
                addr = p.address()

                # Skip if we've already seen this device (in continuous mode)
                if args.continuous and addr in seen_devices:
                    continue

                # Check RSSI filter
                if hasattr(p, "rssi") and p.rssi() <= args.rssi:
                    continue

                # Get manufacturer data
                if not hasattr(p, "manufacturer_data"):
                    continue

                mfg_data = p.manufacturer_data()
                if not mfg_data:
                    continue

                # Look for Apple Find My devices
                apple_data = None
                for company_id, data in mfg_data.items():
                    data_bytes = bytes(data) if not isinstance(data, bytes) else data
                    parsed = parse_apple_data(company_id, data_bytes)
                    if parsed.get("protocol") == "Find My / Offline Finding" and "public_key" in parsed:
                        apple_data = parsed
                        break

                if not apple_data:
                    continue

                # Mark as seen
                seen_devices.add(addr)

                # Display information
                name = p.identifier() if p.identifier() else "(unnamed)"
                rssi = p.rssi() if hasattr(p, "rssi") else "N/A"

                print(f"\n{name}")
                print(f"  RSSI: {rssi} dBm")
                if "status" in apple_data:
                    print(f"  Status: {apple_data['status']}")
                if "hint" in apple_data:
                    print(f"  Hint: {apple_data['hint']}")
                if "public_key" in apple_data:
                    key = apple_data["public_key"]
                    print(f"  Public Key (last {apple_data['public_key_len']} bytes): {key}")
                print("-" * 80)

            if not args.continuous:
                break

            print(f"\nContinuing scan... (Found {len(seen_devices)} unique devices so far)")
            time.sleep(1)

    except KeyboardInterrupt:
        print("\n\nScan stopped.")
        print(f"Total unique devices found: {len(seen_devices)}")


if __name__ == "__main__":
    main()
