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

// JK BMS 藍牙名稱前綴
static const char remote_device_name[] = "JK-";
static bool connect = false;

// 儲存連線所需的介面與 ID
static esp_gatt_if_t jk_bms_gattc_if = ESP_GATT_IF_NONE;
static uint16_t jk_bms_conn_id = 0;
static esp_bd_addr_t jk_bms_remote_bda;

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

void jk_bms_init(void) {
    ESP_LOGI(TAG, "JK BMS module initialized, registering GATTC...");
    esp_err_t ret = esp_ble_gattc_register_callback(jk_bms_gattc_event_handler);
    if(ret){
        ESP_LOGE(TAG, "GATTC register error, error code = %x", ret);
    }
    ret = esp_ble_gattc_app_register(0);
    if (ret){
        ESP_LOGE(TAG, "GATTC app register error, error code = %x", ret);
    }
}

void jk_bms_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
            uint32_t duration = 30; // 掃描持續 30 秒
            esp_ble_gap_start_scanning(duration);
            break;
        }
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Scan start failed, error status = %x", param->scan_start_cmpl.status);
            } else {
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
                                ESP_LOGI(TAG, "Ready to connect to JK BMS...");
                                
                                // 記錄目標 MAC 位址並發起連線
                                memcpy(jk_bms_remote_bda, scan_result->scan_rst.bda, sizeof(esp_bd_addr_t));
                                esp_ble_gattc_open(jk_bms_gattc_if, scan_result->scan_rst.bda, scan_result->scan_rst.ble_addr_type, true);
                            }
                        }
                    }
                    break;
                }
                case ESP_GAP_SEARCH_INQ_CMPL_EVT:
                    ESP_LOGI(TAG, "Scan complete");
                    if (!connect) {
                        esp_ble_gap_start_scanning(30); // 若未連線，重新啟動掃描
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS){
                ESP_LOGE(TAG, "Scan stop failed, error status = %x", param->scan_stop_cmpl.status);
            }
            else {
                ESP_LOGI(TAG, "Stop scan successfully");
            }
            break;
        default:
            break;
    }
}

void jk_bms_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
        case ESP_GATTC_REG_EVT:
            ESP_LOGI(TAG, "GATTC register success, start configuring scan parameters");
            jk_bms_gattc_if = gattc_if;
            esp_err_t scan_ret = esp_ble_gap_set_scan_params(&ble_scan_params);
            if (scan_ret){
                ESP_LOGE(TAG, "Set scan params error, error code = %x", scan_ret);
            }
            break;
        case ESP_GATTC_CONNECT_EVT:
            ESP_LOGI(TAG, "Connected to JK BMS, conn_id = %d", param->connect.conn_id);
            jk_bms_conn_id = param->connect.conn_id;
            jk_bms_data.ui8_connected = 1;
            
            // 連線成功後，立即要求搜尋遠端設備的服務
            esp_ble_gattc_search_service(gattc_if, param->connect.conn_id, NULL);
            break;
        case ESP_GATTC_DISCONNECT_EVT:
            ESP_LOGI(TAG, "Disconnected from JK BMS, reason = 0x%x", param->disconnect.reason);
            connect = false;
            jk_bms_data.ui8_connected = 0;
            jk_bms_conn_id = 0;
            
            // 斷線後重新啟動掃描
            esp_ble_gap_start_scanning(30);
            break;
        case ESP_GATTC_SEARCH_RES_EVT:
            // 當找到任何服務時會觸發此事件，先將找到的 UUID 印出以供確認
            ESP_LOGI(TAG, "Found service UUID: 0x%04x", param->search_res.srvc_id.uuid.uuid.uuid16);
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
            if (param->search_cmpl.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Service search failed, error status = %x", param->search_cmpl.status);
                break;
            }
            ESP_LOGI(TAG, "Service search complete.");
            break;
        default:
            break;
    }
}