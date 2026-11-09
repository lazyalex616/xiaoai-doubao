cmake_minimum_required(VERSION 3.16)
project(transport_resume_local LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DEPS "${REPO}/build-armv7/_deps" CACHE PATH "Existing pinned dependency sources")
foreach(source ixwebsocket spdlog nlohmann_json)
  if(NOT EXISTS "${DEPS}/${source}-src/CMakeLists.txt")
    message(FATAL_ERROR "Existing pinned dependency missing: ${source}; do not download for this fixture")
  endif()
endforeach()
set(USE_TLS ON CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/ixwebsocket-src" "${CMAKE_BINARY_DIR}/ixwebsocket")
add_subdirectory("${DEPS}/spdlog-src" "${CMAKE_BINARY_DIR}/spdlog")
add_subdirectory("${DEPS}/nlohmann_json-src" "${CMAKE_BINARY_DIR}/nlohmann_json")
set(CLIENT "${REPO}/src/realtime/client.cpp" "${REPO}/src/realtime/daily_usage.cpp"
           "${REPO}/src/realtime/o2_protocol.cpp" "${REPO}/src/realtime/frame_codec.cpp"
           "${REPO}/src/realtime/duplex_protocol.cpp" "${REPO}/src/config/config.cpp")
add_executable(transport_resume_test ../transport_resume_test.cpp ${CLIENT}
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(transport_resume_test PRIVATE "${REPO}/src")
target_link_libraries(transport_resume_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads ZLIB::ZLIB)
# One regression target covers the unchanged strict branch and callback fence.
add_executable(barge_client_offline_test ../barge_client_offline_test.cpp ${CLIENT})
target_include_directories(barge_client_offline_test PRIVATE "${REPO}/src")
target_link_libraries(barge_client_offline_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads ZLIB::ZLIB)
enable_testing()
add_test(NAME transport_resume_test COMMAND transport_resume_test)
add_test(NAME barge_client_offline_test COMMAND barge_client_offline_test)
set_tests_properties(transport_resume_test barge_client_offline_test PROPERTIES TIMEOUT 30)

add_executable(fresh_input_lifecycle_test ../fresh_input_lifecycle_test.cpp ${CLIENT}
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(fresh_input_lifecycle_test PRIVATE "${REPO}/src")
target_link_libraries(fresh_input_lifecycle_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads ZLIB::ZLIB)
add_test(NAME fresh_input_lifecycle_test COMMAND fresh_input_lifecycle_test)
set_tests_properties(fresh_input_lifecycle_test PROPERTIES TIMEOUT 30)

add_executable(fresh_response_usage_test ../fresh_response_usage_test.cpp ${CLIENT}
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(fresh_response_usage_test PRIVATE "${REPO}/src")
target_link_libraries(fresh_response_usage_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads ZLIB::ZLIB)
add_test(NAME fresh_response_usage_test COMMAND fresh_response_usage_test)
set_tests_properties(fresh_response_usage_test PROPERTIES TIMEOUT 30)
