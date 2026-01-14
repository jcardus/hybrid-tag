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
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include "keys.h"

static bool device_configured = false;

/* Protocol selection */
typedef enum {
	PROTOCOL_APPLE_FINDMY,
	PROTOCOL_GOOGLE_FMDN,
} protocol_t;

/* Current active protocol */
static protocol_t current_protocol = PROTOCOL_GOOGLE_FMDN;

/* Protocol switch interval in seconds (1 minute per protocol) */
#define PROTOCOL_SWITCH_INTERVAL_SEC 60

#define BT_UUID_CUSTOM_SERVICE_VAL BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

static const struct bt_uuid_128 config_service_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);

static const struct bt_uuid_128 write_apple_key_cmd_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1));

static uint8_t apple_key[28];

static ssize_t write_apple_key(struct bt_conn *conn,
					 const struct bt_gatt_attr *attr,
					 const void *buf, uint16_t len, uint16_t offset,
					 uint8_t flags)
{
	printk("write_apple_key received %u bytes: ", len);
	for (uint16_t i = 0; i < len; i++) {
		printk("%02x ", ((uint8_t *)buf)[i]);
	}
	printk("\n");
	return len;
}

BT_GATT_SERVICE_DEFINE(config_svc,
	BT_GATT_PRIMARY_SERVICE(&config_service_uuid),
	BT_GATT_CHARACTERISTIC(&write_apple_key_cmd_uuid.uuid,
				   BT_GATT_CHRC_WRITE,
				   BT_GATT_PERM_WRITE, NULL,
				   write_apple_key, &apple_key),
);

static const struct bt_data config_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
			  BT_UUID_16_ENCODE(BT_UUID_HRS_VAL),
			  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL),
			  BT_UUID_16_ENCODE(BT_UUID_CTS_VAL)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_CUSTOM_SERVICE_VAL),
};

static const struct bt_data config_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
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

/* Work item for protocol switching (runs in thread context) */
static void protocol_switch_work_handler(struct k_work *work)
{
	/* Switch protocol */
	if (current_protocol == PROTOCOL_APPLE_FINDMY) {
		current_protocol = PROTOCOL_GOOGLE_FMDN;
		printk("Switching to Google FMDN\n");
	} else {
		current_protocol = PROTOCOL_APPLE_FINDMY;
		printk("Switching to Apple FindMy\n");
	}

	/* Restart advertising with a new protocol payload */
	const int err = start_advertising();
	if (err) {
		printk("Failed to restart advertising (err %d)\n", err);
	}
}

K_WORK_DEFINE(protocol_switch_work, protocol_switch_work_handler);

/* Protocol switcher timer callback (runs in ISR context) */
static void protocol_switcher(struct k_timer *timer)
{
	/* Submit work to the system work queue (will run in thread context) */
	k_work_submit(&protocol_switch_work);
}

K_TIMER_DEFINE(protocol_timer, protocol_switcher, NULL);

/* Start advertising as an unconfigured device */
static void start_config_advertising(void)
{
	const int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, config_ad, ARRAY_SIZE(config_ad), config_sd, ARRAY_SIZE(config_sd));
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}
	printk("Configuration mode active. Waiting for BLE configuration...\n");
}

/* Connection callbacks for configuration mode */
static void config_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err %d)\n", err);
		return;
	}
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Connected %s\n", addr);
}

static void config_disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason %d)\n", reason);
}

BT_CONN_CB_DEFINE(config_conn_callbacks) = {
	.connected = config_connected,
	.disconnected = config_disconnected
};

/* Wait for configuration over BLE */
static void wait_for_configuration(void)
{
	printk("\n");
	printk("========================================\n");
	printk("  HYBRID TAG - FIRST RUN SETUP\n");
	printk("========================================\n");
	printk("\n");
	printk("Device not configured. Entering configuration mode...\n");
	/* Start advertising with a configuration service */
	start_config_advertising();
}

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth ready failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	/* Check if a device is configured */
	if (!device_configured) {
		/* Enter configuration mode - waits until configured */
		wait_for_configuration();
		/* After configuration, device needs reboot to start normal operation */
		return;
	}

	/* Device is configured, start normal operation */
	printk("Device already configured\n");

	/* Set MAC address BEFORE starting advertising */
	set_mac_address();

	printk("Starting with %s\n",
		current_protocol == PROTOCOL_APPLE_FINDMY ? "Apple FindMy" : "Google FMDN");

	err = start_advertising();
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}

	/* Start the protocol switcher timer */
	k_timer_start(&protocol_timer, K_SECONDS(PROTOCOL_SWITCH_INTERVAL_SEC),K_SECONDS(PROTOCOL_SWITCH_INTERVAL_SEC));
	printk("Protocol switcher timer started (interval: %d seconds)\n", PROTOCOL_SWITCH_INTERVAL_SEC);
}

int main(void)
{
	printk("Hybrid Tag starting...\n");

	/* Initialize Bluetooth - bt_ready callback will handle config check */
	const int err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return err;
	}

	return 0;
}
