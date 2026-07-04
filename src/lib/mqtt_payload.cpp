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
