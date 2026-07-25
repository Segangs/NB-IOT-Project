#include "mqtt_payload.hpp"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
    while (p != nullptr && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
    {
        p++;
    }
    return p;
}

static bool parse_finite_json_number(const char *start, const char **end, float *value)
{
    const char *p = start;
    if (*p == '-')
    {
        p++;
    }

    if (*p == '0')
    {
        p++;
    }
    else if (*p >= '1' && *p <= '9')
    {
        do
        {
            p++;
        } while (*p >= '0' && *p <= '9');
    }
    else
    {
        return false;
    }

    if (*p == '.')
    {
        p++;
        if (*p < '0' || *p > '9')
        {
            return false;
        }
        do
        {
            p++;
        } while (*p >= '0' && *p <= '9');
    }

    if (*p == 'e' || *p == 'E')
    {
        p++;
        if (*p == '+' || *p == '-')
        {
            p++;
        }
        if (*p < '0' || *p > '9')
        {
            return false;
        }
        do
        {
            p++;
        } while (*p >= '0' && *p <= '9');
    }

    char *conversion_end = nullptr;
    const float parsed = strtof(start, &conversion_end);
    if (conversion_end != p || !isfinite(parsed))
    {
        return false;
    }

    *end = p;
    *value = parsed;
    return true;
}

static bool copy_trimmed_json(const char *start, char open_ch, char close_ch, char *out, size_t out_len)
{
    const char *end = strrchr(start, close_ch);
    if (end == nullptr || end < start || out_len == 0)
    {
        return false;
    }

    const char *after = skip_ws(end + 1);
    if (after == nullptr || *after != '\0')
    {
        return false;
    }

    if (open_ch == '[')
    {
        const char *inner = skip_ws(start + 1);
        if (inner == nullptr || inner >= end)
        {
            return false;
        }
    }

    size_t len = (size_t)(end - start + 1);
    if (len >= out_len)
    {
        return false;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

bool mqtt_config_payload_extract_object(const char *payload, char *out, size_t out_len)
{
    if (payload == nullptr || out == nullptr || out_len == 0)
    {
        return false;
    }

    out[0] = '\0';
    const char *start = skip_ws(payload);
    if (start == nullptr || *start == '\0')
    {
        return false;
    }

    if (strncmp(start, "undefined", 9) == 0 || strncmp(start, "null", 4) == 0)
    {
        return false;
    }

    if (*start == '{')
    {
        return copy_trimmed_json(start, '{', '}', out, out_len);
    }
    if (*start == '[')
    {
        return copy_trimmed_json(start, '[', ']', out, out_len);
    }

    return false;
}

bool mqtt_config_compact_limits_parse(
    const char *payload,
    float *temp1_upper,
    float *temp2_upper)
{
    if (payload == nullptr || temp1_upper == nullptr || temp2_upper == nullptr)
    {
        return false;
    }

    const char *p = skip_ws(payload);
    if (p == nullptr || *p != '[')
    {
        return false;
    }
    p = skip_ws(p + 1);

    const char *end = nullptr;
    float parsed_temp1 = 0.0f;
    if (!parse_finite_json_number(p, &end, &parsed_temp1))
    {
        return false;
    }

    p = skip_ws(end);
    if (p == nullptr || *p != ',')
    {
        return false;
    }
    p = skip_ws(p + 1);

    float parsed_temp2 = 0.0f;
    if (!parse_finite_json_number(p, &end, &parsed_temp2))
    {
        return false;
    }

    p = skip_ws(end);
    if (p == nullptr || *p != ']')
    {
        return false;
    }
    p = skip_ws(p + 1);
    if (p == nullptr || *p != '\0')
    {
        return false;
    }

    *temp1_upper = parsed_temp1;
    *temp2_upper = parsed_temp2;
    return true;
}

bool mqtt_kmqtt_data_extract_payload(
    const char *frame,
    char *out,
    size_t out_len,
    size_t *frame_len,
    size_t *payload_len)
{
    if (frame_len != nullptr)
    {
        *frame_len = 0;
    }
    if (payload_len != nullptr)
    {
        *payload_len = 0;
    }
    if (frame == nullptr || out == nullptr || out_len < 2)
    {
        return false;
    }
    out[0] = '\0';

    const char *const marker = strstr(frame, "+KMQTT_DATA:");
    if (marker == nullptr)
    {
        return false;
    }
    const char *object = strchr(marker, '{');
    const char *array = strchr(marker, '[');
    const char *start = object;
    if (array != nullptr && (start == nullptr || array < start))
    {
        start = array;
    }
    if (start == nullptr)
    {
        return false;
    }

    char delimiters[64];
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    const char *end = nullptr;
    for (const char *cursor = start; *cursor != '\0'; ++cursor)
    {
        const char value = *cursor;
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (value == '\\')
            {
                escaped = true;
            }
            else if (value == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (value == '"')
        {
            in_string = true;
            continue;
        }
        if (value == '{' || value == '[')
        {
            if (depth >= sizeof(delimiters))
            {
                return false;
            }
            delimiters[depth++] = value;
            continue;
        }
        if (value == '}' || value == ']')
        {
            const char expected = value == '}' ? '{' : '[';
            if (depth == 0 || delimiters[depth - 1] != expected)
            {
                return false;
            }
            --depth;
            if (depth == 0)
            {
                end = cursor;
                break;
            }
        }
    }
    if (end == nullptr || in_string || depth != 0)
    {
        return false;
    }
    if (end[1] != '"' || end[2] != '\r' || end[3] != '\n')
    {
        return false;
    }

    const size_t length = (size_t)(end - start + 1);
    if (length >= out_len)
    {
        return false;
    }
    memcpy(out, start, length);
    out[length] = '\0';
    if (frame_len != nullptr)
    {
        *frame_len = (size_t)((end + 3) - marker + 1);
    }
    if (payload_len != nullptr)
    {
        *payload_len = length;
    }
    return true;
}

bool mqtt_telemetry_payload_build(int sensor_id, float value, char *out, size_t out_len)
{
    if (out == nullptr || out_len == 0 || sensor_id <= 0 || !isfinite(value) || value <= -990.0f)
    {
        return false;
    }

    int written = snprintf(out, out_len, "[%d,%.1f]", sensor_id, value);
    return written > 0 && (size_t)written < out_len;
}

bool mqtt_telemetry_payload_validate(const char *payload, int *sensor_id, float *value)
{
    if (payload == nullptr)
    {
        return false;
    }

    const char *p = skip_ws(payload);
    if (p == nullptr || *p != '[')
    {
        return false;
    }
    p++;

    char *end = nullptr;
    long parsed_id = strtol(p, &end, 10);
    if (end == p || parsed_id <= 0)
    {
        return false;
    }

    p = skip_ws(end);
    if (p == nullptr || *p != ',')
    {
        return false;
    }
    p++;

    p = skip_ws(p);
    if (p == nullptr || strncmp(p, "null", 4) == 0 || *p == '"')
    {
        return false;
    }

    float parsed_value = strtof(p, &end);
    if (end == p || !isfinite(parsed_value) || parsed_value <= -990.0f)
    {
        return false;
    }

    p = skip_ws(end);
    if (p == nullptr || *p != ']')
    {
        return false;
    }
    p = skip_ws(p + 1);
    if (p == nullptr || *p != '\0')
    {
        return false;
    }

    if (sensor_id != nullptr)
    {
        *sensor_id = (int)parsed_id;
    }
    if (value != nullptr)
    {
        *value = parsed_value;
    }
    return true;
}
