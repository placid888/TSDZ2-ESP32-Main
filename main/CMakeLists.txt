# 自動抓取 main 目錄下所有的 .c 檔案，避免手動漏打
file(GLOB_RECURSE srcs "*.c")

# 註冊組件並引入所有必要的 ESP-IDF 模組
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES 
        bt                # 藍牙功能
        app_update        # OTA 更新功能
        nvs_flash         # NVS 儲存
        driver            # GPIO, UART 等硬體驅動
        esp_timer         # 定時器功能
        esp_event         # 系統事件循環
        esp_system        # 核心系統功能
        spi_flash         # 快閃記憶體操作
        esp_http_client   # 新增：解決 esp_http_client.h 找不到的問題
)
