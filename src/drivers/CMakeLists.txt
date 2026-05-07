add_library(drivers STATIC
    storage.c
)

target_include_directories(drivers PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)
