#!/usr/bin/env python3
# mac_ble_advertise_only.py
import time
import struct
import uuid
import objc

from Foundation import NSObject, NSRunLoop, NSDate, NSData
from CoreBluetooth import (
    CBPeripheralManager,
    CBAdvertisementDataLocalNameKey,
    CBAdvertisementDataManufacturerDataKey,
)

# ---- iBeacon payload (common broadcast-only pattern) ----
# Format: CompanyID (Apple=0x004C) + type(0x02) + len(0x15) + proximity UUID + major + minor + txPower
APPLE_COMPANY_ID = 0x004C

PROX_UUID = uuid.UUID("E2C56DB5-DFFB-48D2-B060-D0F5A71096E0")
MAJOR = 1
MINOR = 2
TX_POWER = -59  # typical calibrated value

def ibeacon_manufacturer_data():
    prefix = struct.pack(">HBB", APPLE_COMPANY_ID, 0x02, 0x15)  # big-endian company id per iBeacon examples
    payload = (
            PROX_UUID.bytes
            + struct.pack(">HHb", MAJOR, MINOR, TX_POWER)
    )
    return prefix + payload

class AdvDelegate(NSObject):
    def init(self):
        self = objc.super(AdvDelegate, self).init()
        if self is None:
            return None
        self.pm = CBPeripheralManager.alloc().initWithDelegate_queue_options_(self, None, None)
        self.started = False
        return self

    def peripheralManagerDidUpdateState_(self, peripheral):
        # 5 == poweredOn
        if peripheral.state() != 5:
            print("Bluetooth not powered on / unavailable. State =", peripheral.state())
            return

        if self.started:
            return

        mfg = ibeacon_manufacturer_data()
        nsdata = NSData.dataWithBytes_length_(mfg, len(mfg))

        adv = {
            # Local name is optional; some beacon formats omit it
            CBAdvertisementDataLocalNameKey: "NCB",
            CBAdvertisementDataManufacturerDataKey: nsdata,
        }

        print("Starting advertising (manufacturer data only)...")
        peripheral.startAdvertising_(adv)
        self.started = True

    def peripheralManagerDidStartAdvertising_error_(self, peripheral, error):
        if error is not None:
            print("Advertising failed:", error)
        else:
            print("Advertising started.")

if __name__ == "__main__":
    d = AdvDelegate.alloc().init()
    rl = NSRunLoop.currentRunLoop()
    while True:
        rl.runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.2))
        time.sleep(0.2)
