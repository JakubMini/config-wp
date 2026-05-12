include(GoogleTest)

add_executable(unit_tests
    test_config.cpp
    test_config_codec.cpp
    test_config_concurrency.cpp
    test_config_json.cpp
    test_crc32.cpp
    test_slot.cpp
    test_storage.cpp
    test_tlv.cpp
    test_types.cpp
)

target_link_libraries(unit_tests PRIVATE
    application
    drivers
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    project_warnings
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

gtest_discover_tests(unit_tests)
