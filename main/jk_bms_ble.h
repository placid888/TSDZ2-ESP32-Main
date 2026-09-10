#ifndef _JK_BMS_BLE_H_
#define _JK_BMS_BLE_H_

#include <stdint.h>
#include <stdbool.h>

#define JK_BMS_DEVICE_NAME_PREFIX "JK-"

// 全域變數：儲存從極空板解析出來的精準數據
extern float jk_bms_voltage;
extern float jk_bms_current;
extern uint8_t jk_bms_soc;

// 初始化函數
void jk_bms_init(void);

#endif /* _JK_BMS_BLE_H_ */