#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_spiffs.h"
#include "jk_bms_ble.h"

static const char *TAG = "jk_bms_ble";

#define JK_BMS_SERVICE_UUID        0xFFE0
#define JK_BMS_CHAR_RX_TX_UUID     0xFFE1
#define PROFILE_NUM 1
#define PROFILE_A_APP_ID 0
#define BLINK_GPIO 2

// 宣告供外部讀取的數值變數
float jk_bms_voltage = 0.0f;
float jk_bms_current = 0.0f;
uint8_t jk_bms_soc = 0;

// 藍牙資料緩衝區 (防範封包分段傳送)
#define JK_BUF_SIZE 512
static uint8_t jk_buf[JK_BUF_SIZE];
static uint16_t jk_buf_len = 0;

static uint8_t bms_sys_state = 0; 
static bool connect = false;
static bool get_server = false;
static esp_gattc_char_elem_t *char_elem_result = NULL;
static esp_gattc_descr_elem_t *descr_elem_result = NULL;

static esp_bt_uuid_t remote_filter_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = JK_BMS_SERVICE_UUID,},
};

static esp_bt_uuid_t remote_filter_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = JK_BMS_CHAR_RX_TX_UUID,},
};

static esp_bt_uuid_t notify_descr_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,},
};

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);

struct gattc_profile_inst {
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t char_handle;
    esp_bd_addr_t remote_bda;
};

static struct gattc_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gattc_cb = gattc_profile_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,
    },
};

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};

static void write_offline_log(const char *msg) {
    FILE* f = fopen("/spiffs/jk_log.txt", "a");
    if (f != NULL) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static void led_indicator_task(void *pvParameter) {
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    while(1) {
        if (bms_sys_state == 1) { 
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else if (bms_sys_state == 0) { 
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else { 
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void jk_bms_send_request(esp_gatt_if_t gattc_if, uint16_t conn_id, uint16_t char_handle)
{
    uint8_t req_data[] = {0x4E, 0x57, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0x29};
    esp_ble_gattc_write_char(gattc_if, conn_id, char_handle, sizeof(req_data), req_data, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
}

static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATTC 註冊成功, 啟動掃描");
        write_offline_log("[系統] 啟動掃描");
        bms_sys_state = 0;
        esp_ble_gap_set_scan_params(&ble_scan_params);
        break;
        
    case ESP_GATTC_CONNECT_EVT:
        ESP_LOGI(TAG, "=> 成功連接到 JK BMS！");
        write_offline_log("[事件] 成功連接到 JK BMS");
        bms_sys_state = 1;
        gl_profile_tab[PROFILE_A_APP_ID].conn_id = p_data->connect.conn_id;
        memcpy(gl_profile_tab[PROFILE_A_APP_ID].remote_bda, p_data->connect.remote_bda, sizeof(esp_bd_addr_t));
        esp_ble_gattc_send_mtu_req(gattc_if, p_data->connect.conn_id);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK){
            bms_sys_state = 2;
        }
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, &remote_filter_service_uuid);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (p_data->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 && p_data->search_res.srvc_id.uuid.uuid.uuid16 == JK_BMS_SERVICE_UUID) {
            get_server = true;
            gl_profile_tab[PROFILE_A_APP_ID].service_start_handle = p_data->search_res.start_handle;
            gl_profile_tab[PROFILE_A_APP_ID].service_end_handle = p_data->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (get_server){
            uint16_t count = 0;
            esp_gatt_status_t status = esp_ble_gattc_get_attr_count( gattc_if,
                                                                     p_data->search_cmpl.conn_id,
                                                                     ESP_GATT_DB_CHARACTERISTIC,
                                                                     gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                                     gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                                     0, &count);
            if (status == ESP_GATT_OK && count > 0){
                char_elem_result = (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * count);
                if (char_elem_result){
                    status = esp_ble_gattc_get_char_by_uuid( gattc_if,
                                                             p_data->search_cmpl.conn_id,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                             remote_filter_char_uuid,
                                                             char_elem_result, &count);
                    if (status == ESP_GATT_OK && count > 0 && (char_elem_result[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)){
                        gl_profile_tab[PROFILE_A_APP_ID].char_handle = char_elem_result[0].char_handle;
                        esp_ble_gattc_register_for_notify (gattc_if, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, char_elem_result[0].char_handle);
                    }
                    free(char_elem_result);
                }
            }
        }
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (p_data->reg_for_notify.status == ESP_GATT_OK){
            uint16_t count = 0;
            esp_gatt_status_t ret_status = esp_ble_gattc_get_attr_count( gattc_if,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                                         ESP_GATT_DB_DESCRIPTOR,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                                         &count);
            if (ret_status == ESP_GATT_OK && count > 0){
                descr_elem_result = (esp_gattc_descr_elem_t *)malloc(sizeof(esp_gattc_descr_elem_t) * count);
                if (descr_elem_result){
                    ret_status = esp_ble_gattc_get_descr_by_char_handle( gattc_if,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                                         p_data->reg_for_notify.handle,
                                                                         notify_descr_uuid,
                                                                         descr_elem_result, &count);
                    if (ret_status == ESP_GATT_OK && count > 0 && descr_elem_result[0].uuid.len == ESP_UUID_LEN_16 && descr_elem_result[0].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG){
                        uint16_t notify_en = 1;
                        esp_ble_gattc_write_char_descr( gattc_if,
                                                        gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                        descr_elem_result[0].handle,
                                                        sizeof(notify_en),
                                                        (uint8_t *)&notify_en,
                                                        ESP_GATT_WRITE_TYPE_RSP,
                                                        ESP_GATT_AUTH_REQ_NONE);
                    }
                    free(descr_elem_result);
                }
            }
        }
        break;
    }
    
    case ESP_GATTC_NOTIFY_EVT:
        // 如果收到新封包的開頭是 0x4E 0x57，代表這是新的一輪資料，清空緩衝區
        if (p_data->notify.value[0] == 0x4E && p_data->notify.value[1] == 0x57) {
            jk_buf_len = 0; 
        }

        // 將接收到的資料塞進我們的緩衝區
        if (jk_buf_len + p_data->notify.value_len < JK_BUF_SIZE) {
            memcpy(jk_buf + jk_buf_len, p_data->notify.value, p_data->notify.value_len);
            jk_buf_len += p_data->notify.value_len;
        }

        // 開始進行「特徵碼比對」解析
        // JK 協議固定特徵：0x83(電壓2碼) -> 0x84(電流2碼) -> 0x85(電量1碼)
        for (uint16_t i = 0; i < (jk_buf_len > 8 ? jk_buf_len - 8 : 0); i++) {
            if (jk_buf[i] == 0x83 && jk_buf[i+3] == 0x84 && jk_buf[i+6] == 0x85) {
                // 解析總電壓 (單位: 0.01V)
                uint16_t vol_raw = (jk_buf[i+1] << 8) | jk_buf[i+2];
                jk_bms_voltage = (float)vol_raw / 100.0f;
                
                // 解析總電流 (最高位元決定充放電方向)
                uint16_t cur_raw = (jk_buf[i+4] << 8) | jk_buf[i+5];
                if (cur_raw & 0x8000) {
                    jk_bms_current = (float)(cur_raw & 0x7FFF) / 100.0f; // 充電
                } else {
                    jk_bms_current = (float)cur_raw / -100.0f; // 放電 (我們顯示為負值)
                }
                
                // 解析真實剩餘電量 SOC (%)
                jk_bms_soc = jk_buf[i+7];
                
                // 印出人類看得懂的數據！
                ESP_LOGI(TAG, "【解碼成功】電壓: %.2f V | 電流: %.2f A | 電量: %d %%", jk_bms_voltage, jk_bms_current, jk_bms_soc);
                
                // 寫入離線日誌
                char log_str[64];
                sprintf(log_str, "[數據] V:%.2f I:%.2f SOC:%d%%", jk_bms_voltage, jk_bms_current, jk_bms_soc);
                write_offline_log(log_str);
                
                // 解析完畢後清空緩衝區，並再次發送請求，形成不斷更新的迴圈
                jk_buf_len = 0;
                vTaskDelay(pdMS_TO_TICKS(1000)); // 等 1 秒再要下一次資料
                jk_bms_send_request(gattc_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id, gl_profile_tab[PROFILE_A_APP_ID].char_handle);
                break;
            }
        }
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (p_data->write.status == ESP_GATT_OK){
            write_offline_log("[系統] 訂閱成功，發送首次資料請求");
            jk_bms_send_request(gattc_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id, gl_profile_tab[PROFILE_A_APP_ID].char_handle);
        }
        break;
        
    case ESP_GATTC_DISCONNECT_EVT:
        connect = false;
        get_server = false;
        bms_sys_state = 2; // 斷線狀態
        ESP_LOGI(TAG, "與 JK BMS 斷開連接，重新掃描...");
        write_offline_log("[事件] 藍牙斷線");
        esp_ble_gap_start_scanning(30);
        break;
        
    default:
        break;
    }
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(30);
        break;
        
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            bms_sys_state = 0; 
        }
        break;
        
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
        if (scan_result->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            uint8_t *adv_name = NULL;
            uint8_t adv_name_len = 0;
            adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
            
            if (adv_name != NULL) {
                if (strncmp((char *)adv_name, JK_BMS_DEVICE_NAME_PREFIX, strlen(JK_BMS_DEVICE_NAME_PREFIX)) == 0) {
                    if (connect == false) {
                        connect = true;
                        esp_ble_gap_stop_scanning();
                        esp_ble_gattc_open(gl_profile_tab[PROFILE_A_APP_ID].gattc_if, 
                                           scan_result->scan_rst.bda, 
                                           scan_result->scan_rst.ble_addr_type, true);
                    }
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT && param->reg.status == ESP_GATT_OK) {
        gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
    }
    for (int idx = 0; idx < PROFILE_NUM; idx++) {
        if (gattc_if == ESP_GATT_IF_NONE || gattc_if == gl_profile_tab[idx].gattc_if) {
            if (gl_profile_tab[idx].gattc_cb) {
                gl_profile_tab[idx].gattc_cb(event, gattc_if, param);
            }
        }
    }
}

void jk_bms_init(void)
{
    ESP_LOGI(TAG, "啟動 JK BMS 客戶端模組...");

    esp_vfs_spiffs_conf_t spiffs_conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };
    if (esp_vfs_spiffs_register(&spiffs_conf) == ESP_OK) {
        write_offline_log("--- 系統開機 ---");
    }

    xTaskCreate(&led_indicator_task, "led_task", 2048, NULL, 5, NULL);
    esp_ble_gap_register_callback(esp_gap_cb);
    esp_ble_gattc_register_callback(esp_gattc_cb);
    esp_ble_gattc_app_register(PROFILE_A_APP_ID);
    esp_ble_gatt_set_local_mtu(500);
}!= 