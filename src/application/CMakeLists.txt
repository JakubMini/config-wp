add_library(application STATIC
    config.c
    config_codec.c
    config_defaults.c
    config_json.c
    config_slot.c
    crc32.c
    tlv.c
)

target_include_directories(application PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${cjson_SOURCE_DIR}
)

target_link_libraries(application PUBLIC drivers cjson)
target_link_libraries(application PRIVATE project_warnings)

# Select the right config_lock backing based on whether we're building
# the FreeRTOS app (BUILD_APP) or the host test binary. Same .h, different
# .c, so the manager code is identical across both builds.
if(BUILD_APP)
    target_sources(application PRIVATE config_lock_freertos.c)
    target_link_libraries(application PUBLIC freertos_kernel)
else()
    target_sources(application PRIVATE config_lock_pthread.c)
    find_package(Threads REQUIRED)
    target_link_libraries(application PUBLIC Threads::Threads)
endif()
