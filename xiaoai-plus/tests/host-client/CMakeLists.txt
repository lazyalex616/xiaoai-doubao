cmake_minimum_required(VERSION 3.16)
project(duplex_client_offline_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DEPS "${REPO}/build-armv7/_deps" CACHE PATH "Pinned dependency source cache")
foreach(source ixwebsocket spdlog nlohmann_json)
  if(NOT EXISTS "${DEPS}/${source}-src/CMakeLists.txt")
    message(FATAL_ERROR "Missing pinned ${source} source; configure the ARM build first")
  endif()
endforeach()

# Loopback speech remains ws://; memory HTTPS tests use local TLS.
set(USE_TLS ON CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/ixwebsocket-src" "${CMAKE_BINARY_DIR}/ixwebsocket")
add_subdirectory("${DEPS}/spdlog-src" "${CMAKE_BINARY_DIR}/spdlog")
add_subdirectory("${DEPS}/nlohmann_json-src" "${CMAKE_BINARY_DIR}/nlohmann_json")

add_executable(duplex_client_offline_test
  ../duplex_client_offline_test.cpp
  "${REPO}/src/realtime/client.cpp"
  "${REPO}/src/realtime/daily_usage.cpp"
  "${REPO}/src/realtime/duplex_protocol.cpp"
  "${REPO}/src/config/config.cpp")
target_include_directories(duplex_client_offline_test PRIVATE "${REPO}/src")
target_link_libraries(duplex_client_offline_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json)

add_executable(half_duplex_offline_test
  ../half_duplex_offline_test.cpp
  "${REPO}/src/realtime/client.cpp"
  "${REPO}/src/realtime/daily_usage.cpp"
  "${REPO}/src/realtime/duplex_protocol.cpp"
  "${REPO}/src/config/config.cpp")
target_include_directories(half_duplex_offline_test PRIVATE "${REPO}/src")
target_link_libraries(half_duplex_offline_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json)

add_executable(daily_usage_offline_test
  ../daily_usage_offline_test.cpp "${REPO}/src/realtime/daily_usage.cpp")
target_include_directories(daily_usage_offline_test PRIVATE "${REPO}/src")

add_executable(duplex_protocol_test
  ../duplex_protocol_test.cpp "${REPO}/src/realtime/duplex_protocol.cpp")
target_include_directories(duplex_protocol_test PRIVATE "${REPO}/src")
target_link_libraries(duplex_protocol_test PRIVATE nlohmann_json::nlohmann_json)

add_executable(config_auth_test
  ../config_auth_test.cpp "${REPO}/src/config/config.cpp")
target_include_directories(config_auth_test PRIVATE "${REPO}/src")

add_executable(conversation_policy_test ../conversation_policy_test.cpp)
target_include_directories(conversation_policy_test PRIVATE "${REPO}/src")

add_executable(native_wake_monitor_offline_test
  ../native_wake_monitor_offline_test.cpp
  "${REPO}/src/app/native_wake_monitor.cpp")
target_include_directories(native_wake_monitor_offline_test PRIVATE "${REPO}/src")
target_link_libraries(native_wake_monitor_offline_test PRIVATE Threads::Threads)

add_executable(dual_wake_decider_offline_test
  ../dual_wake_decider_offline_test.cpp
  "${REPO}/src/wakeup/dual_wake_decider.cpp"
  "${REPO}/src/wakeup/trigger.cpp")
target_include_directories(dual_wake_decider_offline_test PRIVATE "${REPO}/src")

add_executable(kws_diagnostics_offline_test
  ../kws_diagnostics_offline_test.cpp
  "${REPO}/src/audio/kws_diagnostics.cpp"
  "${REPO}/src/audio/kws_preprocess.cpp"
  "${REPO}/src/wakeup/trigger.cpp")
target_include_directories(kws_diagnostics_offline_test PRIVATE "${REPO}/src")

add_executable(native_probe_diagnostics_offline_test
  ../native_probe_diagnostics_offline_test.cpp
  "${REPO}/src/native_speech/probe.cpp"
  "${REPO}/src/native_speech/source.cpp")
target_include_directories(native_probe_diagnostics_offline_test PRIVATE "${REPO}/src")
target_link_libraries(native_probe_diagnostics_offline_test PRIVATE Threads::Threads)

add_executable(native_speech_offline_test
  ../native_speech_offline_test.cpp "${REPO}/src/native_speech/source.cpp")
target_include_directories(native_speech_offline_test PRIVATE "${REPO}/src")
target_link_libraries(native_speech_offline_test PRIVATE Threads::Threads)

add_executable(duplex_segmented_offline_test
  ../duplex_segmented_offline_test.cpp
  "${REPO}/src/realtime/client.cpp"
  "${REPO}/src/realtime/daily_usage.cpp"
  "${REPO}/src/realtime/duplex_protocol.cpp"
  "${REPO}/src/config/config.cpp")
target_include_directories(duplex_segmented_offline_test PRIVATE "${REPO}/src")
target_link_libraries(duplex_segmented_offline_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json)

add_executable(native_app_offline_test
  ../native_app_offline_test.cpp
  "${REPO}/src/app/native_app.cpp"
  "${REPO}/src/native_speech/source.cpp"
  "${REPO}/src/audio/player_aplay.cpp"
  "${REPO}/src/realtime/client.cpp"
  "${REPO}/src/realtime/daily_usage.cpp"
  "${REPO}/src/realtime/duplex_protocol.cpp"
  "${REPO}/src/config/config.cpp")
target_include_directories(native_app_offline_test PRIVATE "${REPO}/src")
target_link_libraries(native_app_offline_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json)

add_executable(native_probe_offline_test
  ../native_probe_offline_test.cpp
  "${REPO}/src/native_speech/probe.cpp"
  "${REPO}/src/native_speech/source.cpp")
target_include_directories(native_probe_offline_test PRIVATE "${REPO}/src")
target_link_libraries(native_probe_offline_test PRIVATE Threads::Threads)

add_executable(gate_timeout_offline_test
  ../gate_timeout_offline_test.cpp
  "${REPO}/src/wakeup/gate.cpp")
target_include_directories(gate_timeout_offline_test PRIVATE "${REPO}/src")
target_link_libraries(gate_timeout_offline_test PRIVATE spdlog::spdlog)

add_executable(recorder_stop_offline_test
  ../recorder_stop_offline_test.cpp "${REPO}/src/audio/recorder_arecord.cpp")
target_include_directories(recorder_stop_offline_test PRIVATE "${REPO}/src")
target_link_libraries(recorder_stop_offline_test PRIVATE spdlog::spdlog Threads::Threads)

add_executable(local_listener_offline_test
  ../local_listener_offline_test.cpp
  "${REPO}/src/wakeup/local_listener.cpp"
  "${REPO}/src/wakeup/dual_wake_decider.cpp"
  "${REPO}/src/wakeup/trigger.cpp"
  "${REPO}/src/wakeup/gate.cpp")
target_include_directories(local_listener_offline_test PRIVATE "${REPO}/src")
target_link_libraries(local_listener_offline_test PRIVATE spdlog::spdlog)

add_executable(player_stall_offline_test
  ../player_stall_offline_test.cpp
  "${REPO}/src/audio/player_aplay.cpp")
target_include_directories(player_stall_offline_test PRIVATE "${REPO}/src")
target_link_libraries(player_stall_offline_test PRIVATE spdlog::spdlog)

add_executable(output_format_offline_test ../output_format_offline_test.cpp)
target_include_directories(output_format_offline_test PRIVATE "${REPO}/src")

add_executable(player_large_chunk_offline_test
  ../player_large_chunk_offline_test.cpp "${REPO}/src/audio/player_aplay.cpp")
target_include_directories(player_large_chunk_offline_test PRIVATE "${REPO}/src")
target_link_libraries(player_large_chunk_offline_test PRIVATE spdlog::spdlog)

add_executable(player_sigpipe_failure_offline_test
  ../player_sigpipe_failure_offline_test.cpp
  "${REPO}/src/audio/player_aplay.cpp")
target_compile_definitions(player_sigpipe_failure_offline_test PRIVATE
  XIAOAI_TEST_SIGPIPE_BLOCK_FAILURE=1)
target_include_directories(player_sigpipe_failure_offline_test PRIVATE "${REPO}/src")
target_link_libraries(player_sigpipe_failure_offline_test PRIVATE spdlog::spdlog)

add_executable(kws_agc_offline_test ../kws_agc_offline_test.cpp "${REPO}/src/audio/kws_preprocess.cpp")
target_include_directories(kws_agc_offline_test PRIVATE "${REPO}/src")
add_executable(kws_agc_pcm ../kws_agc_pcm.cpp "${REPO}/src/audio/kws_preprocess.cpp")
target_include_directories(kws_agc_pcm PRIVATE "${REPO}/src")

add_executable(memory_offline_test ../memory_offline_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp"
  "${REPO}/src/realtime/duplex_protocol.cpp")
target_include_directories(memory_offline_test PRIVATE "${REPO}/src")
target_link_libraries(memory_offline_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

add_executable(memory_client_test ../memory_client_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp" "${REPO}/src/realtime/client.cpp"
  "${REPO}/src/realtime/daily_usage.cpp" "${REPO}/src/realtime/duplex_protocol.cpp"
  "${REPO}/src/config/config.cpp")
target_include_directories(memory_client_test PRIVATE "${REPO}/src")
target_link_libraries(memory_client_test PRIVATE ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json)

add_executable(memory_recovery_test ../memory_recovery_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(memory_recovery_test PRIVATE "${REPO}/src")
target_link_libraries(memory_recovery_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

add_executable(memory_e2e ../memory_e2e.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp" "${REPO}/src/memory/http.cpp")
target_include_directories(memory_e2e PRIVATE "${REPO}/src")
target_link_libraries(memory_e2e PRIVATE ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

add_executable(identity_fallback_test ../identity_fallback_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(identity_fallback_test PRIVATE "${REPO}/src")
target_link_libraries(identity_fallback_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

add_executable(identity_supersession_test ../identity_supersession_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(identity_supersession_test PRIVATE "${REPO}/src")
target_link_libraries(identity_supersession_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

add_executable(core_redaction_precheck_test ../core_redaction_precheck_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(core_redaction_precheck_test PRIVATE "${REPO}/src")
target_link_libraries(core_redaction_precheck_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

add_executable(barge_in_controller_test ../barge_in_controller_test.cpp)
target_include_directories(barge_in_controller_test PRIVATE "${REPO}/src")

add_executable(player_preemption_test
  ../player_preemption_test.cpp "${REPO}/src/audio/player_aplay.cpp")
target_include_directories(player_preemption_test PRIVATE "${REPO}/src")
target_link_libraries(player_preemption_test PRIVATE spdlog::spdlog Threads::Threads)

add_executable(barge_client_offline_test
  ../barge_client_offline_test.cpp "${REPO}/src/realtime/client.cpp"
  "${REPO}/src/realtime/daily_usage.cpp" "${REPO}/src/realtime/duplex_protocol.cpp"
  "${REPO}/src/config/config.cpp")
target_include_directories(barge_client_offline_test PRIVATE "${REPO}/src")
target_link_libraries(barge_client_offline_test PRIVATE
  ixwebsocket::ixwebsocket spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads)

add_executable(player_callback_reentry_test
  ../player_callback_reentry_test.cpp "${REPO}/src/audio/player_aplay.cpp")
target_include_directories(player_callback_reentry_test PRIVATE "${REPO}/src")
target_link_libraries(player_callback_reentry_test PRIVATE spdlog::spdlog Threads::Threads)

add_executable(player_completion_fence_test
  ../player_completion_fence_test.cpp "${REPO}/src/audio/player_aplay.cpp")
target_include_directories(player_completion_fence_test PRIVATE "${REPO}/src")
target_link_libraries(player_completion_fence_test PRIVATE spdlog::spdlog Threads::Threads)

enable_testing()
get_property(host_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(t IN LISTS host_targets)
  if(t MATCHES "_test$")
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 90)
  endif()
endforeach()

add_executable(memory_selftest_test ../memory_selftest_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp" "${REPO}/src/memory/selftest.cpp")
target_include_directories(memory_selftest_test PRIVATE "${REPO}/src")
target_link_libraries(memory_selftest_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME memory_selftest_test COMMAND memory_selftest_test)
set_tests_properties(memory_selftest_test PROPERTIES TIMEOUT 45)

add_executable(memory_prefetch_test ../memory_prefetch_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(memory_prefetch_test PRIVATE "${REPO}/src")
target_link_libraries(memory_prefetch_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME memory_prefetch_test COMMAND memory_prefetch_test)
set_tests_properties(memory_prefetch_test PROPERTIES TIMEOUT 30)

add_executable(memory_continuation_test ../memory_continuation_test.cpp
  "${REPO}/src/memory/core.cpp" "${REPO}/src/memory/service.cpp")
target_include_directories(memory_continuation_test PRIVATE "${REPO}/src")
target_link_libraries(memory_continuation_test PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
add_test(NAME memory_continuation_test COMMAND memory_continuation_test)

add_executable(memory_https_test ../memory_https_test.cpp "${REPO}/src/memory/http.cpp")
target_include_directories(memory_https_test PRIVATE "${REPO}/src")
target_link_libraries(memory_https_test PRIVATE ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
add_executable(memory_legacy_health_probe "${REPO}/toolchain/memory-health-probe.cpp" "${REPO}/src/config/config.cpp")
target_include_directories(memory_legacy_health_probe PRIVATE "${REPO}/src")
target_link_libraries(memory_legacy_health_probe PRIVATE ixwebsocket::ixwebsocket Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(memory_https_test PRIVATE -Wl,--wrap=getaddrinfo)
endif()
add_test(NAME memory_https_test COMMAND python3 "${REPO}/tests/memory_https_fixture.py" $<TARGET_FILE:memory_https_test> $<TARGET_FILE:memory_legacy_health_probe>)
set_tests_properties(memory_https_test PROPERTIES TIMEOUT 60)

add_executable(exit_echo_guard_test ../exit_echo_guard_test.cpp)
target_include_directories(exit_echo_guard_test PRIVATE "${REPO}/src")
add_test(NAME exit_echo_guard_test COMMAND exit_echo_guard_test)

# O2 uses the same Client callbacks in every host regression target.
find_package(ZLIB REQUIRED)
get_property(all_host_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(t IN LISTS all_host_targets)
  get_target_property(sources ${t} SOURCES)
  if("${sources}" MATCHES "realtime/client.cpp")
    if(NOT "${sources}" MATCHES "memory/core.cpp")
      target_sources(${t} PRIVATE "${REPO}/src/memory/core.cpp")
    endif()
    target_sources(${t} PRIVATE "${REPO}/src/realtime/o2_protocol.cpp" "${REPO}/src/realtime/frame_codec.cpp")
    target_link_libraries(${t} PRIVATE ZLIB::ZLIB)
  endif()
endforeach()
add_executable(o2_protocol_test ../o2_protocol_test.cpp
  "${REPO}/src/realtime/o2_protocol.cpp" "${REPO}/src/realtime/frame_codec.cpp"
  "${REPO}/src/realtime/duplex_protocol.cpp")
target_include_directories(o2_protocol_test PRIVATE "${REPO}/src")
target_link_libraries(o2_protocol_test PRIVATE nlohmann_json::nlohmann_json spdlog::spdlog ZLIB::ZLIB)
add_test(NAME o2_protocol_test COMMAND o2_protocol_test)

add_executable(o2_client_offline_test ../o2_client_offline_test.cpp "${REPO}/src/memory/core.cpp"
  "${REPO}/src/realtime/client.cpp" "${REPO}/src/realtime/daily_usage.cpp"
  "${REPO}/src/realtime/duplex_protocol.cpp" "${REPO}/src/realtime/o2_protocol.cpp"
  "${REPO}/src/realtime/frame_codec.cpp" "${REPO}/src/config/config.cpp")
target_include_directories(o2_client_offline_test PRIVATE "${REPO}/src")
target_link_libraries(o2_client_offline_test PRIVATE ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json spdlog::spdlog ZLIB::ZLIB Threads::Threads)
add_test(NAME o2_client_offline_test COMMAND o2_client_offline_test)

add_executable(volume_control_test ../volume_control_test.cpp)
target_include_directories(volume_control_test PRIVATE "${REPO}/src")
add_test(NAME volume_control_test COMMAND volume_control_test)

add_executable(player_audible_drain_test ../player_audible_drain_test.cpp "${REPO}/src/audio/player_aplay.cpp")
target_include_directories(player_audible_drain_test PRIVATE "${REPO}/src")
target_link_libraries(player_audible_drain_test PRIVATE spdlog::spdlog Threads::Threads)
add_test(NAME player_audible_drain_test COMMAND player_audible_drain_test)
