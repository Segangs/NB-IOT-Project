function(nb_iot_load_firmware_env env_file output_var)
    set(allowed_keys
        APN_NAME
        MQTT_BROKER_HOST
        MQTT_BROKER_PORT
    )
    set(definitions)

    if(NOT EXISTS "${env_file}")
        set(${output_var} "" PARENT_SCOPE)
        return()
    endif()

    foreach(key IN LISTS allowed_keys)
        file(STRINGS "${env_file}" matching_lines
            REGEX "^[ \t]*${key}[ \t]*=")
        list(LENGTH matching_lines matching_count)

        if(matching_count GREATER 1)
            message(FATAL_ERROR
                "Duplicate firmware configuration key: ${key}")
        endif()
        if(matching_count EQUAL 0)
            continue()
        endif()

        list(GET matching_lines 0 matching_line)
        string(REGEX REPLACE
            "^[ \t]*${key}[ \t]*=[ \t]*"
            ""
            value
            "${matching_line}")
        string(STRIP "${value}" value)

        if(value MATCHES "^\"(.*)\"$")
            set(value "${CMAKE_MATCH_1}")
        elseif(value MATCHES "^'(.*)'$")
            set(value "${CMAKE_MATCH_1}")
        endif()

        if(key STREQUAL "APN_NAME")
            if(NOT value MATCHES "^[A-Za-z0-9._-]+$")
                message(FATAL_ERROR
                    "Invalid firmware configuration value for key: ${key}")
            endif()
        elseif(key STREQUAL "MQTT_BROKER_HOST")
            if(NOT value MATCHES "^[A-Za-z0-9.-]+$")
                message(FATAL_ERROR
                    "Invalid firmware configuration value for key: ${key}")
            endif()
        elseif(key STREQUAL "MQTT_BROKER_PORT")
            if(NOT value MATCHES "^[0-9]+$")
                message(FATAL_ERROR
                    "Invalid firmware configuration value for key: ${key}")
            endif()
            if(value LESS 1 OR value GREATER 65535)
                message(FATAL_ERROR
                    "Invalid firmware configuration value for key: ${key}")
            endif()
        endif()

        list(APPEND definitions "${key}=\"${value}\"")
    endforeach()

    set(${output_var} "${definitions}" PARENT_SCOPE)
endfunction()
