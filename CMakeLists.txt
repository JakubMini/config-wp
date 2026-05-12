cmake_minimum_required(VERSION 3.22)
project(config_wp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Project-wide warning flags live on an INTERFACE library so we can opt our
# own targets in to -Werror without forcing it onto fetched dependencies
# (FreeRTOS-Kernel, GoogleTest) which tend not to compile clean under
# -Wall -Wextra -Wpedantic.
add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

option(BUILD_TESTING "Build unit tests with GoogleTest/GMock" OFF)
option(BUILD_APP "Build the FreeRTOS application binary" ON)

include(FetchContent)

# cJSON for the human-readable JSON import/export layer. Single .c + .h,
# MIT-licensed, ~10 KB ROM on Cortex-M. We disable its tests / examples
# / install targets and only consume the static library.
set(ENABLE_CJSON_TEST       OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS      OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_VERSION_SO OFF CACHE BOOL "" FORCE)
set(ENABLE_PUBLIC_SYMBOLS   ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_SAVE  ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
# cJSON 1.7.x ships an old cmake_minimum_required; suppress the
# modern-CMake rejection for the fetched subproject only.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.18
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cjson)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVE} CACHE BOOL "" FORCE)

if(BUILD_APP)
    set(FREERTOS_PORT GCC_POSIX CACHE STRING "" FORCE)
    set(FREERTOS_HEAP 4 CACHE STRING "" FORCE)

    add_library(freertos_config INTERFACE)
    target_include_directories(freertos_config SYSTEM INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

    FetchContent_Declare(
        freertos_kernel
        GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
        GIT_TAG V11.1.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(freertos_kernel)

    add_subdirectory(src/drivers)
    add_subdirectory(src/application)

    add_executable(config_wp
        src/main.c
        src/config_print.c
        src/external_comms.c
        src/drivers/uart_sim.c
    )
    target_link_libraries(config_wp PRIVATE
        application
        drivers
        freertos_kernel
        pthread
        project_warnings
    )
endif()

if(BUILD_TESTING)
    enable_testing()
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    # Tests build the application code as host-only libraries (no FreeRTOS).
    if(NOT BUILD_APP)
        add_subdirectory(src/drivers)
        add_subdirectory(src/application)
    endif()

    add_subdirectory(tests)
endif()
