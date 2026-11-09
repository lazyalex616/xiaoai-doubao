#!/bin/bash
set -e
cmake -S tests/host-client -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j4 --target memory_offline_test identity_fallback_test identity_supersession_test core_redaction_precheck_test memory_continuation_test conversation_policy_test o2_protocol_test o2_client_offline_test half_duplex_offline_test memory_selftest_test
ctest --test-dir build-host --output-on-failure -R '^(memory_offline_test|identity_fallback_test|identity_supersession_test|core_redaction_precheck_test|memory_continuation_test|conversation_policy_test|o2_protocol_test|o2_client_offline_test|half_duplex_offline_test|memory_selftest_test)$'
