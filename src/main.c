/* main.c - Hybrid Tag: Apple FindMy & Google FMDN Tracker */

/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/controller.h>

#include "keys.h"

/* Protocol selection */
typedef enum {
	PROTOCOL_APPLE_FINDMY,
	PROTOCOL_GOOGLE_FMDN,
} protocol_t;

/* Current active protocol */
static protocol_t current_protocol = PROTOCOL_APPLE_FINDMY;

/* Protocol switch interval in seconds (1 minute per protocol) */
#define PROTOCOL_SWITCH_INTERVAL_SEC 60

/*
 * Apple FindMy Offline Finding Advertisement Format:
 * - Uses manufacturer-specific data (Apple Company ID: 0x004C)
 * - Payload structure:
 *   [0-1]:  Apple Company ID (0x4C, 0x00) - 2 bytes
 *   [2]:    Type (0x12 for Offline Finding) - 1 byte
 *   [3]:    Length (0x19 = 25 bytes following) - 1 byte
 *   [4]:    Status byte (battery, motion, etc.) - 1 byte
 *   [5-26]: Public key (22 bytes from 28-byte P-224 key) - 22 bytes
 *   [27]:   First two bits of remaining key material - 1 byte
 *   [28]:   Hint byte - 1 byte
 *   Total: 2 + 1 + 1 + 1 + 22 + 1 + 1 = 29 bytes
 */
#define APPLE_FINDMY_PAYLOAD_SIZE 29
static uint8_t apple_findmy_payload[APPLE_FINDMY_PAYLOAD_SIZE];

static struct bt_data apple_ad[] = {
	BT_DATA(BT_DATA_MANUFACTURER_DATA, apple_findmy_payload, APPLE_FINDMY_PAYLOAD_SIZE),
};

/*
 * Google FMDN (Find My Device Network) Advertisement Format:
 * - Uses Service Data for Fast Pair extension
 * - UUID: 0xFE2C (Google Fast Pair)
 * - Frame type: 0x00 for FHN (Find Hub Network)
 * - Payload structure:
 *   [0-1]:  Service UUID (0xFE2C) - 2 bytes
 *   [2]:    Frame type (0x00) - 1 byte
 *   [3-22]: Public key (20 bytes, 160-bit ECC) - 20 bytes
 *   Total: 2 + 1 + 20 = 23 bytes
 */
#define GOOGLE_FMDN_PAYLOAD_SIZE 23
static uint8_t google_fmdn_payload[GOOGLE_FMDN_PAYLOAD_SIZE];

static struct bt_data google_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_SVC_DATA16, google_fmdn_payload, GOOGLE_FMDN_PAYLOAD_SIZE),
};

/* Prepare Apple FindMy advertisement payload */
static void prepare_apple_findmy_adv(void)
{
	/* Apple Company ID (little-endian: 0x004C) */
	apple_findmy_payload[0] = 0x4C;
	apple_findmy_payload[1] = 0x00;

	/* Type: 0x12 for Offline Finding */
	apple_findmy_payload[2] = 0x12;

	/* Length: 0x19 (25 bytes following) */
	apple_findmy_payload[3] = 0x19;

	/* Status byte: 0x00 (no battery/motion info) */
	apple_findmy_payload[4] = 0x00;

	/* Copy 22 bytes of public key starting from byte 6 (bytes 6-27) */
	memcpy(&apple_findmy_payload[5], &apple_keys[0][6], 22);

	/* First two bits from key[0] (top 2 bits of the byte used in MAC) */
	apple_findmy_payload[27] = (apple_keys[0][0] >> 6) & 0x03;

	/* Hint byte: 0x00 */
	apple_findmy_payload[28] = 0x00;
}

/* Prepare Google FMDN advertisement payload */
static void prepare_google_fmdn_adv(void)
{
	/* Fast Pair Service UUID (little-endian: 0xFE2C) */
	google_fmdn_payload[0] = 0x2C;
	google_fmdn_payload[1] = 0xFE;

	/* Frame type: 0x00 for FHN (Find Hub Network) */
	google_fmdn_payload[2] = 0x00;

	/* Copy public key (20 bytes for 160-bit ECC) */
	memcpy(&google_fmdn_payload[3], google_key, GOOGLE_KEY_SIZE);
}

/* Set BLE MAC address based on protocol (following Everytag implementation) */
static void set_mac_address(void)
{
	uint8_t addr[6];

	if (current_protocol == PROTOCOL_APPLE_FINDMY) {
		/* For Apple: derive MAC from first 6 bytes of public key */
		/* Address bytes are reversed (Everytag/heystack style) */
		addr[5] = apple_keys[0][0] | 0xC0; /* MSB with static random bits */
		addr[4] = apple_keys[0][1];
		addr[3] = apple_keys[0][2];
		addr[2] = apple_keys[0][3];
		addr[1] = apple_keys[0][4];
		addr[0] = apple_keys[0][5]; /* LSB */
	} else {
		/* For Google: use non-resolvable private address (NRPA) */
		addr[5] = google_key[0] & 0x3F; /* MSB with NRPA bits cleared */
		addr[4] = google_key[1];
		addr[3] = google_key[2];
		addr[2] = google_key[3];
		addr[1] = google_key[4];
		addr[0] = google_key[5]; /* LSB */
	}

	/* Set the public address using controller API (must be called before bt_enable) */
	bt_ctlr_set_public_addr(addr);
}

/* Start advertising for the current protocol */
static int start_advertising(void)
{
	struct bt_le_adv_param adv_param = {
		.id = 0,
		.options = BT_LE_ADV_OPT_USE_IDENTITY,
		.interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
		.interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
	};

	int err;

	/* Stop any existing advertising */
	bt_le_adv_stop();

	if (current_protocol == PROTOCOL_APPLE_FINDMY) {
		prepare_apple_findmy_adv();
		err = bt_le_adv_start(&adv_param, apple_ad, ARRAY_SIZE(apple_ad), NULL, 0);
	} else {
		prepare_google_fmdn_adv();
		err = bt_le_adv_start(&adv_param, google_ad, ARRAY_SIZE(google_ad), NULL, 0);
	}

	return err;
}

/* Protocol switcher timer callback */
static void protocol_switcher(struct k_timer *timer)
{
	/* Switch protocol */
	if (current_protocol == PROTOCOL_APPLE_FINDMY) {
		current_protocol = PROTOCOL_GOOGLE_FMDN;
	} else {
		current_protocol = PROTOCOL_APPLE_FINDMY;
	}

	/* Update MAC and restart advertising */
	set_mac_address();
	start_advertising();
}

K_TIMER_DEFINE(protocol_timer, protocol_switcher, NULL);

static void bt_ready(int err)
{
	if (err) {
		return;
	}

	/* Start advertising with current protocol */
	err = start_advertising();
	if (err) {
		return;
	}

	/* Start protocol switcher timer (currently disabled) */
	// k_timer_start(&protocol_timer, K_SECONDS(PROTOCOL_SWITCH_INTERVAL_SEC),
	//	      K_SECONDS(PROTOCOL_SWITCH_INTERVAL_SEC));
}

int main(void)
{
	/* Set MAC address BEFORE enabling Bluetooth */
	set_mac_address();

	/* Initialize the Bluetooth Subsystem */
	int err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
	}
	return 0;
}
