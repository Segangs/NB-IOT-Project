cmake_minimum_required(VERSION 3.16)

set(test_directory "${CMAKE_CURRENT_BINARY_DIR}/check_artifact_size_test")
set(checker "${CMAKE_CURRENT_LIST_DIR}/../../cmake/check_artifact_size.cmake")
set(project_root "${CMAKE_CURRENT_LIST_DIR}/../..")
set(root_cmake "${project_root}/CMakeLists.txt")
set(max_bytes 17)

function(expect_checker_failure case_name expected_pattern)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${checker}"
        RESULT_VARIABLE checker_result
        OUTPUT_VARIABLE checker_output
        ERROR_VARIABLE checker_error
    )
    if(checker_result EQUAL 0)
        message(FATAL_ERROR "${case_name} must fail")
    endif()
    if(NOT "${checker_output}${checker_error}" MATCHES "${expected_pattern}")
        message(FATAL_ERROR
            "${case_name} must report '${expected_pattern}': "
            "${checker_output}${checker_error}")
    endif()
endfunction()

file(REMOVE_RECURSE "${test_directory}")
file(MAKE_DIRECTORY "${test_directory}")

string(REPEAT "x" ${max_bytes} exact_limit_contents)
file(WRITE "${test_directory}/exact-limit.bin" "${exact_limit_contents}")
file(WRITE "${test_directory}/limit-plus-one.bin" "${exact_limit_contents}x")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT_FILE=${test_directory}/exact-limit.bin"
        "-DMAX_BYTES=${max_bytes}"
        -P "${checker}"
    RESULT_VARIABLE exact_limit_result
    OUTPUT_VARIABLE exact_limit_output
    ERROR_VARIABLE exact_limit_error
)
if(NOT exact_limit_result EQUAL 0)
    message(FATAL_ERROR
        "exact limit artifact must pass: ${exact_limit_output}${exact_limit_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT_FILE=${test_directory}/limit-plus-one.bin"
        "-DMAX_BYTES=${max_bytes}"
        -P "${checker}"
    RESULT_VARIABLE limit_plus_one_result
    OUTPUT_VARIABLE limit_plus_one_output
    ERROR_VARIABLE limit_plus_one_error
)
if(limit_plus_one_result EQUAL 0)
    message(FATAL_ERROR "limit plus one artifact must fail")
endif()
if(NOT "${limit_plus_one_output}${limit_plus_one_error}" MATCHES
        "exceeds firmware slot")
    message(FATAL_ERROR
        "limit plus one artifact must identify the firmware-slot overflow: "
        "${limit_plus_one_output}${limit_plus_one_error}")
endif()

expect_checker_failure(
    "missing INPUT_FILE"
    "INPUT_FILE is required"
    "-DMAX_BYTES=${max_bytes}"
)

expect_checker_failure(
    "empty INPUT_FILE"
    "INPUT_FILE is required"
    "-DINPUT_FILE="
    "-DMAX_BYTES=${max_bytes}"
)

expect_checker_failure(
    "missing artifact file"
    "Firmware artifact does not exist"
    "-DINPUT_FILE=${test_directory}/missing.bin"
    "-DMAX_BYTES=${max_bytes}"
)

expect_checker_failure(
    "missing MAX_BYTES"
    "MAX_BYTES must be a positive integer"
    "-DINPUT_FILE=${test_directory}/exact-limit.bin"
)

expect_checker_failure(
    "empty MAX_BYTES"
    "MAX_BYTES must be a positive integer"
    "-DINPUT_FILE=${test_directory}/exact-limit.bin"
    "-DMAX_BYTES="
)

expect_checker_failure(
    "invalid MAX_BYTES"
    "MAX_BYTES must be a positive integer"
    "-DINPUT_FILE=${test_directory}/exact-limit.bin"
    "-DMAX_BYTES=not-a-number"
)

# The post-build gate must derive its limit from precisely one authoritative
# declaration.  A stale comment or a second declaration must not silently
# select whichever match happens to appear first.
set(firmware_a_declaration_semicolon "__NB_IOT_FIRMWARE_A_DECLARATION_SEMICOLON__")
set(firmware_a_declaration_regex
    "inline[ ]+constexpr[ ]+std::uint32_t[ ]+firmware_a_size[ ]*=[ ]*(0x[0-9A-Fa-f]+)u[ ]*${firmware_a_declaration_semicolon}")

set(canonical_firmware_a_declaration
    "inline constexpr std::uint32_t firmware_a_size = 0x140000u;")
set(stale_comment_fixture
    "// stale firmware_a_size = 0x010000u;\n${canonical_firmware_a_declaration}")
string(REPLACE ";" "${firmware_a_declaration_semicolon}"
    stale_comment_fixture_normalized "${stale_comment_fixture}")
string(REGEX MATCHALL "${firmware_a_declaration_regex}"
    stale_comment_matches "${stale_comment_fixture_normalized}")
list(LENGTH stale_comment_matches stale_comment_match_count)
if(NOT stale_comment_match_count EQUAL 1)
    message(FATAL_ERROR
        "stale comment fixture must yield exactly one firmware_a_size declaration")
endif()

set(duplicate_declaration_fixture
    "${canonical_firmware_a_declaration}\n${canonical_firmware_a_declaration}")
string(REPLACE ";" "${firmware_a_declaration_semicolon}"
    duplicate_declaration_fixture_normalized "${duplicate_declaration_fixture}")
string(REGEX MATCHALL "${firmware_a_declaration_regex}"
    duplicate_declaration_matches "${duplicate_declaration_fixture_normalized}")
list(LENGTH duplicate_declaration_matches duplicate_declaration_match_count)
if(NOT duplicate_declaration_match_count EQUAL 2)
    message(FATAL_ERROR
        "duplicate declaration fixture must expose both firmware_a_size declarations")
endif()

set(wrong_declaration_fixture
    "constexpr std::uint32_t firmware_a_size = 0x140000u;")
string(REPLACE ";" "${firmware_a_declaration_semicolon}"
    wrong_declaration_fixture_normalized "${wrong_declaration_fixture}")
string(REGEX MATCHALL "${firmware_a_declaration_regex}"
    wrong_declaration_matches "${wrong_declaration_fixture_normalized}")
list(LENGTH wrong_declaration_matches wrong_declaration_match_count)
if(NOT wrong_declaration_match_count EQUAL 0)
    message(FATAL_ERROR
        "non-inline firmware_a_size declaration must not satisfy the parser")
endif()

file(READ "${root_cmake}" root_cmake_source)
foreach(required_root_parser_fragment
        "string(REGEX MATCHALL"
        "firmware_a_declaration_regex"
        "firmware_slot_size_matches"
        "firmware_slot_size_match_count"
        "firmware_slot_size_match_count EQUAL 1")
    string(FIND "${root_cmake_source}" "${required_root_parser_fragment}"
        root_parser_fragment_index)
    if(root_parser_fragment_index EQUAL -1)
        message(FATAL_ERROR
            "root firmware slot parser is missing: ${required_root_parser_fragment}")
    endif()
endforeach()
