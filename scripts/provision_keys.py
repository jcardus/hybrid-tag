import argparse
import asyncio
import base64

from bleak import BleakClient, BleakScanner


SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
APPLE_KEY_UUID = "12345678-1234-5678-1234-56789abcdef1"
GOOGLE_KEY_UUID = "12345678-1234-5678-1234-56789abcdef2"


async def main() -> None:
    parser = argparse.ArgumentParser(description="Provision Apple and Google keys over BLE.")
    parser.add_argument("--name", default="HYBRID-TAG", help="BLE name to match")
    parser.add_argument("--key", default="WPS9RJBtGkPLvMvFBhvKkofMabkdsdiPzLBSzg==", help="28-byte Apple key (base64)")
    parser.add_argument("--keyGoogle", default="34aaaffb11e8bf854630bd2ce56fa6b06603b20b", help="20-byte Google key (hex)")
    args = parser.parse_args()

    apple_key = base64.b64decode(args.key)
    if len(apple_key) != 28:
        raise SystemExit("Apple key must be 28 bytes")

    google_key = bytes.fromhex(args.keyGoogle)
    if len(google_key) != 20:
        raise SystemExit("Google key must be 20 bytes")

    print("Scanning...")
    device = await BleakScanner.find_device_by_name(args.name, timeout=60.0)
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

        # Split Apple key into chunks that fit within MTU
        chunk_size = mtu - 3  # 20 bytes for MTU 23
        print(f"\nWriting Apple key ({len(apple_key)} bytes) in {(len(apple_key) + chunk_size - 1) // chunk_size} chunks...")

        for i in range(0, len(apple_key), chunk_size):
            chunk = apple_key[i:i + chunk_size]
            print(f"  Writing chunk {i // chunk_size + 1}: {len(chunk)} bytes (offset {i})")
            await client.write_gatt_char(APPLE_KEY_UUID, chunk, response=True)

        print("Apple key written!")

        # Write Google key (20 bytes fits in single write)
        print(f"\nWriting Google key ({len(google_key)} bytes)...")
        await client.write_gatt_char(GOOGLE_KEY_UUID, google_key, response=True)
        print("Google key written!")

        print("\nDone! Both keys configured.")


if __name__ == "__main__":
    asyncio.run(main())
