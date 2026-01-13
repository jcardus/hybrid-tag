import argparse
import time

import simplepyble


SERVICE_UUID = "8c5debdb-ad8d-4810-a31f-53862e79ee77"
AUTH_UUID = "8c5debdf-ad8d-4810-a31f-53862e79ee77"
KEY_UUID = "8c5debde-ad8d-4810-a31f-53862e79ee77"


def hex_to_bytes(value: str) -> bytes:
    value = value.replace(" ", "")
    return bytes.fromhex(value)


def main() -> None:
    parser = argparse.ArgumentParser(description="Provision Apple key over BLE.")
    parser.add_argument("--macid", help="BLE MAC/UUID (platform-specific)")
    parser.add_argument("--name", help="BLE name to match (e.g. HYBRID-TAG)")
    parser.add_argument("--auth", default="abcdefgh", help="8-byte auth code (default: abcdefgh)")
    parser.add_argument("--key", required=True, help="28-byte Apple key (56 hex chars)")
    args = parser.parse_args()

    key = hex_to_bytes(args.key)
    if len(key) != 28:
        raise SystemExit("Key must be 28 bytes (56 hex chars)")

    auth = args.auth.encode("utf-8")
    if len(auth) != 8:
        raise SystemExit("Auth code must be exactly 8 bytes")

    adapters = simplepyble.Adapter.get_adapters()
    if not adapters:
        raise SystemExit("No BLE adapters found")
    adapter = adapters[0]

    if not args.macid and not args.name:
        raise SystemExit("Provide --macid or --name")

    target = args.macid.upper() if args.macid else None
    print("Scanning...")
    peripheral = None
    while peripheral is None:
        adapter.scan_for(100)
        results = adapter.scan_get_results()
        print("Found devices:")
        for p in results:
            if hasattr(p, "is_connectable") and not p.is_connectable():
                continue
            name = p.identifier()
            addr = p.address()
            details = [f"name={name} uuid={addr}"]
            if hasattr(p, "rssi"):
                details.append(f"rssi={p.rssi()}")
            if hasattr(p, "is_connectable"):
                details.append(f"connectable={p.is_connectable()}")
            print("- " + " ".join(details))
        for p in results:
            if target and p.address() == target:
                peripheral = p
                break
            if args.name and p.identifier() == args.name:
                peripheral = p
                break
        if peripheral is None:
            print("Device not found, retrying...")
            time.sleep(1)

    print(f"Found {peripheral.address()}, connecting...")
    peripheral.connect()
    print("Connected")

    part1 = key[:14]
    part2 = key[14:]

    print("Writing auth...")
    peripheral.write_request(SERVICE_UUID, AUTH_UUID, auth)
    print("Writing key part 1...")
    peripheral.write_request(SERVICE_UUID, KEY_UUID, part1)
    print("Writing key part 2...")
    peripheral.write_request(SERVICE_UUID, KEY_UUID, part2)
    print("Done. Device should reboot.")
    peripheral.disconnect()


if __name__ == "__main__":
    main()
