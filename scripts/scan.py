#!/usr/bin/env python3
"""Scan for AirTags and display their MAC address and public key."""

import argparse
import time
import simplepyble

def main() -> None:
    parser = argparse.ArgumentParser(description="Scan for AirTags and display MAC/public key.")
    parser.add_argument("--duration", type=int, default=10000, help="Scan duration in ms (default: 8000)")
    parser.add_argument("--rssi", type=int, default=-70, help="Minimum RSSI filter (default: -70)")
    parser.add_argument("--cid", nargs='*', default=[0xfff1, 0xfff2, 0xfff3], type=lambda x: int(x, 0), help="Filter by company IDs (e.g., --cid 0x004C 0x12fa)")
    parser.add_argument("--name", default='hybrid', type=str, help="Include devices matching this name (substring match)")
    parser.add_argument("--continuous", default=1, action="store_true", help="Scan continuously")
    args = parser.parse_args()

    adapters = simplepyble.Adapter.get_adapters()
    if not adapters:
        raise SystemExit("No BLE adapters found")
    adapter = adapters[0]

    print(f"Scanning (RSSI > {args.rssi} dBm)...")
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
                mfg_data = p.manufacturer_data() if hasattr(p, "manufacturer_data") else {}

                # Include if: CID matches OR name matches
                dev_name = (p.identifier() or "").lower()
                cid_match = args.cid and any(cid in mfg_data for cid in args.cid)
                name_match = args.name and args.name.lower() in dev_name

                if not (cid_match or name_match):
                    continue

                seen_devices.add(addr)

                # Display information
                name = p.identifier() if p.identifier() else "(unnamed)"
                rssi = p.rssi() if hasattr(p, "rssi") else "N/A"

                print(f"\n{name}")
                print(f"  addr: {addr}")
                print(f"  RSSI: {rssi} dBm")
                for company_id, data in mfg_data.items():
                    if args.cid and company_id not in args.cid:
                        continue
                    print(f"  Company ID: 0x{company_id:04X}")
                    print(f"  Data ({len(data)}): {data.hex()}")
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
