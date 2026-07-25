if(NOT DEFINED INPUT_FILE OR INPUT_FILE STREQUAL "")
    message(FATAL_ERROR "INPUT_FILE is required")
endif()

if(NOT EXISTS "${INPUT_FILE}")
    message(FATAL_ERROR "Firmware artifact does not exist: ${INPUT_FILE}")
endif()

if(NOT DEFINED MAX_BYTES OR NOT MAX_BYTES MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "MAX_BYTES must be a positive integer")
endif()

file(READ "${INPUT_FILE}" artifact_hex HEX)
string(LENGTH "${artifact_hex}" artifact_hex_length)
math(EXPR artifact_size "${artifact_hex_length} / 2")

if(artifact_size GREATER MAX_BYTES)
    message(FATAL_ERROR
        "Firmware artifact is ${artifact_size} bytes; exceeds firmware slot "
        "(${MAX_BYTES} bytes): ${INPUT_FILE}")
endif()

message(STATUS
    "Firmware artifact size: ${artifact_size} bytes (max ${MAX_BYTES} bytes)")
