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
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/reboot.h>

#include "keys.h"

/* LED for status indication */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Runtime key storage (loaded from NVS) */
static uint8_t runtime_apple_key[APPLE_KEY_SIZE];
static uint8_t runtime_google_key[GOOGLE_KEY_SIZE];
static bool keys_loaded = false;
static bool settings_loading = false;
static bool keys_provisioned = false;

/* Key update triggers a reboot to apply the derived MAC address */
static struct k_work_delayable key_update_reboot_work;
static void schedule_key_update_reboot(void);
static void reset_provisioning_state(void);

static void bt_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("BLE connect failed (err 0x%02X)\n", err);
		return;
	}

	printk("BLE connected\n");
}

static void bt_disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("BLE disconnected (reason 0x%02X)\n", reason);
	reset_provisioning_state();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = bt_connected,
	.disconnected = bt_disconnected,
};

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
 * - Payload structure (following Everytag format):
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

static const char device_name[] = "HYBRID-TAG";

/* Provisioning GATT service UUIDs */
#define BT_UUID_HT_PROV_SERVICE \
	BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x8c5debdb, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77))
#define BT_UUID_HT_PROV_AUTH \
	BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x8c5debdf, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77))
#define BT_UUID_HT_PROV_KEY \
	BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x8c5debde, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77))

static const struct bt_data provisioning_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, device_name, sizeof(device_name) - 1),
};

static const struct bt_data provisioning_scan_rsp[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_128_ENCODE(0x8c5debdb, 0xad8d,
							      0x4810, 0xa31f,
							      0x53862e79ee77)),
};

static const uint8_t provision_auth_code[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
static bool provisioning_allowed = false;
static uint8_t provision_key_buf[APPLE_KEY_SIZE];
static uint8_t provision_key_chunks = 0;

static ssize_t write_provision_auth(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				    const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len != sizeof(provision_auth_code)) {
		printk("Provision auth length invalid (%u)\n", (unsigned)len);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (memcmp(buf, provision_auth_code, sizeof(provision_auth_code)) != 0) {
		printk("Provision auth failed\n");
		reset_provisioning_state();
		return len;
	}

	printk("Provision auth ok\n");
	provisioning_allowed = true;
	provision_key_chunks = 0;
	return len;
}

static ssize_t write_provision_key(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (!provisioning_allowed || len != 14 || provision_key_chunks >= 2) {
		printk("Provision key write rejected (allowed=%d len=%u chunks=%u)\n",
		       provisioning_allowed, (unsigned)len, (unsigned)provision_key_chunks);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(&provision_key_buf[provision_key_chunks * 14], buf, 14);
	provision_key_chunks++;

	if (provision_key_chunks == 2) {
		int rc;

		memcpy(runtime_apple_key, provision_key_buf, APPLE_KEY_SIZE);
		rc = settings_save_one("keys/apple", runtime_apple_key, APPLE_KEY_SIZE);
		if (rc != 0) {
			printk("Provision key save failed (err %d)\n", rc);
			return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
		}

		keys_loaded = true;
		keys_provisioned = true;
		printk("Updated Apple key via provisioning GATT\n");
		reset_provisioning_state();
		schedule_key_update_reboot();
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(provision_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HT_PROV_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_HT_PROV_AUTH, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_provision_auth, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HT_PROV_KEY, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, write_provision_key, NULL),
);

static void reset_provisioning_state(void)
{
	provisioning_allowed = false;
	provision_key_chunks = 0;
}

/* Settings handlers for loading keys from NVS */
static int keys_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	int rc;

	if (settings_name_steq(name, "apple", &next) && !next) {
		if (len == APPLE_KEY_SIZE) {
			rc = read_cb(cb_arg, runtime_apple_key, sizeof(runtime_apple_key));
			if (rc >= 0) {
				keys_loaded = true;
				if (settings_loading) {
					printk("Loaded Apple key from NVS\n");
					keys_provisioned = true;
				} else {
					printk("Updated Apple key via MCUmgr\n");
				}
				if (!settings_loading) {
					rc = settings_save_one("keys/apple", runtime_apple_key,
							       APPLE_KEY_SIZE);
					if (rc != 0) {
						return rc;
					}
					keys_provisioned = true;
					schedule_key_update_reboot();
				}
				return 0;
			}
		}
		return -EINVAL;
	}

	if (settings_name_steq(name, "google", &next) && !next) {
		if (len == GOOGLE_KEY_SIZE) {
			rc = read_cb(cb_arg, runtime_google_key, sizeof(runtime_google_key));
			if (rc >= 0) {
				if (settings_loading) {
					printk("Loaded Google key from NVS\n");
					keys_provisioned = true;
				} else {
					printk("Updated Google key via MCUmgr\n");
				}
				if (!settings_loading) {
					rc = settings_save_one("keys/google", runtime_google_key,
							       GOOGLE_KEY_SIZE);
					if (rc != 0) {
						return rc;
					}
					keys_provisioned = true;
					schedule_key_update_reboot();
				}
				return 0;
			}
		}
		return -EINVAL;
	}

	return -ENOENT;
}

static struct settings_handler keys_settings = {
	.name = "keys",
	.h_set = keys_settings_set,
};

/* Load keys from NVS, or use defaults from keys.h if not found */
static void load_keys(void)
{
	int err;

	/* Register settings handler */
	err = settings_register(&keys_settings);
	if (err) {
		printk("Failed to register settings handler: %d\n", err);
	}

	/* Load settings from NVS */
	settings_loading = true;
	keys_provisioned = false;
	err = settings_load();
	settings_loading = false;
	if (err) {
		printk("Failed to load settings: %d\n", err);
	}

	/* If no keys in NVS, use defaults from keys.h */
	if (!keys_loaded) {
		printk("No keys in NVS, using defaults from keys.h\n");
		memcpy(runtime_apple_key, apple_keys[0], APPLE_KEY_SIZE);
		memcpy(runtime_google_key, google_key, GOOGLE_KEY_SIZE);
		keys_loaded = true;
	}
}

static void schedule_key_update_reboot(void)
{
	k_work_reschedule(&key_update_reboot_work, K_SECONDS(1));
}

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
	memcpy(&apple_findmy_payload[5], &runtime_apple_key[6], 22);

	/* First two bits from key[0] (top 2 bits of the byte used in MAC) */
	apple_findmy_payload[27] = (runtime_apple_key[0] >> 6) & 0x03;

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
	memcpy(&google_fmdn_payload[3], runtime_google_key, GOOGLE_KEY_SIZE);
}

/* Set BLE MAC address based on protocol (following Everytag implementation) */
static void set_mac_address(void)
{
	uint8_t addr[6];

	if (!keys_provisioned) {
		printk("Provisioning mode: using default controller address\n");
		return;
	}

	if (current_protocol == PROTOCOL_APPLE_FINDMY) {
		/* For Apple: derive MAC from first 6 bytes of public key */
		/* Address bytes are reversed (Everytag/heystack style) */
		addr[5] = runtime_apple_key[0] | 0xC0; /* MSB with static random bits */
		addr[4] = runtime_apple_key[1];
		addr[3] = runtime_apple_key[2];
		addr[2] = runtime_apple_key[3];
		addr[1] = runtime_apple_key[4];
		addr[0] = runtime_apple_key[5]; /* LSB */
	} else {
		/* For Google: use non-resolvable private address (NRPA) */
		addr[5] = runtime_google_key[0] & 0x3F; /* MSB with NRPA bits cleared */
		addr[4] = runtime_google_key[1];
		addr[3] = runtime_google_key[2];
		addr[2] = runtime_google_key[3];
		addr[1] = runtime_google_key[4];
		addr[0] = runtime_google_key[5]; /* LSB */
	}

	/* Set the public address using controller API (must be called before bt_enable) */
	bt_ctlr_set_public_addr(addr);
	printk("MAC set for %s: %02X:%02X:%02X:%02X:%02X:%02X\n",
	       current_protocol == PROTOCOL_APPLE_FINDMY ? "Apple" : "Google",
	       addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

/* Start advertising for current protocol */
static int start_advertising(void)
{
	struct bt_le_adv_param adv_param = {
		.id = 0,
		.options = BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_CONN,
		.interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
		.interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
	};

	int err;

	/* Stop any existing advertising */
	bt_le_adv_stop();

	if (!keys_provisioned) {
		printk("Starting MCUmgr provisioning advertising\n");
		err = bt_le_adv_start(&adv_param, provisioning_ad, ARRAY_SIZE(provisioning_ad),
				      provisioning_scan_rsp, ARRAY_SIZE(provisioning_scan_rsp));
	} else if (current_protocol == PROTOCOL_APPLE_FINDMY) {
		printk("Starting Apple advertising (payload %u bytes)\n",
		       APPLE_FINDMY_PAYLOAD_SIZE);
		prepare_apple_findmy_adv();
		err = bt_le_adv_start(&adv_param, apple_ad, ARRAY_SIZE(apple_ad),
				      NULL, 0);
	} else {
		printk("Starting Google advertising (payload %u bytes)\n",
		       GOOGLE_FMDN_PAYLOAD_SIZE);
		prepare_google_fmdn_adv();
		err = bt_le_adv_start(&adv_param, google_ad, ARRAY_SIZE(google_ad),
				      NULL, 0);
	}

	if (err) {
		printk("Advertising start failed (err %d)\n", err);
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

/* LED blink timer callback - indicates active protocol
 * Apple FindMy: 1 short blink every 2 seconds
 * Google FMDN: 2 short blinks every 2 seconds
 */
static void led_blink_handler(struct k_timer *timer)
{
	static uint8_t blink_state = 0;

	if (current_protocol == PROTOCOL_APPLE_FINDMY) {
		/* Single blink pattern for Apple FindMy: ON at 0, OFF at 1-9 */
		if (blink_state == 0) {
			gpio_pin_set_dt(&led, 1); /* On */
		} else {
			gpio_pin_set_dt(&led, 0); /* Off */
		}
		blink_state = (blink_state + 1) % 10;
	} else {
		/* Double blink pattern for Google FMDN: ON at 0,2 OFF at 1,3-9 */
		switch (blink_state) {
		case 0:
		case 2:
			gpio_pin_set_dt(&led, 1); /* On */
			break;
		default:
			gpio_pin_set_dt(&led, 0); /* Off */
			break;
		}
		blink_state = (blink_state + 1) % 10;
	}
}

K_TIMER_DEFINE(led_blink_timer, led_blink_handler, NULL);

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		/* BT init error blink: every 2 seconds */
		while (1) {
			gpio_pin_toggle_dt(&led);
			k_msleep(2000);
		}
		return;
	}

	{
		bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
		size_t count = ARRAY_SIZE(addrs);
		char addr_str[BT_ADDR_LE_STR_LEN];

		bt_id_get(addrs, &count);
		if (count > 0) {
			bt_addr_le_to_str(&addrs[0], addr_str, sizeof(addr_str));
		printk("BLE identity: %s\n", addr_str);
		}
		printk("BLE name: %s\n", bt_get_name());
		{
			char uuid_str[BT_UUID_STR_LEN];

			bt_uuid_to_str(BT_UUID_HT_PROV_SERVICE, uuid_str, sizeof(uuid_str));
			printk("Provisioning service UUID: %s\n", uuid_str);
		}
	}

	printk("Bluetooth initialized\n");
	/* Start advertising with current protocol */
	err = start_advertising();
	if (err) {
		printk("Advertising error (err %d)\n", err);
		/* Advertising error blink: every 4 seconds */
		while (1) {
			printk("Advertising error (err %d)\n", err);
			gpio_pin_toggle_dt(&led);
			k_msleep(4000);
		}
		return;
	}

	/* Start LED blink timer - 200ms interval for blink patterns */
	k_timer_start(&led_blink_timer, K_MSEC(200), K_MSEC(200));

	/* Start protocol switcher timer (currently disabled) */
	// k_timer_start(&protocol_timer, K_SECONDS(PROTOCOL_SWITCH_INTERVAL_SEC),
	//	      K_SECONDS(PROTOCOL_SWITCH_INTERVAL_SEC));
}

static void key_update_reboot_handler(struct k_work *work)
{
	printk("Key updated over BLE, rebooting to apply MAC address\n");
	sys_reboot(SYS_REBOOT_COLD);
}

int main(void)
{
	k_work_init_delayable(&key_update_reboot_work, key_update_reboot_handler);

	/* Initialize LED */
	if (!gpio_is_ready_dt(&led)) {
		/* LED init failed - blink rapidly forever */
		while (1) {
			k_msleep(100);
		}
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

	/* Flash LED 3 times on startup */
	for (int i = 0; i < 10; i++) {
		gpio_pin_set_dt(&led, 1);
		k_msleep(100);
		gpio_pin_set_dt(&led, 0);
		k_msleep(100);
	}

	/* Initialize settings subsystem */
	int err = settings_subsys_init();
	if (err) {
		printk("Settings init failed (err %d)\n", err);
	}

	/* Load keys from NVS (or use defaults) */
	load_keys();

	/* Set MAC address BEFORE enabling Bluetooth */
	set_mac_address();

	/* Initialize the Bluetooth Subsystem */
	err = bt_enable(bt_ready);
	if (err) {
		printk("bt_enable failed (err %d)\n", err);
	}

	/* Main loop - just sleep forever, timers will handle everything */
	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
