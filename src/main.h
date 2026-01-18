#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

/* Apple FindMy uses 28-byte public keys (P-224 curve) */
#define APPLE_KEY_SIZE 28

/* Google FMDN can use 20-byte (160-bit) or 32-byte (256-bit) keys */
#define GOOGLE_KEY_SIZE 20

#define PROTOCOL_SWITCH_INTERVAL_SEC 60

#define BT_UUID_CUSTOM_SERVICE_VAL BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
static const struct bt_uuid_128 config_service_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);

static const struct bt_uuid_128 write_apple_key_cmd_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1));

static const struct bt_uuid_128 write_google_key_cmd_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2));

#define APPLE_FINDMY_PAYLOAD_SIZE 29

/* Protocol selection */
typedef enum {
    PROTOCOL_APPLE_FINDMY,
    PROTOCOL_GOOGLE_FMDN,
} protocol_t;

static void protocol_switcher(struct k_timer *timer);

K_TIMER_DEFINE(protocol_timer, protocol_switcher, NULL);

static void set_mac_address(void);
static int start_advertising(void);
static void start_scan(void);
#endif /* MAIN_H */
