/*
 * Copyright 2012 The Android Open Source Project
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/******************************************************************************
 *
 *  Filename:      bt_vendor_qcom.c
 *
 *  Description:   vendor specific library implementation
 *
 ******************************************************************************/

#define LOG_TAG "bt_vendor"

//#define LOG_NDEBUG 0

#include <utils/Log.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <termios.h>
#include "bt_vendor_qcom.h"
#include "hci_uart.h"
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <linux/un.h>
#ifdef BT_NV_SUPPORT
#include "bt_vendor_persist.h"
#endif
#include "hw_rome.h"

#define WAIT_TIMEOUT 200000
#define BLUETOOTH_MAC_ADDR_BOOT_PROPERTY "ro.boot.btmacaddr"
#define DEFAULT_BAUDRATE 2000000

/******************************************************************************
 **  Externs
 ******************************************************************************/
extern int rome_ver;

/******************************************************************************
 **  Variables
 ******************************************************************************/
bt_vendor_callbacks_t *bt_vendor_cbacks = NULL;
char vnd_local_bd_addr[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static int rfkill_id = -1;
static char *rfkill_state = NULL;

static const tUSERIAL_CFG userial_init_cfg =
{
	(USERIAL_DATABITS_8 | USERIAL_PARITY_NONE | USERIAL_STOPBITS_1),
	USERIAL_BAUD_115200
};

#if (HW_NEED_END_WITH_HCI_RESET == TRUE)
void hw_epilog_process(void);
#endif

#ifdef WIFI_BT_STATUS_SYNC
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include "cutils/properties.h"

static const char WIFI_PROP_NAME[]    = "wlan.driver.status";
static const char SERVICE_PROP_NAME[] = "bluetooth.hsic_ctrl";
static const char BT_STATUS_NAME[]    = "bluetooth.enabled";
static const char WIFI_SERVICE_PROP[] = "wlan.hsic_ctrl";

#define WIFI_BT_STATUS_LOCK    "/data/connectivity/wifi_bt_lock"
int isInit=0;
#endif /* WIFI_BT_STATUS_SYNC */

/******************************************************************************
 ** Extern functions
 ******************************************************************************/
int readTrpState();

/******************************************************************************
 **  Functions
 ******************************************************************************/
#ifdef WIFI_BT_STATUS_SYNC
static int bt_semaphore_create(void)
{
	int fd;

	fd = open(WIFI_BT_STATUS_LOCK, O_RDONLY);
	if (fd < 0)
		ALOGE("can't create file\n");

	return fd;
}

static int bt_semaphore_get(int fd)
{
	int ret;

	if (fd < 0)
		return -1;

	ret = flock(fd, LOCK_EX);
	if (ret != 0) {
		ALOGE("can't hold lock: %s\n", strerror(errno));
		return -1;
	}

	return ret;
}

static int bt_semaphore_release(int fd)
{
	int ret;

	if (fd < 0)
		return -1;

	ret = flock(fd, LOCK_UN);
	if (ret != 0) {
		ALOGE("can't release lock: %s\n", strerror(errno));
		return -1;
	}

	return ret;
}

static int bt_semaphore_destroy(int fd)
{
	if (fd < 0)
		return -1;

	return close (fd);
}

static int bt_wait_for_service_done(void)
{
	char service_status[PROPERTY_VALUE_MAX];
	int count = 30;

	ALOGE("%s: check\n", __func__);

	/* wait for service done */
	while (count-- > 0) {
		property_get(WIFI_SERVICE_PROP, service_status, NULL);

		if (strcmp(service_status, "") != 0) {
			usleep(200000);
		} else {
			break;
		}
	}

	return 0;
}

#endif /* WIFI_BT_STATUS_SYNC */

/** Bluetooth Controller power up or shutdown */
static int bt_powerup(int en )
{
	char rfkill_type[64];
	char type[16];
	int fd = -1, size, i, ret;

	char prop[PROPERTY_VALUE_MAX];
	char state;
	char on = (en)?'1':'0';

#ifdef WIFI_BT_STATUS_SYNC
	char wifi_status[PROPERTY_VALUE_MAX];
	int lock_fd;
#endif /*WIFI_BT_STATUS_SYNC*/

	ALOGI("bt_powerup: %c", on);

	/* Check if rfkill has been disabled */
	ret = property_get("ro.rfkilldisabled", prop, "0");
	if (!ret) {
		ALOGE("Couldn't get ro.rfkilldisabled (%d)", ret);
		return -1;
	}
	/* In case rfkill disabled, then no control power*/
	if (strcmp(prop, "1") == 0) {
		ALOGI("ro.rfkilldisabled : %s", prop);
		return -1;
	}

#ifdef WIFI_BT_STATUS_SYNC
	lock_fd = bt_semaphore_create();
	bt_semaphore_get(lock_fd);
	bt_wait_for_service_done();
#endif

	/* Checking if the rfkill node is in property */
	if (property_get("ro.bt.rfkill.state", prop, NULL)) {
		asprintf(&rfkill_state, "%s", prop);
		goto skip;
	}

	/* Assign rfkill_id and find bluetooth rfkill state path*/
	for (i=0; (rfkill_id == -1) && (rfkill_state == NULL); i++) {
		snprintf(rfkill_type, sizeof(rfkill_type), "/sys/class/rfkill/rfkill%d/type", i);
		if ((fd = open(rfkill_type, O_RDONLY)) < 0) {
			ALOGE("open(%s) failed: %s (%d)\n", rfkill_type, strerror(errno), errno);
#ifdef WIFI_BT_STATUS_SYNC
			bt_semaphore_release(lock_fd);
			bt_semaphore_destroy(lock_fd);
#endif
			return -1;
		}

		size = read(fd, &type, sizeof(type));
		close(fd);

		if ((size >= 9) && !memcmp(type, "bluetooth", 9)) {
			asprintf(&rfkill_state, "/sys/class/rfkill/rfkill%d/state", rfkill_id = i);
			break;
		}
	}
skip:
	/* Get rfkill State to control */
	if (rfkill_state != NULL) {
		if ((fd = open(rfkill_state, O_RDWR)) < 0) {
			ALOGE("open(%s) for write failed: %s (%d)",rfkill_state, strerror(errno), errno);
#ifdef WIFI_BT_STATUS_SYNC
			bt_semaphore_release(lock_fd);
			bt_semaphore_destroy(lock_fd);
#endif
			return -1;
		}
		ALOGV("using %s", rfkill_state);
	}

	/* Write value to control rfkill */
	if ((size = write(fd, &on, 1)) < 0) {
		ALOGE("write(%s) failed: %s (%d)",rfkill_state, strerror(errno),errno);
#ifdef WIFI_BT_STATUS_SYNC
		bt_semaphore_release(lock_fd);
		bt_semaphore_destroy(lock_fd);
#endif
		return -1;
	}
#ifdef WIFI_BT_STATUS_SYNC
	/* query wifi status */
	property_get(WIFI_PROP_NAME, wifi_status, "");

	ALOGE("bt get wifi status: %s, isInit: %d\n",  wifi_status, isInit);

	/* If wlan driver is not loaded, and bt is changed from off => on */
	if (strncmp(wifi_status, "unloaded", strlen("unloaded")) == 0 || strlen(wifi_status) == 0) {
		if (on == '1') {
			ALOGI("%s: BT_VND_PWR_ON\n", __func__);
			if (property_set(SERVICE_PROP_NAME, "load_wlan") < 0) {
				ALOGE("%s Property setting failed", SERVICE_PROP_NAME);
				close(fd);
				bt_semaphore_release(lock_fd);
				bt_semaphore_destroy(lock_fd);
				return -1;
			}
		} else if (isInit == 0 && on == '0') {
			ALOGI("%s: BT_VND_PWR_OFF\n", __func__);
			if (property_set(SERVICE_PROP_NAME, "unbind_hsic") < 0) {
				ALOGE("%s Property setting failed", SERVICE_PROP_NAME);
				close(fd);
				bt_semaphore_release(lock_fd);
				bt_semaphore_destroy(lock_fd);
				return -1;
			}
		}
	}

	if (isInit == 0 && on == '0')
		property_set(BT_STATUS_NAME, "false");
	else if (on == '1')
		property_set(BT_STATUS_NAME, "true");

	bt_semaphore_release(lock_fd);
	bt_semaphore_destroy(lock_fd);
#endif /* WIFI_BT_STATUS_SYNC */

done:
	if (fd >= 0)
		close(fd);

	return 0;
}

static bool validate_tok(char* bdaddr_tok) {
	int i = 0;
	bool ret;

	if (strlen(bdaddr_tok) != 2) {
		ret = FALSE;
		ALOGE("Invalid token length");
	} else {
		ret = TRUE;
		for (i=0; i<2; i++) {
			if ((bdaddr_tok[i] >= '0' && bdaddr_tok[i] <= '9') ||
			    (bdaddr_tok[i] >= 'A' && bdaddr_tok[i] <= 'F') ||
			    (bdaddr_tok[i] >= 'a' && bdaddr_tok[i] <= 'f')) {
				ret = TRUE;
				ALOGV("%s: tok %s @ %d is good", __func__, bdaddr_tok, i);
			} else {
				ret = FALSE;
				ALOGE("invalid character in tok: %s at ind: %d", bdaddr_tok, i);
				break;
			}
		}
	}
	return ret;
}

/*****************************************************************************
 **
 **   BLUETOOTH VENDOR INTERFACE LIBRARY FUNCTIONS
 **
 *****************************************************************************/

static int init(const bt_vendor_callbacks_t* p_cb, unsigned char *local_bdaddr)
{
	int i = 0;
	static char bd_addr[PROPERTY_VALUE_MAX];
	uint8_t local_bd_addr_from_prop[6];
	char* tok;

	ALOGI("bt-vendor : init");

	if (p_cb == NULL) {
		ALOGE("init failed with no user callbacks!");
		return -1;
	}

	ALOGI("bt-vendor : Initializing UART transport layer");
	userial_vendor_init();

	/* store reference to user callbacks */
	bt_vendor_cbacks = (bt_vendor_callbacks_t *) p_cb;

	/* Copy BD Address as little-endian byte order */
	if (local_bdaddr)
		for (i=0;i<6;i++)
			vnd_local_bd_addr[i] = *(local_bdaddr + (5-i));

	/* BD address from boot property can override local_bdaddr */
	if (property_get(BLUETOOTH_MAC_ADDR_BOOT_PROPERTY, bd_addr, NULL)) {
		ALOGV("BD address read from Boot property: %s\n", bd_addr);
		i = 0;
		tok =  strtok(bd_addr, ":");
		while (tok != NULL) {
			ALOGV("bd add [%d]: %lx ", i, strtol(tok, NULL, 16));
			if (i>=6) {
				ALOGE("bd property of invalid length");
				break;
			}
			if (!validate_tok(tok)) {
				ALOGE("Invalid token in BD address");
				break;
			}
			local_bd_addr_from_prop[i] = strtol(tok, NULL, 16);
			tok = strtok(NULL, ":");
			i++;
		}
		if (i == 6) {
			ALOGV("Valid BD address read from prop");
			memcpy(vnd_local_bd_addr, local_bd_addr_from_prop, sizeof(vnd_local_bd_addr));
		} else {
			ALOGV("There are not enough tokens in BD addr");
		}
	}

	ALOGI("%s: Local BD Address : %.2x:%.2x:%.2x:%.2x:%.2x:%.2x", __FUNCTION__,
	      vnd_local_bd_addr[0],
	      vnd_local_bd_addr[1],
	      vnd_local_bd_addr[2],
	      vnd_local_bd_addr[3],
	      vnd_local_bd_addr[4],
	      vnd_local_bd_addr[5]);

#ifdef WIFI_BT_STATUS_SYNC
	isInit = 1;
#endif /* WIFI_BT_STATUS_SYNC */

	return 0;
}

static bool is_soc_initialized() {
	bool init = false;
	char init_value[PROPERTY_VALUE_MAX];
	int ret;

	ALOGI("bt-vendor : is_soc_initialized");

	ret = property_get("bluetooth.soc_initialized", init_value, NULL);
	if (ret != 0) {
		ALOGI("bluetooth.soc_initialized set to %s\n", init_value);
		if (!strncasecmp(init_value, "1", sizeof("1"))) {
			init = true;
		}
	} else {
		ALOGE("%s: Failed to get bluetooth.soc_initialized", __FUNCTION__);
	}

	return init;
}


/** Requested operations */
static int op(bt_vendor_opcode_t opcode, void *param)
{
	int retval = 0;
	int nCnt = 0;
	int nState = -1;
	bool is_ant_req = false;
	char wipower_status[PROPERTY_VALUE_MAX];
	char value[PROPERTY_VALUE_MAX] = {'\0'};
	int (*fd_array)[] = (int (*)[]) param;
	int idx, fd;
	uint8_t *state;
	uint8_t wake_assert;

	ALOGV("bt-vendor : op for %d", opcode);

	switch(opcode) {
		case BT_VND_OP_POWER_CTRL:
			nState = *(int *) param;
			ALOGI("bt-vendor : BT_VND_OP_POWER_CTRL: %s",
			      (nState == BT_VND_PWR_ON)? "On" : "Off" );
			/* BT Chipset Power Control through Device Tree Node */
			property_get("bluetooth.vnd_power", value, "0");
			if ((nState == BT_VND_PWR_ON) && (strcmp(value, "1") == 0))
				bt_powerup(BT_VND_PWR_OFF);
			retval = bt_powerup(nState);
			if (retval == 0)
				property_set("bluetooth.vnd_power", nState == BT_VND_PWR_ON ? "1" : "0");
			break;

		case BT_VND_OP_FW_CFG:
			// call hciattach to initalize the stack
			if (bt_vendor_cbacks) {
				ALOGI("Bluetooth Firmware and transport layer are initialized");
				bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);
			}
			else{
				ALOGE("bt_vendor_cbacks is null");
				ALOGE("Error : hci, smd initialization Error");
				retval = -1;
			}
			break;

		case BT_VND_OP_SCO_CFG:
			if (bt_vendor_cbacks)
				bt_vendor_cbacks->scocfg_cb(BT_VND_OP_RESULT_SUCCESS); //dummy
			break;

		case BT_VND_OP_USERIAL_OPEN:
			ALOGI("bt-vendor : BT_VND_OP_USERIAL_OPEN");
			fd = userial_vendor_open((tUSERIAL_CFG *) &userial_init_cfg);
			if (fd != -1) {
				for (idx=0; idx < CH_MAX; idx++)
					(*fd_array)[idx] = fd;

				retval = 1;
			}

			if (!is_soc_initialized() && retval) {
				ALOGV("rome_soc_init is started");
				property_set("bluetooth.soc_initialized", "0");
				if (rome_soc_init(fd, DEFAULT_BAUDRATE) < 0) {
					retval = -1;
				} else {
					ALOGD("rome_soc_init is completed @2Mbaud");
					property_set("bluetooth.soc_initialized", "1");
				}
			}

			property_set("bluetooth.clean_up","0");
			break;

		case BT_VND_OP_USERIAL_CLOSE:
			property_set("bluetooth.clean_up","1");
			userial_vendor_close();
			break;

		case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
			break;

		case BT_VND_OP_LPM_SET_MODE:
			if (bt_vendor_cbacks)
				bt_vendor_cbacks->lpm_cb(BT_VND_OP_RESULT_SUCCESS); //dummy
			break;

		case BT_VND_OP_LPM_WAKE_SET_STATE:
			state = (uint8_t *) param;
			wake_assert = (*state == BT_VND_LPM_WAKE_ASSERT) ? \
					      BT_VND_LPM_WAKE_ASSERT : BT_VND_LPM_WAKE_DEASSERT;

			if (wake_assert == 0)
				ALOGV("ASSERT: Waking up BT-Device");
			else if (wake_assert == 1)
				ALOGV("DEASSERT: Allowing BT-Device to Sleep");
			break;

		case BT_VND_OP_EPILOG:
#if (HW_NEED_END_WITH_HCI_RESET == FALSE)
			if (bt_vendor_cbacks) {
				bt_vendor_cbacks->epilog_cb(BT_VND_OP_RESULT_SUCCESS);
			}
#else
			property_get("bluetooth.hci_filter_status", value, "0");
			if (is_soc_initialized()&& (strcmp(value,"1") == 0)) {
				hw_epilog_process();
			} else {
				if (bt_vendor_cbacks) {
					ALOGE("vendor lib epilog process aborted");
					bt_vendor_cbacks->epilog_cb(BT_VND_OP_RESULT_SUCCESS);
				}
			}
#endif
			break;

		default:
			break;
	}

	return retval;
}

/** Closes the interface */
static void cleanup(void)
{
	ALOGI("cleanup");
	bt_vendor_cbacks = NULL;

#ifdef WIFI_BT_STATUS_SYNC
	isInit = 0;
#endif /* WIFI_BT_STATUS_SYNC */
}

// Entry point of DLib
const bt_vendor_interface_t BLUETOOTH_VENDOR_LIB_INTERFACE = {
	sizeof(bt_vendor_interface_t),
	init,
	op,
	cleanup,
};
