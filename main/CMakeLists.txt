# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES nvs_flash bt)
set(COMPONENT_PRIV_REQUIRES esp_timer)

set(COMPONENT_SRCDIRS " .")
set(COMPONENT_ADD_INCLUDEDIRS ".")

register_component()

target_link_libraries(${COMPONENT_TARGET} "-u bt_passkey")
