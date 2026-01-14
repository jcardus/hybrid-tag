import argparse
import asyncio
import base64

from bleak import BleakClient, BleakScanner


SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
APPLE_KEY_UUID = "12345678-1234-5678-1234-56789abcdef1"


async def main() -> None:
    parser = argparse.ArgumentParser(description="Provision Apple key over BLE.")
    parser.add_argument("--name", default="HYBRID-TAG", help="BLE name to match")
    parser.add_argument("--key", default="WPS9RJBtGkPLvMvFBhvKkofMabkdsdiPzLBSzg==", help="28-byte Apple key (base64)")
    parser.add_argument("--keyGoogle", default="34aaaffb11e8bf854630bd2ce56fa6b06603b20b", help="20-byte Google key (hexa)")
    args = parser.parse_args()

    key = base64.b64decode(args.key)
    if len(key) != 28:
        raise SystemExit("Key must be 28 bytes")

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
