# 宣告這是一個 CMake 專案並指定支援的版本
cmake_minimum_required(VERSION 3.16)

# 載入 ESP-IDF 的標準專案設定檔
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 將專案根目錄的 components 資料夾加入編譯搜尋路徑
set(EXTRA_COMPONENT_DIRS "components")

# 定義專案名稱 (編譯出來的執行檔會以此命名)
project(tsdz2_esp32_main)