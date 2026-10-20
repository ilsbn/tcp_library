cmake_minimum_required(VERSION 3.30)

# Select vcpkg features before project() runs its manifest installation.
option(BUILD_TESTING "Build automated tests" ON)
if(BUILD_TESTING)
    list(APPEND VCPKG_MANIFEST_FEATURES tests)
    list(REMOVE_DUPLICATES VCPKG_MANIFEST_FEATURES)
endif()

project(tcp_helper VERSION 0.1.0 LANGUAGES CXX)

option(TCP_HELPER_BUILD_EXAMPLES "Build command-line examples and smoke test" ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

find_package(Boost CONFIG REQUIRED COMPONENTS asio crc endian)
find_package(spdlog CONFIG REQUIRED)

add_library(tcp_helper SHARED
    src/data_buffer.cpp
    src/messages.cpp
    src/message_diagnostics.cpp
    src/message_stream_decoder.cpp
    src/typed_events.cpp
    src/tcp_connection.cpp
    src/tcp_client.cpp
    src/tcp_server.cpp)
add_library(tcp_helper::tcp_helper ALIAS tcp_helper)
target_include_directories(tcp_helper PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
target_link_libraries(tcp_helper PUBLIC Boost::asio PRIVATE Boost::crc Boost::endian spdlog::spdlog)
target_compile_definitions(tcp_helper PRIVATE TCP_HELPER_BUILDING_DLL)
if(WIN32)
    target_compile_definitions(tcp_helper PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(tcp_helper PRIVATE ws2_32 mswsock)
endif()

function(tcp_helper_compile_settings target)
    if(MSVC)
        # Explicitly select MSVC's newest mode; do not also request a conflicting /std flag.
        target_compile_options(${target} PUBLIC /std:c++latest /Zc:__cplusplus /permissive-)
        target_compile_options(${target} PRIVATE /W4 /EHsc /utf-8)
    else()
        target_compile_features(${target} PUBLIC cxx_std_23)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
tcp_helper_compile_settings(tcp_helper)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(tcp_helper_tests
        tests/data_buffer_tests.cpp
        tests/messages_tests.cpp
        tests/tcp_tests.cpp
        tests/typed_tcp_tests.cpp
        tests/threaded_send_tests.cpp
        tests/message_channel_tests.cpp
        tests/diagnostics_tests.cpp
        tests/stream_decoder_tests.cpp
        tests/message_corruption_tests.cpp)
    target_include_directories(tcp_helper_tests PRIVATE src)
    target_link_libraries(tcp_helper_tests PRIVATE tcp_helper::tcp_helper GTest::gmock_main)
    tcp_helper_compile_settings(tcp_helper_tests)
    include(GoogleTest)
    gtest_discover_tests(tcp_helper_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 15)
endif()

if(TCP_HELPER_BUILD_EXAMPLES)
    foreach(example IN ITEMS echo_server echo_client smoke_test message_demo_server message_demo_client)
        add_executable(tcp_${example} examples/${example}.cpp)
        target_link_libraries(tcp_${example} PRIVATE tcp_helper::tcp_helper spdlog::spdlog)
        tcp_helper_compile_settings(tcp_${example})
    endforeach()
    if(BUILD_TESTING)
        add_test(NAME tcp_smoke COMMAND tcp_smoke_test)
        set_tests_properties(tcp_smoke PROPERTIES TIMEOUT 15)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME tcp_message_exchange
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_message_demo.py"
                    "$<TARGET_FILE:tcp_message_demo_server>" "$<TARGET_FILE:tcp_message_demo_client>")
            set_tests_properties(tcp_message_exchange PROPERTIES TIMEOUT 25)
            add_test(NAME tcp_command_line_examples
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_examples.py"
                    "$<TARGET_FILE:tcp_echo_server>" "$<TARGET_FILE:tcp_echo_client>")
            set_tests_properties(tcp_command_line_examples PROPERTIES TIMEOUT 25)
        endif()
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS tcp_helper EXPORT tcp_helperTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT tcp_helperTargets NAMESPACE tcp_helper:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tcp_helper)
configure_package_config_file(cmake/tcp_helperConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/tcp_helperConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tcp_helper)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/tcp_helperConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tcp_helperConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/tcp_helperConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tcp_helper)
