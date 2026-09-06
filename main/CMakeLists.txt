# 自動抓取 main 目錄下所有的 .c 檔案
file(GLOB_RECURSE srcs "*.c")

# 自動抓取 main 目錄下所有包含 .h 檔案的資料夾
file(GLOB_RECURSE headers "*.h")
set(include_dirs "")
foreach(header_file ${headers})
    get_filename_component(header_dir ${header_file} DIRECTORY)
    list(APPEND include_dirs ${header_dir})
endforeach()
# 移除重複的資料夾路徑
if(include_dirs)
    list(REMOVE_DUPLICATES include_dirs)
endif()

# 註冊組件並引入所有必要的 ESP-IDF 模組與動態抓取的標頭檔路徑
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." ${include_dirs}
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
