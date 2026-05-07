add_library(application STATIC
    config.c
)

target_include_directories(application PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(application PUBLIC drivers)

# Only link FreeRTOS into the library when we're building the embedded app.
# When BUILD_TESTING is on (host build), we deliberately skip it so the
# application code can be unit-tested without the kernel.
if(BUILD_APP)
    target_link_libraries(application PUBLIC freertos_kernel)
endif()
