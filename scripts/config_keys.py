import argparse
import asyncio

from bleak import BleakClient, BleakScanner


SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
APPLE_KEY_UUID = "12345678-1234-5678-1234-56789abcdef1"


def hex_to_bytes(value: str) -> bytes:
    value = value.replace(" ", "")
    return bytes.fromhex(value)


async def main() -> None:
    parser = argparse.ArgumentParser(description="Provision Apple key over BLE.")
    parser.add_argument("--name", default="HYBRID-TAG", help="BLE name to match")
    parser.add_argument("--key", default="0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c", help="28-byte Apple key (56 hex chars)")
    args = parser.parse_args()

    key = hex_to_bytes(args.key)
    if len(key) != 28:
        raise SystemExit("Key must be 28 bytes (56 hex chars)")

    print("Scanning...")
    device = await BleakScanner.find_device_by_name(args.name, timeout=30.0)
    if not device:
        raise SystemExit(f"Device '{args.name}' not found")

    print(f"Found {device.name}, connecting...")
    async with BleakClient(device) as client:
        print("Connected")

        # List services and characteristics
        for service in client.services:
            print(f"Service: {service.uuid}")
            for char in service.characteristics:
                print(f"  Characteristic: {char.uuid}")
                print(f"  Properties: {char.properties}")

        # Get MTU
        mtu = client.mtu_size
        print(f"\nMTU: {mtu} bytes (max write: {mtu - 3} bytes)")

        # Split key into chunks that fit within MTU
        chunk_size = mtu - 3  # 20 bytes for MTU 23
        print(f"\nWriting {len(key)} bytes in {(len(key) + chunk_size - 1) // chunk_size} chunks...")

        for i in range(0, len(key), chunk_size):
            chunk = key[i:i + chunk_size]
            print(f"  Writing chunk {i // chunk_size + 1}: {len(chunk)} bytes (offset {i})")
            await client.write_gatt_char(APPLE_KEY_UUID, chunk, response=True)

        print("Done!")


if __name__ == "__main__":
    asyncio.run(main())
