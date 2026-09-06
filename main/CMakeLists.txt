# 自動抓取 main 目錄下所有的 .c 檔案
file(GLOB_RECURSE srcs "*.c")

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
)
