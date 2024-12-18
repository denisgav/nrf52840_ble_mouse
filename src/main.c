/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <soc.h>
#include <assert.h>

#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/bluetooth/services/bas.h>
#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/services/dis.h>
#include <dk_buttons_and_leds.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define BASE_USB_HID_SPEC_VERSION   0x0101

/* Number of pixels by which the cursor is moved when a button is pushed. */
#define MOVEMENT_SPEED              5
/* Number of input reports in this application. */
#define INPUT_REPORT_COUNT          3
/* Length of Mouse Input Report containing button data. */
#define INPUT_REP_BUTTONS_LEN       3
/* Length of Mouse Input Report containing movement data. */
#define INPUT_REP_MOVEMENT_LEN      3
/* Length of Mouse Input Report containing media player data. */
#define INPUT_REP_MEDIA_PLAYER_LEN  1
/* Index of Mouse Input Report containing button data. */
#define INPUT_REP_BUTTONS_INDEX     0
/* Index of Mouse Input Report containing movement data. */
#define INPUT_REP_MOVEMENT_INDEX    1
/* Index of Mouse Input Report containing media player data. */
#define INPUT_REP_MPLAYER_INDEX     2
/* Id of reference to Mouse Input Report containing button data. */
#define INPUT_REP_REF_BUTTONS_ID    1
/* Id of reference to Mouse Input Report containing movement data. */
#define INPUT_REP_REF_MOVEMENT_ID   2
/* Id of reference to Mouse Input Report containing media player data. */
#define INPUT_REP_REF_MPLAYER_ID    3

/* HIDs queue size. */
#define HIDS_QUEUE_SIZE 16

/* Key used to move cursor left */
#define KEY_LEFT_MASK   DK_BTN1_MSK
/* Key used to move cursor up */
#define KEY_UP_MASK     DK_BTN2_MSK
/* Key used to move cursor right */
#define KEY_RIGHT_MASK  DK_BTN3_MSK
/* Key used to move cursor down */
#define KEY_DOWN_MASK   DK_BTN4_MSK

/* Key used to accept or reject passkey value */
#define KEY_PAIRING_ACCEPT DK_BTN1_MSK
#define KEY_PAIRING_REJECT DK_BTN2_MSK

/* HIDS instance. */
BT_HIDS_DEF(hids_obj,
	    INPUT_REP_BUTTONS_LEN,
	    INPUT_REP_MOVEMENT_LEN,
	    INPUT_REP_MEDIA_PLAYER_LEN);

struct mouse_pos {
	int16_t x_val;
	int16_t y_val;
};

struct mouse_buttons_scroll_pan {
	uint8_t buttons;
	int8_t scroll;
	int8_t pan;
};

struct mouse_hid_report{
	struct mouse_pos pos;
	struct mouse_buttons_scroll_pan buttons_scroll_pan;
};

struct mouse_player_buttons {
	uint8_t buttons;
};

static struct k_work hids_work;

/* Mouse movement, buttons+scroll pan queue. */
K_MSGQ_DEFINE(hids_mouse_hid_queue,
	      sizeof(struct mouse_hid_report),
	      HIDS_QUEUE_SIZE,
	      4);

/* Mouse buttons player buttons queue. */
K_MSGQ_DEFINE(hids_mouse_player_buttons_queue,
	      sizeof(struct mouse_player_buttons),
	      HIDS_QUEUE_SIZE,
	      4);

#if CONFIG_BT_DIRECTED_ADVERTISING
/* Bonded address queue. */
K_MSGQ_DEFINE(bonds_queue,
	      sizeof(bt_addr_le_t),
	      CONFIG_BT_MAX_PAIRED,
	      4);
#endif

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
					  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static struct conn_mode {
	struct bt_conn *conn;
	bool in_boot_mode;
} conn_mode[CONFIG_BT_HIDS_MAX_CLIENT_COUNT];

static volatile bool is_adv_running;

static struct k_work adv_work;

static struct k_work pairing_work;
struct pairing_data_mitm {
	struct bt_conn *conn;
	unsigned int passkey;
};

K_MSGQ_DEFINE(mitm_queue,
	      sizeof(struct pairing_data_mitm),
	      CONFIG_BT_HIDS_MAX_CLIENT_COUNT,
	      4);


// ADC settings
#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || \
	!DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

#define DT_SPEC_AND_COMMA(node_id, prop, idx) \
	ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

/* Data of ADC io-channels specified in devicetree. */
static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels,
			     DT_SPEC_AND_COMMA)
};

#if CONFIG_BT_DIRECTED_ADVERTISING
static void bond_find(const struct bt_bond_info *info, void *user_data)
{
	int err;

	/* Filter already connected peers. */
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn) {
			const bt_addr_le_t *dst =
				bt_conn_get_dst(conn_mode[i].conn);

			if (!bt_addr_le_cmp(&info->addr, dst)) {
				return;
			}
		}
	}

	err = k_msgq_put(&bonds_queue, (void *) &info->addr, K_NO_WAIT);
	if (err) {
		printk("No space in the queue for the bond.\n");
	}
}
#endif

static void advertising_continue(void)
{
	struct bt_le_adv_param adv_param;

#if CONFIG_BT_DIRECTED_ADVERTISING
	bt_addr_le_t addr;

	if (!k_msgq_get(&bonds_queue, &addr, K_NO_WAIT)) {
		char addr_buf[BT_ADDR_LE_STR_LEN];
		int err;

		if (is_adv_running) {
			err = bt_le_adv_stop();
			if (err) {
				printk("Advertising failed to stop (err %d)\n", err);
				return;
			}
			is_adv_running = false;
		}

		adv_param = *BT_LE_ADV_CONN_DIR(&addr);
		adv_param.options |= BT_LE_ADV_OPT_DIR_ADDR_RPA;

		err = bt_le_adv_start(&adv_param, NULL, 0, NULL, 0);

		if (err) {
			printk("Directed advertising failed to start (err %d)\n", err);
			return;
		}

		bt_addr_le_to_str(&addr, addr_buf, BT_ADDR_LE_STR_LEN);
		printk("Direct advertising to %s started\n", addr_buf);
	} else
#endif
	{
		int err;

		if (is_adv_running) {
			return;
		}

		adv_param = *BT_LE_ADV_CONN;
		adv_param.options |= BT_LE_ADV_OPT_ONE_TIME;
		err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
		if (err) {
			printk("Advertising failed to start (err %d)\n", err);
			return;
		}

		printk("Regular advertising started\n");
	}

	is_adv_running = true;
}

static void advertising_start(void)
{
#if CONFIG_BT_DIRECTED_ADVERTISING
	k_msgq_purge(&bonds_queue);
	bt_foreach_bond(BT_ID_DEFAULT, bond_find, NULL);
#endif

	k_work_submit(&adv_work);
}

static void advertising_process(struct k_work *work)
{
	advertising_continue();
}

static void pairing_process(struct k_work *work)
{
	int err;
	struct pairing_data_mitm pairing_data;

	char addr[BT_ADDR_LE_STR_LEN];

	err = k_msgq_peek(&mitm_queue, &pairing_data);
	if (err) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(pairing_data.conn),
			  addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, pairing_data.passkey);
	printk("Press Button 1 to confirm, Button 2 to reject.\n");
}


static void insert_conn_object(struct bt_conn *conn)
{
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (!conn_mode[i].conn) {
			conn_mode[i].conn = conn;
			conn_mode[i].in_boot_mode = false;

			return;
		}
	}

	printk("Connection object could not be inserted %p\n", conn);
}


static bool is_conn_slot_free(void)
{
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn) {
			return true;
		}
	}

	return false;
}


static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	is_adv_running = false;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		if (err == BT_HCI_ERR_ADV_TIMEOUT) {
			printk("Direct advertising to %s timed out\n", addr);
			k_work_submit(&adv_work);
		} else {
			printk("Failed to connect to %s (%u)\n", addr, err);
		}
		return;
	}

	printk("Connected %s\n", addr);

	err = bt_hids_connected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about connection\n");
		return;
	}

	insert_conn_object(conn);

	if (is_conn_slot_free()) {
		advertising_start();
	}
}


static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected from %s (reason %u)\n", addr, reason);

	err = bt_hids_disconnected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about disconnection\n");
	}

	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn == conn) {
			conn_mode[i].conn = NULL;
			break;
		}
	}

	advertising_start();
}


#ifdef CONFIG_BT_HIDS_SECURITY_ENABLED
static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err) {
		printk("Security changed: %s level %u\n", addr, level);
	} else {
		printk("Security failed: %s level %u err %d\n", addr, level,
			err);
	}
}
#endif


BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
#ifdef CONFIG_BT_HIDS_SECURITY_ENABLED
	.security_changed = security_changed,
#endif
};


static void hids_pm_evt_handler(enum bt_hids_pm_evt evt,
				struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	size_t i;

	for (i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn == conn) {
			break;
		}
	}

	if (i >= CONFIG_BT_HIDS_MAX_CLIENT_COUNT) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	switch (evt) {
	case BT_HIDS_PM_EVT_BOOT_MODE_ENTERED:
		printk("Boot mode entered %s\n", addr);
		conn_mode[i].in_boot_mode = true;
		break;

	case BT_HIDS_PM_EVT_REPORT_MODE_ENTERED:
		printk("Report mode entered %s\n", addr);
		conn_mode[i].in_boot_mode = false;
		break;

	default:
		break;
	}
}


static void hid_init(void)
{
	int err;
	struct bt_hids_init_param hids_init_param = { 0 };
	struct bt_hids_inp_rep *hids_inp_rep;
	static const uint8_t mouse_movement_mask[DIV_ROUND_UP(INPUT_REP_MOVEMENT_LEN, 8)] = {0};

	static const uint8_t report_map[] = {
		0x05, 0x01,     /* Usage Page (Generic Desktop) */
		0x09, 0x02,     /* Usage (Mouse) */

		0xA1, 0x01,     /* Collection (Application) */

		/* Report ID 1: Mouse buttons + scroll/pan */
		0x85, 0x01,       /* Report Id 1 */
		0x09, 0x01,       /* Usage (Pointer) */
		0xA1, 0x00,       /* Collection (Physical) */
		0x95, 0x05,       /* Report Count (3) */
		0x75, 0x01,       /* Report Size (1) */
		0x05, 0x09,       /* Usage Page (Buttons) */
		0x19, 0x01,       /* Usage Minimum (01) */
		0x29, 0x05,       /* Usage Maximum (05) */
		0x15, 0x00,       /* Logical Minimum (0) */
		0x25, 0x01,       /* Logical Maximum (1) */
		0x81, 0x02,       /* Input (Data, Variable, Absolute) */
		0x95, 0x01,       /* Report Count (1) */
		0x75, 0x03,       /* Report Size (3) */
		0x81, 0x01,       /* Input (Constant) for padding */
		0x75, 0x08,       /* Report Size (8) */
		0x95, 0x01,       /* Report Count (1) */
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x38,       /* Usage (Wheel) */
		0x15, 0x81,       /* Logical Minimum (-127) */
		0x25, 0x7F,       /* Logical Maximum (127) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0x05, 0x0C,       /* Usage Page (Consumer) */
		0x0A, 0x38, 0x02, /* Usage (AC Pan) */
		0x95, 0x01,       /* Report Count (1) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0xC0,             /* End Collection (Physical) */

		/* Report ID 2: Mouse motion */
		0x85, 0x02,       /* Report Id 2 */
		0x09, 0x01,       /* Usage (Pointer) */
		0xA1, 0x00,       /* Collection (Physical) */
		0x75, 0x0C,       /* Report Size (12) */
		0x95, 0x02,       /* Report Count (2) */
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x30,       /* Usage (X) */
		0x09, 0x31,       /* Usage (Y) */
		0x16, 0x01, 0xF8, /* Logical maximum (2047) */
		0x26, 0xFF, 0x07, /* Logical minimum (-2047) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0xC0,             /* End Collection (Physical) */
		0xC0,             /* End Collection (Application) */

		/* Report ID 3: Advanced buttons */
		0x05, 0x0C,       /* Usage Page (Consumer) */
		0x09, 0x01,       /* Usage (Consumer Control) */
		0xA1, 0x01,       /* Collection (Application) */
		0x85, 0x03,       /* Report Id (3) */
		0x15, 0x00,       /* Logical minimum (0) */
		0x25, 0x01,       /* Logical maximum (1) */
		0x75, 0x01,       /* Report Size (1) */
		0x95, 0x01,       /* Report Count (1) */

		0x09, 0xCD,       /* Usage (Play/Pause) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x0A, 0x83, 0x01, /* Usage (Consumer Control Configuration) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x09, 0xB5,       /* Usage (Scan Next Track) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x09, 0xB6,       /* Usage (Scan Previous Track) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */

		0x09, 0xEA,       /* Usage (Volume Down) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x09, 0xE9,       /* Usage (Volume Up) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x0A, 0x25, 0x02, /* Usage (AC Forward) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0x0A, 0x24, 0x02, /* Usage (AC Back) */
		0x81, 0x06,       /* Input (Data,Value,Relative,Bit Field) */
		0xC0              /* End Collection */
	};

	hids_init_param.rep_map.data = report_map;
	hids_init_param.rep_map.size = sizeof(report_map);

	hids_init_param.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	hids_init_param.info.b_country_code = 0x00;
	hids_init_param.info.flags = (BT_HIDS_REMOTE_WAKE |
				      BT_HIDS_NORMALLY_CONNECTABLE);

	hids_inp_rep = &hids_init_param.inp_rep_group_init.reports[0];
	hids_inp_rep->size = INPUT_REP_BUTTONS_LEN;
	hids_inp_rep->id = INPUT_REP_REF_BUTTONS_ID;
	hids_init_param.inp_rep_group_init.cnt++;

	hids_inp_rep++;
	hids_inp_rep->size = INPUT_REP_MOVEMENT_LEN;
	hids_inp_rep->id = INPUT_REP_REF_MOVEMENT_ID;
	hids_inp_rep->rep_mask = mouse_movement_mask;
	hids_init_param.inp_rep_group_init.cnt++;

	hids_inp_rep++;
	hids_inp_rep->size = INPUT_REP_MEDIA_PLAYER_LEN;
	hids_inp_rep->id = INPUT_REP_REF_MPLAYER_ID;
	hids_init_param.inp_rep_group_init.cnt++;

	hids_init_param.is_mouse = true;
	hids_init_param.pm_evt_handler = hids_pm_evt_handler;

	err = bt_hids_init(&hids_obj, &hids_init_param);
	__ASSERT(err == 0, "HIDS initialization failed\n");
}


static void mouse_movement_buttons_scroll_pan_send(int16_t x_delta, int16_t y_delta, uint8_t buttons, int8_t scroll, int8_t pan)
{
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {

		if (!conn_mode[i].conn) {
			continue;
		}

		if (conn_mode[i].in_boot_mode) {
			x_delta = MAX(MIN(x_delta, SCHAR_MAX), SCHAR_MIN);
			y_delta = MAX(MIN(y_delta, SCHAR_MAX), SCHAR_MIN);

			bt_hids_boot_mouse_inp_rep_send(&hids_obj,
							     conn_mode[i].conn,
							     &buttons,
							     (int8_t) x_delta,
							     (int8_t) y_delta,
							     NULL);
		} else {
			uint8_t x_buff[2];
			uint8_t y_buff[2];
			uint8_t buffer_mouse_pos[INPUT_REP_MOVEMENT_LEN];
			uint8_t buffer_buttons_scroll_pan[INPUT_REP_BUTTONS_LEN];

			int16_t x = MAX(MIN(x_delta, 0x07ff), -0x07ff);
			int16_t y = MAX(MIN(y_delta, 0x07ff), -0x07ff);

			/* Convert to little-endian. */
			sys_put_le16(x, x_buff);
			sys_put_le16(y, y_buff);

			/* Encode report. */
			BUILD_ASSERT(sizeof(buffer_mouse_pos) == 3,
					 "Only 2 axis, 12-bit each, are supported");

			buffer_mouse_pos[0] = x_buff[0];
			buffer_mouse_pos[1] = (y_buff[0] << 4) | (x_buff[1] & 0x0f);
			buffer_mouse_pos[2] = (y_buff[1] << 4) | (y_buff[0] >> 4);


			bt_hids_inp_rep_send(&hids_obj, conn_mode[i].conn,
						  INPUT_REP_MOVEMENT_INDEX,
						  buffer_mouse_pos, sizeof(buffer_mouse_pos), NULL);

			buffer_buttons_scroll_pan[0] = buttons;
			buffer_buttons_scroll_pan[1] = scroll;
			buffer_buttons_scroll_pan[2] = pan;

			bt_hids_inp_rep_send(&hids_obj, conn_mode[i].conn,
						  INPUT_REP_BUTTONS_INDEX,
						  buffer_buttons_scroll_pan, sizeof(buffer_buttons_scroll_pan), NULL);
		}
	}
}

static void mouse_hid_handler(struct k_work *work)
{
	struct mouse_hid_report hid_report;

	while (!k_msgq_get(&hids_mouse_hid_queue, &hid_report, K_NO_WAIT)) {
		mouse_movement_buttons_scroll_pan_send(hid_report.pos.x_val, hid_report.pos.y_val, hid_report.buttons_scroll_pan.buttons, hid_report.buttons_scroll_pan.scroll, hid_report.buttons_scroll_pan.pan);
	}
}


#if defined(CONFIG_BT_HIDS_SECURITY_ENABLED)
static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Passkey for %s: %06u\n", addr, passkey);
}


static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
	int err;

	struct pairing_data_mitm pairing_data;

	pairing_data.conn    = bt_conn_ref(conn);
	pairing_data.passkey = passkey;

	err = k_msgq_put(&mitm_queue, &pairing_data, K_NO_WAIT);
	if (err) {
		printk("Pairing queue is full. Purge previous data.\n");
	}

	/* In the case of multiple pairing requests, trigger
	 * pairing confirmation which needed user interaction only
	 * once to avoid display information about all devices at
	 * the same time. Passkey confirmation for next devices will
	 * be proccess from queue after handling the earlier ones.
	 */
	if (k_msgq_num_used_get(&mitm_queue) == 1) {
		k_work_submit(&pairing_work);
	}
}


static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing cancelled: %s\n", addr);
}


static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}


static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];
	struct pairing_data_mitm pairing_data;

	if (k_msgq_peek(&mitm_queue, &pairing_data) != 0) {
		return;
	}

	if (pairing_data.conn == conn) {
		bt_conn_unref(pairing_data.conn);
		k_msgq_get(&mitm_queue, &pairing_data, K_NO_WAIT);
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing failed conn: %s, reason %d\n", addr, reason);
}


static struct bt_conn_auth_cb conn_auth_callbacks = {
	.passkey_display = auth_passkey_display,
	.passkey_confirm = auth_passkey_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed
};
#else
static struct bt_conn_auth_cb conn_auth_callbacks;
static struct bt_conn_auth_info_cb conn_auth_info_callbacks;
#endif /* defined(CONFIG_BT_HIDS_SECURITY_ENABLED) */


static void num_comp_reply(bool accept)
{
	struct pairing_data_mitm pairing_data;
	struct bt_conn *conn;

	if (k_msgq_get(&mitm_queue, &pairing_data, K_NO_WAIT) != 0) {
		return;
	}

	conn = pairing_data.conn;

	if (accept) {
		bt_conn_auth_passkey_confirm(conn);
		printk("Numeric Match, conn %p\n", conn);
	} else {
		bt_conn_auth_cancel(conn);
		printk("Numeric Reject, conn %p\n", conn);
	}

	bt_conn_unref(pairing_data.conn);

	if (k_msgq_num_used_get(&mitm_queue)) {
		k_work_submit(&pairing_work);
	}
}


void button_changed(uint32_t button_state, uint32_t has_changed)
{
	bool data_to_send = false;
	struct mouse_pos pos;
	uint32_t buttons = button_state & has_changed;

	memset(&pos, 0, sizeof(struct mouse_pos));

	if (IS_ENABLED(CONFIG_BT_HIDS_SECURITY_ENABLED)) {
		if (k_msgq_num_used_get(&mitm_queue)) {
			if (buttons & KEY_PAIRING_ACCEPT) {
				num_comp_reply(true);

				return;
			}

			if (buttons & KEY_PAIRING_REJECT) {
				num_comp_reply(false);

				return;
			}
		}
	}

	// if (buttons & KEY_LEFT_MASK) {
	// 	pos.x_val -= MOVEMENT_SPEED;
	// 	printk("%s(): left\n", __func__);
	// 	data_to_send = true;
	// }
	// if (buttons & KEY_UP_MASK) {
	// 	pos.y_val -= MOVEMENT_SPEED;
	// 	printk("%s(): up\n", __func__);
	// 	data_to_send = true;
	// }
	// if (buttons & KEY_RIGHT_MASK) {
	// 	pos.x_val += MOVEMENT_SPEED;
	// 	printk("%s(): right\n", __func__);
	// 	data_to_send = true;
	// }
	// if (buttons & KEY_DOWN_MASK) {
	// 	pos.y_val += MOVEMENT_SPEED;
	// 	printk("%s(): down\n", __func__);
	// 	data_to_send = true;
	// }

	// if (data_to_send) {
	// 	int err;

	// 	err = k_msgq_put(&hids_mouse_pos_queue, &pos, K_NO_WAIT);
	// 	if (err) {
	// 		printk("No space in the queue for button pressed\n");
	// 		return;
	// 	}
	// 	if (k_msgq_num_used_get(&hids_mouse_pos_queue) == 1) {
	// 		k_work_submit(&hids_work);
	// 	}
	// }
}


void configure_buttons(void)
{
	int err;

	err = dk_buttons_init(button_changed);
	if (err) {
		printk("Cannot init buttons (err: %d)\n", err);
	}
}


static void bas_notify(void)
{
	static uint32_t last_bas_notify_time = 0;
	static const uint32_t BAS_NOTIFY_PERIOD_MS = 1000; // Notify each second
	uint32_t cur_time = k_uptime_get();

	if(cur_time < (last_bas_notify_time + BAS_NOTIFY_PERIOD_MS)){
		return;
	}

	last_bas_notify_time = cur_time;

	uint8_t battery_level = bt_bas_get_battery_level();

	// battery_level--;

	// if (!battery_level) {
	// 	battery_level = 100U;
	// }

	battery_level = 100U;

	bt_bas_set_battery_level(battery_level);
}

static int configure_adc(void){
	int err;

	/* Configure channels individually prior to sampling. */
	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
		if (!adc_is_ready_dt(&adc_channels[i])) {
			printk("ADC controller device %s not ready\n", adc_channels[i].dev->name);
			return -1;
		}

		err = adc_channel_setup_dt(&adc_channels[i]);
		if (err < 0) {
			printk("Could not setup channel #%d (%d)\n", i, err);
			return -1;
		}
	}

	return 0;
}

static bool has_active_connection(){
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn) {
			return true;
		}
	}
	return false;
}

static int16_t convert_analog_joystick_to_mouse_pos(int32_t val_mv){
	int32_t val_mv_tmp = val_mv - 2048;

	val_mv_tmp = (val_mv_tmp >= 0x8000) ? -(0xFFFF - val_mv_tmp + 1) : val_mv_tmp;

	val_mv_tmp = val_mv - 2048;
	if(val_mv_tmp <= 0){
		if(val_mv_tmp <= -1024)
			return -4;
		if(val_mv_tmp <= -512)
			return -2;
	} else {
		if(val_mv_tmp >= 1024)
			return 4;
		if(val_mv_tmp >= 512)
			return 2;
	}
	
	return 0;
}

static void read_joystick(void){
	static uint32_t last_joystick_read_time = 0;
	static const uint32_t JOYSTICK_READ_PERIOD_MS = 50; 
	uint32_t cur_time = k_uptime_get();

	if(cur_time < (last_joystick_read_time + JOYSTICK_READ_PERIOD_MS)){
		return;
	}

	last_joystick_read_time = cur_time;


	//static uint32_t count = 0;
	static uint16_t buf;
	static struct adc_sequence sequence = {
		.buffer = &buf,
		/* buffer size in bytes, not number of samples */
		.buffer_size = sizeof(buf),
	};

	int err;

	struct mouse_hid_report hid_report;
	memset(&hid_report, 0, sizeof(struct mouse_hid_report));
	int32_t adc_val_mvX = 0;
	int32_t adc_val_mvY = 0;

	//printk("ADC reading[%u]:\n", count++);
	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
		int32_t val_mv;

		// printk("- %s, channel %d: ",
		// 		adc_channels[i].dev->name,
		// 		adc_channels[i].channel_id);

		(void)adc_sequence_init_dt(&adc_channels[i], &sequence);

		err = adc_read_dt(&adc_channels[i], &sequence);
		if (err < 0) {
			printk("Could not read (%d)\n", err);
			continue;
		}

		/*
			* If using differential mode, the 16 bit value
			* in the ADC sample buffer should be a signed 2's
			* complement value.
			*/
		if (adc_channels[i].channel_cfg.differential) {
			val_mv = (int32_t)((int16_t)buf);
		} else {
			val_mv = (int32_t)buf;
		}

		switch(i){
			case 0:{
				adc_val_mvY = val_mv;
				hid_report.pos.y_val = convert_analog_joystick_to_mouse_pos(adc_val_mvY);
				break;
			}
			case 1:{
				adc_val_mvX = val_mv;
				hid_report.pos.x_val = -convert_analog_joystick_to_mouse_pos(adc_val_mvX);
				break;
			}
			default:{ // Scroll
				break;
			}
		}
		
		// printk("%"PRId32, val_mv);
		// printk("\n");
		// err = adc_raw_to_millivolts_dt(&adc_channels[i],
		// 					&val_mv);
		// /* conversion to mV may not be supported, skip if not */
		// if (err < 0) {
		// 	printk(" (value in mV not available)\n");
		// } else {
		// 	printk(" = %"PRId32" mV\n", val_mv);
		// }
	}

	if((hid_report.pos.x_val != 0) || (hid_report.pos.y_val != 0)){
		printk("X: %"PRId32" Y: %"PRId32". ADC valX %"PRId32" valY %"PRId32"\n", hid_report.pos.x_val, hid_report.pos.y_val, adc_val_mvX, adc_val_mvY);
		err = k_msgq_put(&hids_mouse_hid_queue, &hid_report, K_FOREVER);
		if (err) {
			printk("No space in the queue for button pressed\n");
			return;
		}
		if (k_msgq_num_used_get(&hids_mouse_hid_queue) >= 1) {
			k_work_submit(&hids_work);
		}
	}
}


int main(void)
{
	int err;

	printk("Starting Bluetooth Peripheral HIDS mouse example\n");

	if (IS_ENABLED(CONFIG_BT_HIDS_SECURITY_ENABLED)) {
		err = bt_conn_auth_cb_register(&conn_auth_callbacks);
		if (err) {
			printk("Failed to register authorization callbacks.\n");
			return 0;
		}

		err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
		if (err) {
			printk("Failed to register authorization info callbacks.\n");
			return 0;
		}
	}

	/* DIS initialized at system boot with SYS_INIT macro. */
	hid_init();

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	k_work_init(&hids_work, mouse_hid_handler);
	k_work_init(&adv_work, advertising_process);
	if (IS_ENABLED(CONFIG_BT_HIDS_SECURITY_ENABLED)) {
		k_work_init(&pairing_work, pairing_process);
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	advertising_start();

	configure_buttons();

	configure_adc();

	while (1) {
		//k_sleep(K_SECONDS(1));
		k_msleep(1);

		if(has_active_connection()){
			/* Battery level simulation */
			bas_notify();

			/* Read joystick */
			read_joystick();
		}
	}

}
