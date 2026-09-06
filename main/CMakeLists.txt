# 自動抓取 main 目錄下所有的 .c 檔案
file(GLOB_RECURSE srcs "*.c")

# 註冊組件並引入所有必要的 ESP-IDF 模組
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES 
        bt
        app_update
        nvs_flash
        driver
        esp_timer
        esp_event
        esp_system
        spi_flash
        esp_http_client
)
