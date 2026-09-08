#include "jk_bms_ble.h"
#include "esp_log.h"
#include "tsdz_data.h"
#include <string.h>

/* 補齊 BLE 核心標頭檔 */
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

static const char *TAG = "jk_bms";

/* JK BMS 標準 UUID 定義 */
#define REMOTE_SERVICE_UUID        0xFFE0
#define REMOTE_NOTIFY_CHAR_UUID    0xFFE1

static const char remote_device_name[] = "JK-";
static bool connect = false;
static bool get_server = false;

// 儲存連線所需的介面與 ID
static esp_gatt_if_t jk_bms_gattc_if = ESP_GATT_IF_NONE;
static uint16_t jk_bms_conn_id = 0;
static esp_bd_addr_t jk_bms_remote_bda;

// 儲存 BLE 服務與特徵值控制代碼
static uint16_t jk_bms_start_handle = 0;
static uint16_t jk_bms_end_handle = 0;
static uint16_t jk_bms_char_handle = 0;

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

/* BMS 數據接收緩衝區 */
#define BMS_RX_BUF_SIZE 320
static uint8_t bms_rx_buf[BMS_RX_BUF_SIZE];
static uint16_t bms_rx_len = 0;

/* 提前宣告 (Forward Declaration) 以解決 undeclared 錯誤 */
void jk_bms_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
void jk_bms_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

/* 數據解析函式框架 */
static void jk_bms_parse_frame(uint8_t *data, uint16_t len) {
    // 待硬體測試確認原始資料封包特徵後，於此處實作：
    ESP_LOGI(TAG, "BMS Full Frame Received (Len: %d)", len);
    
    // 解析範例：
    // jk_bms_data.ui16_voltage_x100 = ...;
    // jk_bms_data.i16_current_x100 = ...;
    // jk_bms_data.ui8_soc = ...;
}

void jk_bms_init(void) {
    ESP_LOGI(TAG, "JK BMS module initialized, registering GATTC...");
    esp_ble_gattc_register_callback(jk_bms_gattc_event_handler);
    esp_ble_gattc_app_register(0);
}

void jk_bms_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
            uint32_t duration = 30; // 掃描持續 30 秒
            esp_ble_gap_start_scanning(duration);
            break;
        }
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Scan start success");
            }
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
            switch (scan_result->scan_rst.search_evt) {
                case ESP_GAP_SEARCH_INQ_RES_EVT: {
                    uint8_t *adv_name = NULL;
                    uint8_t adv_name_len = 0;
                    adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
                    if (adv_name != NULL) {
                        if (strncmp((char *)adv_name, remote_device_name, strlen(remote_device_name)) == 0) {
                            ESP_LOGI(TAG, "Found JK BMS: %.*s", adv_name_len, adv_name);
                            if (!connect) {
                                connect = true;
                                esp_ble_gap_stop_scanning();
                                memcpy(jk_bms_remote_bda, scan_result->scan_rst.bda, sizeof(esp_bd_addr_t));
                                esp_ble_gattc_open(jk_bms_gattc_if, scan_result->scan_rst.bda, scan_result->scan_rst.ble_addr_type, true);
                            }
                        }
                    }
                    break;
                }
                case ESP_GAP_SEARCH_INQ_CMPL_EVT:
                    if (!connect) {
                        esp_ble_gap_start_scanning(30); // 若未連線，重新啟動掃描
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
}

void jk_bms_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
        case ESP_GATTC_REG_EVT:
            jk_bms_gattc_if = gattc_if;
            esp_ble_gap_set_scan_params(&ble_scan_params);
            break;
        case ESP_GATTC_CONNECT_EVT:
            ESP_LOGI(TAG, "Connected to JK BMS");
            jk_bms_conn_id = param->connect.conn_id;
            jk_bms_data.ui8_connected = 1;
            get_server = false;
            esp_ble_gattc_search_service(gattc_if, param->connect.conn_id, NULL);
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            ESP_LOGI(TAG, "Disconnected from JK BMS");
            connect = false;
            get_server = false;
            jk_bms_data.ui8_connected = 0;
            jk_bms_conn_id = 0;
            esp_ble_gap_start_scanning(30);
            break;
        case ESP_GATTC_SEARCH_RES_EVT:
            // 尋找 0xFFE0 服務
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 && 
                param->search_res.srvc_id.uuid.uuid.uuid16 == REMOTE_SERVICE_UUID) {
                get_server = true;
                jk_bms_start_handle = param->search_res.start_handle;
                jk_bms_end_handle = param->search_res.end_handle;
                ESP_LOGI(TAG, "Found JK BMS Service (0xFFE0)");
            }
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (get_server) {
                esp_bt_uuid_t char_uuid;
                char_uuid.len = ESP_UUID_LEN_16;
                char_uuid.uuid.uuid16 = REMOTE_NOTIFY_CHAR_UUID;
                
                uint16_t count = 1;
                esp_gattc_char_elem_t char_elem;
                esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(
                    gattc_if,
                    param->search_cmpl.conn_id,
                    jk_bms_start_handle,
                    jk_bms_end_handle,
                    char_uuid,
                    &char_elem,
                    &count
                );
                
                if (status == ESP_GATT_OK && count > 0) {
                    jk_bms_char_handle = char_elem.char_handle;
                    // 向底層註冊通知接收
                    esp_ble_gattc_register_for_notify(gattc_if, jk_bms_remote_bda, jk_bms_char_handle);
                    ESP_LOGI(TAG, "Registered for notify on characteristic 0xFFE1");
                } else {
                    ESP_LOGE(TAG, "Failed to find characteristic 0xFFE1");
                }
            }
            break;
        case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
            if (param->reg_for_notify.status == ESP_GATT_OK) {
                uint16_t count = 1;
                esp_gattc_descr_elem_t descr_elem;
                esp_bt_uuid_t descr_uuid;
                descr_uuid.len = ESP_UUID_LEN_16;
                descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
                
                // 獲取 CCCD 描述符控制代碼
                esp_gatt_status_t status = esp_ble_gattc_get_descr_by_char_handle(
                    gattc_if,
                    jk_bms_conn_id,
                    param->reg_for_notify.handle,
                    descr_uuid,
                    &descr_elem,
                    &count
                );
                
                if (status == ESP_GATT_OK && count > 0) {
                    uint8_t notify_en[2] = {0x01, 0x00};
                    // 寫入 0x0001 啟用藍牙層級 Notification
                    esp_ble_gattc_write_char_descr(
                        gattc_if,
                        jk_bms_conn_id,
                        descr_elem.handle,
                        sizeof(notify_en),
                        notify_en,
                        ESP_GATT_WRITE_TYPE_RSP,
                        ESP_GATT_AUTH_REQ_NONE
                    );
                    ESP_LOGI(TAG, "Notification enabled in CCCD");
                }
            }
            break;
        }
        case ESP_GATTC_NOTIFY_EVT:
            if (param->notify.is_notify) {
                // 將接收到的短封包推入緩衝區拼接
                if (bms_rx_len + param->notify.value_len <= BMS_RX_BUF_SIZE) {
                    memcpy(&bms_rx_buf[bms_rx_len], param->notify.value, param->notify.value_len);
                    bms_rx_len += param->notify.value_len;
                } else {
                    bms_rx_len = 0; // 緩衝區溢出，捨棄並重置
                }

                // 檢查是否已接收完單次完整資料幀 (通常 JK BMS 完整廣播幀大於 290 bytes)
                if (bms_rx_len >= 290) {
                    jk_bms_parse_frame(bms_rx_buf, bms_rx_len);
                    bms_rx_len = 0; // 解析完畢，清空緩衝區等待下一幀
                }
            }
            break;
        default:
            break;
    }
}