/*
 * tsdz_uart.c
 *
 *  Created on: 2 set 2019
 *      Author: Max
 *  Modified for Felt eMTB: UART Pins Hardcoded to GPIO 16/17
 */
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/task.h"
#include "main.h"
#include "tsdz_data.h"
#include "tsdz_uart.h"
#include "tsdz_utils.h"
#include "tsdz_ota_stm8.h"

static const char *TAG = "tsdz_uart";

static uint8_t lcd_recived_msg[LCD_OEM_MSG_BYTES];
static uint8_t lcd_rx_counter = 0;
static uint8_t lcd_state_machine = 0;

static uint8_t ct_received_msg[CT_OS_MSG_BYTES];
static uint8_t ct_rx_counter = 0;
static uint8_t ct_state_machine = 0;

static uint8_t lcd_send_msg[CT_OEM_MSG_BYTES];
static uint8_t ct_send_msg[LCD_OS_MSG_BYTES];

static TickType_t last_read_lcd_tick = 0;
static TickType_t last_read_ct_tick = 0;
static TickType_t last_send_lcd_tick = pdMS_TO_TICKS(20);
static TickType_t last_send_ct_tick = pdMS_TO_TICKS(40);

// ==============================================================================
// 【宣告區】
// ==============================================================================
bool lcdMessageReceived(void);
bool ctMessageReceived(void);
bool checkCRC(uint8_t *message, uint8_t count);
bool checkCRC16(uint8_t *message, uint8_t count);
char* bytesToHex(uint8_t* bytes, uint8_t n);
// ==============================================================================

void tsdz_uart_init(void) {

    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
            .baud_rate = 9600,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    esp_err_t err;
    
    // ==============================================================================
    // 1. 原廠實體儀表 (LCD_UART) 設定區
    // ==============================================================================
    err = uart_param_config(LCD_UART, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config LCD_UART error=%d", err);
    }

    // [腳位封印]：把沒在用的螢幕綁定到 GPIO 32 與 33
    err = uart_set_pin(LCD_UART, 32, 33, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin LCD_UART error=%d", err);
    }
    err = uart_driver_install(LCD_UART, 256, 256, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install LCD_UART error=%d", err);
    }

    // ==============================================================================
    // 2. 馬達控制器 (CT_UART) 設定區 (與 TSDZ2 對接的命脈)
    // ==============================================================================
    err = uart_param_config(CT_UART, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config CT_UART error=%d", err);
    }
    
    // [腳位修改]：RX=16, TX=17 (硬體接線：ESP32 16接馬達TX；17接馬達RX)
    err = uart_set_pin(CT_UART, 16, 17, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin CT_UART error=%d", err);
    }
    err = uart_driver_install(CT_UART, 256, 256, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install CT_UART error=%d", err);
    }
}

// ---------------------------------------------------------
// 下方程式碼皆為原廠通訊邏輯，保持原封不動
// ---------------------------------------------------------
void tsdz_uart_task(void) {
    if (stm8_ota_status > 2) {
        esp_err_t err = uart_wait_tx_done(CT_UART,  pdMS_TO_TICKS(100));
        if (err == ESP_OK) {
            ESP_LOGI(TAG,"STM8 OTA Start!");
            ota_stm8_start();
        } else {
            ESP_LOGE(TAG, "STM8 OTA Aborted. uart_wait_tx_done error=%d", err);
            stm8_ota_status = 0;
        }
    }
	
	TickType_t current_tick = xTaskGetTickCount();
    
    if (lcdMessageReceived()) {
    	if (checkCRC(lcd_recived_msg, LCD_OEM_MSG_BYTES)) {
    		ESP_LOGD(TAG, "LCD Received: %s", bytesToHex(lcd_recived_msg,LCD_OEM_MSG_BYTES));
	        processLcdMessage(lcd_recived_msg);
            getControllerMessage(ct_send_msg);
            uart_write_bytes(CT_UART, (char*)ct_send_msg, (size_t)LCD_OS_MSG_BYTES);
	        last_read_lcd_tick = current_tick;
			last_send_ct_tick = current_tick;
		} else {
			ESP_LOGW(TAG,"LCD-CRC-ERROR: %s", bytesToHex(lcd_recived_msg,LCD_OEM_MSG_BYTES));
	        tsdz_data.ui8_rxl_errors++;
		}
    }
	if ((current_tick - last_send_ct_tick) > pdMS_TO_TICKS(100)) {
		getControllerMessage(ct_send_msg);
		uart_write_bytes(CT_UART, (char*)ct_send_msg, (size_t)LCD_OS_MSG_BYTES);
		last_send_ct_tick = current_tick;
	}

    if (ctMessageReceived()) {
        if (checkCRC16(ct_received_msg, CT_OS_MSG_BYTES)) {
            ESP_LOGD(TAG, "CT Received: %s", bytesToHex(ct_received_msg,CT_OS_MSG_BYTES));
            processControllerMessage(ct_received_msg);
            getLCDMessage(lcd_send_msg);
            uart_write_bytes(LCD_UART, (char*)lcd_send_msg, (size_t)CT_OEM_MSG_BYTES);
	        last_read_ct_tick = current_tick;
			last_send_lcd_tick = current_tick;
        } else {
            ESP_LOGW(TAG,"CONTROLLER-CRC-ERROR %s", bytesToHex(ct_received_msg,CT_OS_MSG_BYTES));
            tsdz_data.ui8_rxc_errors++;
        }
    }
	if ((current_tick - last_send_lcd_tick) > pdMS_TO_TICKS(100)) {
		getLCDMessage(lcd_send_msg);
		uart_write_bytes(LCD_UART, (char*)lcd_send_msg, (size_t)CT_OEM_MSG_BYTES);
		last_send_lcd_tick = current_tick;
	}
	
    if ((current_tick - last_read_ct_tick) > pdMS_TO_TICKS(500))
        tsdz_data.ui8_system_state |= ERROR_CONTROLLER_COMMUNICATION;
    else
        tsdz_data.ui8_system_state &= ~ERROR_CONTROLLER_COMMUNICATION;
    if ((current_tick - last_read_lcd_tick) > pdMS_TO_TICKS(500))
        tsdz_data.ui8_system_state |= ERROR_LCD_COMMUNICATION;
    else
        tsdz_data.ui8_system_state &= ~ERROR_LCD_COMMUNICATION;
}

bool lcdMessageReceived(void) {
    uint8_t byte_received;
    size_t available;
    int received;

    uart_get_buffered_data_len(LCD_UART, &available);
    while (available > 0) {
        received = uart_read_bytes(LCD_UART, &byte_received, 1, 0);
        if (received > 0)
            switch (lcd_state_machine) {
				case 0:
					if (byte_received != LCD_MSG_ID)
						break;
					lcd_rx_counter = 1;
					lcd_state_machine = 1;
					lcd_recived_msg[0] = byte_received;
					break;

				case 1:
					lcd_recived_msg[lcd_rx_counter++] = byte_received;
					if (lcd_rx_counter >= LCD_OEM_MSG_BYTES) {
						lcd_state_machine = 0;
						lcd_rx_counter = 0;
						return true;
					}
					break;
            }
        uart_get_buffered_data_len(LCD_UART, &available);
    }
    return false;
}

bool ctMessageReceived(void) {
    uint8_t byte_received;
    size_t available;
    int received;

    uart_get_buffered_data_len(CT_UART, &available);
    while (available > 0) {
        received = uart_read_bytes(CT_UART, &byte_received, 1, 0);
        if (received > 0)
            switch (ct_state_machine) {
				case 0:
					if (byte_received != CT_MSG_ID) {
						break;
					}
					ct_rx_counter = 1;
					ct_state_machine = 1;
					ct_received_msg[0] = byte_received;
					break;
				case 1:
					ct_received_msg[ct_rx_counter++] = byte_received;
					if (ct_rx_counter >= CT_OS_MSG_BYTES) {
						ct_state_machine = 0;
						ct_rx_counter = 0;
						return true;
					}
					break;
            }
        uart_get_buffered_data_len(CT_UART, &available);
    }
    return false;
}

bool checkCRC(uint8_t *message, uint8_t count) {
    if (crc8(message, count - 1) == message[count - 1])
        return true;
    return false;
}

bool checkCRC16(uint8_t *message, uint8_t count) {
    uint16_t ui16_crc_rx = 0xffff;
    uint8_t ui8_i;

    for (ui8_i = 0; ui8_i < count-2; ui8_i++)
    {
        crc16 (message[ui8_i], &ui16_crc_rx);
    }

    if (((((uint16_t) message [count - 1]) << 8) + ((uint16_t) message [count - 2])) == ui16_crc_rx)
        return true;
    else
        return false;
}

const  char HEX_ARRAY[] = "0123456789ABCDEF";
char sb[256];
char* bytesToHex(uint8_t* bytes, uint8_t n) {
    int i,j;
    for (j = 0, i = 0; j < n; j++) {
        sb[i++] = HEX_ARRAY[bytes[j] >> 4];
        sb[i++] = HEX_ARRAY[bytes[j] & 0x0F];
        sb[i++] = 0x20;
    }
    sb[i++] = 0;
    return sb;
}
