
#include "custom_ble.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"

#include "commands.h"
#include "conf_general.h"
#include "main.h"
#include "utils.h"
#include "packet.h"

#define ADV_CFG_FLAG      (1 << 0)
#define SCAN_RSP_CFG_FLAG (1 << 1)

typedef uint8_t custom_ble_id_t;

typedef enum {
	CUSTOM_BLE_TYPE_CHR,
	CUSTOM_BLE_TYPE_DESCR,
} custom_ble_attr_type_t;

typedef struct {
	custom_ble_id_t service_index;
	uint16_t chr_handle;
	esp_bt_uuid_t uuid;
	custom_ble_attr_type_t type;
	bool initialized;
} attr_instance_t;

typedef struct {
	uint16_t service_handle;
	esp_bt_uuid_t uuid;
	bool initialized;
} service_instance_t;

static bool has_started                = false;
static custom_ble_result_t init_result = false;

static char device_name[CUSTOM_BLE_MAX_NAME_LEN + 1];

static size_t custom_service_len           = 0;
static service_instance_t *custom_services = NULL;
static size_t custom_attr_len              = 0;
static attr_instance_t *custom_attr        = NULL;

static int waiting_add_service_index     = -1;
static int waiting_remove_service_handle = -1;

static uint16_t waiting_handle_indices_count = 0;
static uint16_t waiting_handle_indices[CUSTOM_BLE_MAX_CHR_AND_DESCR_COUNT + 1];

static bool result_ready             = false;
static uint16_t result_handles_count = 0;
static uint16_t result_handles[CUSTOM_BLE_MAX_CHR_AND_DESCR_COUNT + 1];
static esp_gatt_status_t result_status;

static esp_gatt_if_t stored_gatts_if;
static bool is_connected        = false;
static uint16_t ble_current_mtu = 20;

static uint8_t adv_config_done = 0;

static esp_ble_adv_data_t ble_adv_data = {
	.set_scan_rsp     = false,
	.include_name     = true,
	.include_txpower  = false,
	.min_interval     = 0x06,
	.max_interval     = 0x30,
	.appearance       = 0x00,
	.manufacturer_len = 0,
	.service_data_len = 0,
	.service_uuid_len = 0,
	// .p_service_uuid = ble_service_uuid128,
	.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t ble_scan_rsp_data = {
	.set_scan_rsp        = true,
	.include_name        = false,
	.include_txpower     = true,
	.min_interval        = 0x06,
	.max_interval        = 0x30,
	.appearance          = 0x00,
	.manufacturer_len    = 0,
	.p_manufacturer_data = NULL,
	.service_data_len    = 0,
	.p_service_data      = NULL,
	.service_uuid_len    = 0,
	// .service_uuid_len = ESP_UUID_LEN_128,
	// .p_service_uuid = ble_service_uuid128,
	.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t ble_adv_params = {
	.adv_int_min       = 0x20,
	.adv_int_max       = 0x40,
	.adv_type          = ADV_TYPE_IND,
	.own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
	.channel_map       = ADV_CHNL_ALL,
	.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/**
 * Stringify the given uuid and return the result. The buffer is only valid
 * until this function is called again, as the same buffer is reused for every
 * call.
 */
static const char *get_temp_uuid_str(esp_bt_uuid_t uuid) {
	static char buffer[ESP_UUID_LEN_128 * 3 + 1];
	buffer[0] = '\0';

	uint16_t len   = uuid.len;
	uint8_t *bytes = (uint8_t *)&uuid.uuid;

	int written = 0;
	for (uint16_t i = 0; i < len; i++) {
		int res = sprintf(buffer + written, "%02x ", bytes[i]);
		if (res == -1) {
			strcpy(buffer, "failed");
			return buffer;
		}

		written += res;
	}

	return buffer;
};

static void print_attr_db(
	uint16_t len, const esp_gatts_attr_db_t attr_db[len]
) {
	stored_printf("%u entries:", len);
	for (uint16_t i = 0; i < len; i++) {
		stored_printf(
			"{\n"
			"  attr_control = {%u}\n"
			"  att_desc = {",
			attr_db[i].attr_control.auto_rsp
		);

		uint16_t uuid_len = attr_db[i].att_desc.uuid_length;
		bool valid_length = uuid_len == ESP_UUID_LEN_16
			|| uuid_len == ESP_UUID_LEN_32 || uuid_len == ESP_UUID_LEN_128;
		const char *uuid_str = "<invalid>";
		if (valid_length) {
			esp_bt_uuid_t uuid = {
				.len = uuid_len,
			};
			memcpy(&uuid.uuid, attr_db[i].att_desc.uuid_p, uuid_len);
			uuid_str = get_temp_uuid_str(uuid);
		}
		stored_printf(
			"    uuid_length = %u\n"
			"    uuid_p = %s\n"
			"    perm = %u\n"
			"    max_length = %u\n"
			"    length = %u\n"
			"    value = %p\n"
			"  }\n"
			"}",
			uuid_len, uuid_str, attr_db[i].att_desc.perm,
			attr_db[i].att_desc.max_length, attr_db[i].att_desc.length,
			attr_db[i].att_desc.value
		);
	}
}

static bool uuid_eq(esp_bt_uuid_t a, esp_bt_uuid_t b) {
	if (a.len != b.len) {
		return false;
	}

	switch (a.len) {
		case ESP_UUID_LEN_16: {
			return a.uuid.uuid16 == b.uuid.uuid16;
		}
		case ESP_UUID_LEN_32: {
			return a.uuid.uuid32 == b.uuid.uuid32;
		}
		case ESP_UUID_LEN_128: {
			for (size_t i = 0; i < ESP_UUID_LEN_128; i++) {
				if (a.uuid.uuid128[i] != b.uuid.uuid128[i]) {
					return false;
				}
			}
			return true;
		}
		default: {
			return false;
		}
	}
}

/**
 * Get a service's index by it's handle.
 *
 * @param service_handle The service whose you wan't to get.
 * @param service_index Will be set to the index if found.
 * @return A boolean indicating if the specified service exists.
 */
static bool get_service_index(
	uint16_t service_handle, custom_ble_id_t *service_index
) {
	for (size_t i = 0; i < custom_service_len; i++) {
		if (custom_services[i].initialized
			&& custom_services[i].service_handle == service_handle) {
			*service_index = i;
			return true;
		}
	}

	return false;
}

/**
 * @param handles A list of handles as found in custom_ble_add_service. The
 * service handle is first in the list, followed by all characteristic handles,
 * where each characteristic's descriptor handles immediately follows it's
 * handle.
 */
static bool initialize_service_with_handles(
	custom_ble_id_t service_index, size_t len, uint16_t handles[len]
) {
	if (len < 1) {
		return false;
	}
	if (service_index >= custom_service_len) {
		return false;
	}

	custom_services[service_index].service_handle = handles[0];
	custom_services[service_index].initialized    = true;

	size_t handle_index = 0;
	for (size_t i = 0; i < custom_attr_len; i++) {
		if (custom_attr[i].service_index == service_index) {
			if (handle_index >= len) {
				return false;
			}

			custom_attr[i].initialized = true;
			custom_attr[i].chr_handle  = handles[handle_index];
		}
	}

	return true;
}

static void gap_event_handler(
	esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param
) {
	stored_printf("gap event %d", event);

	switch (event) {
		case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
			adv_config_done &= (~ADV_CFG_FLAG);
			if (adv_config_done == 0) {
				esp_ble_gap_start_advertising(&ble_adv_params);
			}
			break;

		case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
			adv_config_done &= (~SCAN_RSP_CFG_FLAG);
			if (adv_config_done == 0) {
				esp_ble_gap_start_advertising(&ble_adv_params);
			}
			break;

		default:
			break;
	}
}

static void gatts_event_handler(
	esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
	esp_ble_gatts_cb_param_t *param
) {
	stored_printf("gatts event %d", event);
	switch (event) {
		case ESP_GATTS_REG_EVT: {
			// There should only ever be one gatt interface.
			stored_gatts_if = gatts_if;

			esp_ble_gap_set_device_name(device_name);

			esp_ble_gap_config_adv_data(&ble_adv_data);
			adv_config_done |= ADV_CFG_FLAG;

			esp_ble_gap_config_adv_data(&ble_scan_rsp_data);
			adv_config_done |= SCAN_RSP_CFG_FLAG;

			break;
		}
		case ESP_GATTS_WRITE_EVT: {
			if (!param->write.is_prep) {
				// TODO: Do I need to handle notifications here?

				if (param->write.need_rsp) {
					esp_ble_gatts_send_response(
						gatts_if, param->write.conn_id, param->write.trans_id,
						ESP_GATT_OK, NULL
					);
				}
			} else {
				stored_printf("I need to handle prepared writes...");
			}

			break;
		}
		case ESP_GATTS_EXEC_WRITE_EVT: {
			stored_printf("I need to handle execute writes...");

			break;
		}
		case ESP_GATTS_MTU_EVT: {
			ble_current_mtu = param->mtu.mtu;

			break;
		}
		case ESP_GATTS_DELETE_EVT: {
			stored_printf(
				"remove service, status: %d, service_handle: %u",
				param->del.status, param->del.service_handle
			);

			if (waiting_remove_service_handle != -1
				&& waiting_remove_service_handle == param->del.service_handle) {
				result_status = param->del.status;
				result_ready  = true;
			}

			break;
		}
		case ESP_GATTS_START_EVT: {
			stored_printf(
				"service start, status: %d, service_handle: %u",
				param->start.status, param->start.service_handle
			);
			break;
		}
		case ESP_GATTS_CONNECT_EVT: {
			is_connected = true;
			LED_BLUE_ON();

			esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P18);
			esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL1, ESP_PWR_LVL_P18);
			esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL2, ESP_PWR_LVL_P18);
			esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P18);
			esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P18);
			esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P18);

			// TODO: Should investigate if this is necessary for IOS.
			// esp_ble_conn_update_params_t conn_params = {0};
			// memcpy(
			// 	conn_params.bda, param->connect.remote_bda,
			// 	sizeof(esp_bd_addr_t)
			// );
			// /* For the iOS system, please refer to Apple official documents
			//  * about the BLE connection parameters restrictions. */
			// conn_params.latency = 0;
			// conn_params.max_int = 0x20; // max_int = 0x20*1.25ms = 40ms
			// conn_params.min_int = 0x10; // min_int = 0x10*1.25ms = 20ms
			// conn_params.timeout = 400;	// timeout = 400*10ms = 4000ms
			// // start sent the update connection parameters to the peer
			// device. esp_ble_gap_update_conn_params(&conn_params);

			break;
		}
		case ESP_GATTS_DISCONNECT_EVT: {
			is_connected = false;
			LED_BLUE_OFF();

			esp_ble_gap_start_advertising(&ble_adv_params);

			break;
		}
		case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
			stored_printf(
				"created attribute table; status: %u, svc_inst_id: %u, "
				"num_handle: %u, waiting_add_service_index: %d, uuid equal: %s",
				param->add_attr_tab.status, param->add_attr_tab.svc_inst_id,
				param->add_attr_tab.num_handle, waiting_add_service_index,
				waiting_add_service_index != -1
						&& uuid_eq(
							param->add_attr_tab.svc_uuid,
							custom_services[waiting_add_service_index].uuid
						)
					? "true"
					: "false"
			);
			stored_printf(
				"svc_uuid (%u): %s", param->add_attr_tab.svc_uuid.len,
				get_temp_uuid_str(param->add_attr_tab.svc_uuid)
			);
			stored_printf(
				"custom_services[].uuid (%u): %s",
				custom_services[waiting_add_service_index].uuid.len,
				get_temp_uuid_str(
					custom_services[waiting_add_service_index].uuid
				)
			);
			if (param->add_attr_tab.status != ESP_GATT_OK) {
				// TODO: Should somehow report to the waiting thread, instead of
				// just letting it timeout...
				break;
			}

			if (param->add_attr_tab.num_handle >= 1) {
				esp_err_t result =
					esp_ble_gatts_start_service(param->add_attr_tab.handles[0]);
				stored_printf(
					"esp_ble_gatts_start_service(%u), result: %d",
					param->add_attr_tab.handles[0], result
				);
				if (result != ESP_OK) {
					// TODO: Should somehow report to the waiting thread,
					// instead of just letting it timeout...
					break;
				}
			}

			if (waiting_add_service_index != -1
				&& param->add_attr_tab.svc_inst_id
					== waiting_add_service_index) {
				if (NUMBER_OF(result_handles) < waiting_handle_indices_count) {
					stored_printf(
						"number of requested handles are too great! "
						"waiting_handle_indices_count: "
						"%u",
						waiting_handle_indices_count
					);
					// TODO: Same as above...
					break;
				}

				for (uint16_t i = 0; i < waiting_handle_indices_count; i++) {
					uint16_t index = waiting_handle_indices[i];
					if (index >= param->add_attr_tab.num_handle) {
						stored_printf(
							"requested handle index %u is invalid! "
							"must be less than num_handle: %u"
							"%u",
							index, param->add_attr_tab.num_handle
						);
						// TODO: Same as above...
						break;
					}

					result_handles[i] = param->add_attr_tab.handles[index];
				}

				waiting_add_service_index = -1;
				result_handles_count      = waiting_handle_indices_count;
				result_ready              = true;
			}

			break;
		}
		default: {
			break;
		}
	}
}

custom_ble_result_t custom_ble_start() {
	if (init_result != CUSTOM_BLE_OK) {
		return CUSTOM_BLE_INIT_FAILED;
	}

	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
	esp_bt_controller_init(&bt_cfg);

	esp_bt_controller_enable(ESP_BT_MODE_BLE);
	esp_bluedroid_init();
	esp_bluedroid_enable();

	esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P18);
	esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL1, ESP_PWR_LVL_P18);
	esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL2, ESP_PWR_LVL_P18);
	esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P18);
	esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, ESP_PWR_LVL_P18);
	esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P18);

	esp_bt_dev_set_device_name(device_name);

	esp_ble_gatts_register_callback(gatts_event_handler);
	esp_ble_gap_register_callback(gap_event_handler);
	esp_ble_gatts_app_register(0);

	return CUSTOM_BLE_OK;
}

custom_ble_result_t custom_ble_set_name(const char *name) {
	if (init_result != CUSTOM_BLE_OK) {
		return CUSTOM_BLE_INIT_FAILED;
	}

	if (has_started) {
		return CUSTOM_BLE_ALREADY_STARTED;
	}

	size_t len = strlen(name);
	if (len > CUSTOM_BLE_MAX_NAME_LEN) {
		return CUSTOM_BLE_NAME_TOO_LONG;
	}

	strcpy(device_name, name);

	has_started = true;

	return CUSTOM_BLE_OK;
};

custom_ble_result_t custom_ble_add_service(
	esp_bt_uuid_t service_uuid, uint16_t chr_count,
	const ble_chr_definition_t chr[chr_count], service_handles_cb_t handles_cb
) {
	void push_wanted_handle_index(uint16_t handle_index) {
		waiting_handle_indices[waiting_handle_indices_count++] = handle_index;
	}
	static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
	static const uint16_t character_declaration_uuid =
		ESP_GATT_UUID_CHAR_DECLARE;

	stored_printf("inside custom_ble_add_service, chr_count: %u", chr_count);
	if (init_result != CUSTOM_BLE_OK) {
		return CUSTOM_BLE_INIT_FAILED;
	}

	if (!has_started) {
		return CUSTOM_BLE_NOT_STARTED;
	}

	if (custom_service_len + 1 >= CUSTOM_BLE_MAX_SERVICE_COUNT) {
		return CUSTOM_BLE_TOO_MANY_SERVICES;
	}

	uint16_t chr_and_descr_count = chr_count;
	for (size_t i = 0; i < chr_count; i++) {
		chr_and_descr_count += chr[i].descr_count;
	}

	if (custom_attr_len + chr_and_descr_count
		>= CUSTOM_BLE_MAX_CHR_AND_DESCR_COUNT) {
		return CUSTOM_BLE_TOO_MANY_CHR_AND_DESCR;
	}

	waiting_handle_indices_count = 0;

	uint16_t attr_count = 1 + chr_and_descr_count + chr_count;

	stored_printf(
		"attr_count: %u, chr_and_descr_count: %u", attr_count,
		chr_and_descr_count
	);

	esp_gatts_attr_db_t table[attr_count];
	uint16_t table_index = 0;

	custom_ble_id_t service_index  = custom_service_len++;
	custom_services[service_index] = (service_instance_t){
		.initialized = false,
		.uuid        = service_uuid,
	};

	// Service declaration
	push_wanted_handle_index(table_index);
	table[table_index++] = (esp_gatts_attr_db_t){
		.attr_control = {ESP_GATT_AUTO_RSP},
		.att_desc =
			{
				.uuid_length = sizeof(primary_service_uuid),
				// This doesn't feel very endianess-safe...
				.uuid_p      = (uint8_t *)&primary_service_uuid,
				.perm        = ESP_GATT_PERM_READ,
				.max_length  = service_uuid.len,
				.length      = service_uuid.len,
				.value       = (uint8_t *)&service_uuid.uuid,
			},
	};

	// It is safe to pass references of this variable to
	// esp_ble_gatts_create_attr_tab, since this function waits for the gatts
	// event ESP_GATTS_CREAT_ATTR_TAB_EVT ensuring that this variable doesn't
	// go out of scope until the bluetooth controller (btc) has read it's value
	// (and made its own copy).
	// TODO: But what if we timeout before the btc reads these values, causing
	// it to read invalid values?
	uint8_t prop_flag_values[chr_count];

	for (uint16_t i = 0; i < chr_count; i++) {
		custom_attr[custom_attr_len++] = (attr_instance_t){
			.service_index = service_index,
			.initialized   = false,
			.uuid          = chr[i].uuid,
			.type          = CUSTOM_BLE_TYPE_CHR,
		};

		prop_flag_values[i] = chr[i].property;

		// Characteristic declaration
		table[table_index++] = (esp_gatts_attr_db_t){
			.attr_control = {ESP_GATT_AUTO_RSP},
			.att_desc =
				{
					.uuid_length = ESP_UUID_LEN_16,
					.uuid_p      = (uint8_t *)&character_declaration_uuid,
					.perm        = ESP_GATT_PERM_READ,
					.max_length  = sizeof(uint8_t),
					.length      = sizeof(uint8_t),
					.value       = &prop_flag_values[i],
				},
		};

		// Characteristic value
		push_wanted_handle_index(table_index);
		table[table_index++] = (esp_gatts_attr_db_t){
			.attr_control = {ESP_GATT_AUTO_RSP},
			.att_desc =
				{
					.uuid_length = chr[i].uuid.len,
					.uuid_p      = (uint8_t *)&chr[i].uuid.uuid,
					.perm        = chr[i].perm,
					.max_length  = chr[i].value_len,
					.length      = chr[i].value_len,
					.value       = chr[i].value,
				},
		};

		// Characteristic descriptors
		for (uint16_t j = 0; j < chr[i].descr_count; j++) {
			custom_attr[custom_attr_len++] = (attr_instance_t){
				.service_index = service_index,
				.initialized   = false,
				.uuid          = chr[i].descriptors[j].uuid,
				.type          = CUSTOM_BLE_TYPE_DESCR,
			};

			push_wanted_handle_index(table_index);
			table[table_index++] = (esp_gatts_attr_db_t){
				.attr_control = {ESP_GATT_AUTO_RSP},
				.att_desc =
					{
						.uuid_length = chr[i].descriptors[j].uuid.len,
						.uuid_p = (uint8_t *)&chr[i].descriptors[j].uuid.uuid,
						.perm   = chr[i].descriptors[j].perm,
						.max_length = chr[i].descriptors[j].value_len,
						.length     = chr[i].descriptors[j].value_len,
						.value      = chr[i].descriptors[j].value,
					},
			};
		}
	}

	stored_printf("table_index: %u, attr_count: %u", table_index, attr_count);
	if (table_index != attr_count) {
		return CUSTOM_BLE_ERROR;
	}

	waiting_add_service_index = service_index;
	result_ready              = false;

	stored_printf(
		"esp_ble_gatts_create_attr_tab, gatts_if: %u, attr_count: %u, "
		"service_index: %u",
		stored_gatts_if, attr_count, service_index
	);
	print_attr_db(attr_count, table);
	esp_err_t result = esp_ble_gatts_create_attr_tab(
		table, stored_gatts_if, attr_count, service_index
	);
	if (result != ESP_OK) {
		stored_printf("esp_ble_gatts_create_attr_tab error: %d", result);

		return CUSTOM_BLE_ESP_ERROR;
	}

	size_t tries = 0;
	while (true) {
		if (tries >= 100) {
			return CUSTOM_BLE_TIMEOUT;
		}
		if (result_ready) {
			break;
		}

		tries++;
		vTaskDelay(10 / portTICK_PERIOD_MS);
	}
	
	if (!initialize_service_with_handles(service_index, result_handles_count, result_handles)) {
		stored_printf("initialize_service_with_handles failed");
		return CUSTOM_BLE_INTERNAL_ERROR;
	}
	
	handles_cb(result_handles_count, result_handles);

	return CUSTOM_BLE_OK;
}

custom_ble_result_t custom_ble_remove_service(uint16_t service_handle) {
	if (!has_started) {
		return CUSTOM_BLE_NOT_STARTED;
	}

	custom_ble_id_t service_index;
	if (!get_service_index(service_handle, &service_index)) {
		return CUSTOM_BLE_INVALID_HANDLE;
	}

	if (service_index != custom_service_len - 1) {
		return CUSTOM_BLE_SERVICE_NOT_LAST;
	}

	result_ready                  = false;
	waiting_remove_service_handle = service_handle;

	custom_ble_result_t result = esp_ble_gatts_delete_service(service_handle);

	if (result != ESP_OK) {
		return CUSTOM_BLE_ESP_ERROR;
	}

	size_t tries = 0;
	while (true) {
		if (tries >= 100) {
			return CUSTOM_BLE_TIMEOUT;
		}
		if (result_ready) {
			break;
		}

		tries++;
		vTaskDelay(10 / portTICK_PERIOD_MS);
	}

	if (result_status != ESP_GATT_OK) {
		stored_printf("delete service failed, status: %d", result_status);

		return CUSTOM_BLE_ESP_ERROR;
	}

	// free service resources
	size_t least_attr_index = SIZE_MAX;
	for (size_t i = 0; i < custom_attr_len; i++) {
		if (custom_attr[i].service_index == service_index) {
			if (i < least_attr_index) {
				least_attr_index = i;
			}
		} else {
			if (i > least_attr_index) {
				// This shouldn't ever happen...
				stored_printf(
					"found attr index %u that shouldn't be removed above the "
					"attrs to remove, least_attr_index: %u",
					i, least_attr_index
				);
				return CUSTOM_BLE_INTERNAL_ERROR;
			}
		}
	}
	custom_service_len -= 1;
	custom_attr_len     = least_attr_index;

	return CUSTOM_BLE_OK;
}

custom_ble_result_t custom_ble_get_attr_value(
	uint16_t attr_handle, uint16_t *length, const uint8_t **value
) {
	esp_gatt_status_t result =
		esp_ble_gatts_get_attr_value(attr_handle, length, value);
	if (result == ESP_GATT_INVALID_HANDLE) {
		// TODO: not sure if ESP_GATT_INVALID_HANDLE is the correct error to
		// check for here.
		return CUSTOM_BLE_INVALID_HANDLE;
	}
	if (result != ESP_GATT_OK) {
		stored_printf(
			"esp_ble_gatts_get_attr_value failed, result: %d", result
		);
		return CUSTOM_BLE_ESP_ERROR;
	}

	return CUSTOM_BLE_OK;
}

custom_ble_result_t custom_ble_set_attr_value(
	uint16_t attr_handle, uint16_t length, const uint8_t value[length]
) {
	esp_err_t result = esp_ble_gatts_set_attr_value(attr_handle, length, value);

	// bool found_match = false;
	// for (size_t i = 0; i < custom_attr_len; i++) {
	// 	if (custom_attr[i].chr_handle == attr_handle) {
	// 		found_match = true;
	// 		break;
	// 	}
	// }
	// if (found_match) {
	// 	return CUSTOM_BLE_INVALID_HANDLE;
	// }

	if (result != ESP_OK) {
		stored_printf(
			"esp_ble_gatts_set_attr_value failed, result: %d", result
		);
		return CUSTOM_BLE_ESP_ERROR;
	}

	return CUSTOM_BLE_OK;
}

void custom_ble_init() {
	custom_services =
		calloc(CUSTOM_BLE_MAX_SERVICE_COUNT, sizeof(service_instance_t));
	if (!custom_services) {
		init_result = CUSTOM_BLE_ERROR;
		return;
	}
	custom_attr =
		calloc(CUSTOM_BLE_MAX_CHR_AND_DESCR_COUNT, sizeof(attr_instance_t));
	if (!custom_attr) {
		init_result = CUSTOM_BLE_ERROR;
		return;
	}

	memcpy(device_name, (char *)backup.config.ble_name, 9);
	device_name[9] = '\0';
}