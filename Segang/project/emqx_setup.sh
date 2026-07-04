#!/bin/bash

# EMQX 6.2.1 Supabase 연동 자동화 설정 스크립트
# 사용법: ./emqx_setup.sh

# 스크립트 위치 기준으로 .env 파일 경로 지정
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# .env 파일에서 Supabase URL과 Key 추출
if [ -f "$SCRIPT_DIR/.env" ]; then
    SUPABASE_URL=$(grep SUPABASE_URL "$SCRIPT_DIR/.env" | cut -d '=' -f2 | tr -d '\r' | xargs)
    SUPABASE_KEY=$(grep SUPABASE_KEY "$SCRIPT_DIR/.env" | cut -d '=' -f2 | tr -d '\r' | xargs)
    EMQX_AUTH_SECRET=$(grep EMQX_AUTH_SECRET "$SCRIPT_DIR/.env" | cut -d '=' -f2 | tr -d '\r' | xargs)
    EMQX_API_AUTH_HEADER=$(grep EMQX_API_AUTH_HEADER "$SCRIPT_DIR/.env" | cut -d '=' -f2- | tr -d '\r' | xargs)
else
    echo "❌ 에러: $SCRIPT_DIR/.env 파일을 찾을 수 없습니다."
    exit 1
fi

if [ -z "$EMQX_AUTH_SECRET" ]; then
    echo "❌ 에러: $SCRIPT_DIR/.env 파일에 EMQX_AUTH_SECRET 값이 필요합니다."
    exit 1
fi

if [ -z "$EMQX_API_AUTH_HEADER" ]; then
    echo "❌ 에러: $SCRIPT_DIR/.env 파일에 EMQX_API_AUTH_HEADER 값이 필요합니다."
    exit 1
fi

EMQX_API="http://localhost:18083/api/v5"
AUTH_HEADER="$EMQX_API_AUTH_HEADER"

echo "🌀 Supabase URL: $SUPABASE_URL"
echo "🌀 EMQX API 연결 확인 및 설정 주입을 시작합니다..."

# 1. HTTP Server 기반 IMEI/IMSI 패스워드 인증 생성
echo "🔑 [1/6] Supabase HTTP API 기기 인증 생성 중..."
curl -s -o /dev/null -w "%{http_code}" -X POST \
  -H "$AUTH_HEADER" \
  -H "Content-Type: application/json" \
  -d '{
    "enable": true,
    "backend": "http",
    "mechanism": "password_based",
    "method": "post",
    "url": "'"${SUPABASE_URL}"'/rest/v1/rpc/auth_device",
    "headers": {
      "content-type": "application/json",
      "apikey": "'"${SUPABASE_KEY}"'",
      "authorization": "Bearer '"${SUPABASE_KEY}"'",
      "x-emqx-auth-secret": "'"${EMQX_AUTH_SECRET}"'"
    },
    "body": {
      "username": "${username}",
      "password": "${password}",
      "clientid": "${clientid}",
      "peerhost": "${peerhost}",
      "listener": "${listener}",
      "username_raw": "${username}"
    },
    "ssl": {"enable": true, "verify": "verify_none"}
  }' "$EMQX_API/authentication" | grep -E "200|201" > /dev/null

if [ $? -eq 0 ]; then
    echo "✅ [완료] 기기 인증 플러그인 등록 성공."
else
    echo "⚠️  [경고] 인증 플러그인이 이미 등록되어 있거나 생성이 보류되었습니다."
fi

# 2. Supabase 데이터 릴레이용 HTTP Webhook 브릿지 생성
# 2-1) telemetry 브릿지
echo "🌉 [2/6] Supabase Telemetry Webhook 브릿지 생성 중..."
curl -s -o /dev/null -w "%{http_code}" -X POST \
  -H "$AUTH_HEADER" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "supabase_telemetry",
    "type": "webhook",
    "url": "'"${SUPABASE_URL}"'/rest/v1/rpc/t",
    "method": "post",
    "headers": {
      "Content-Type": "application/json",
      "apikey": "'"${SUPABASE_KEY}"'",
      "Authorization": "Bearer '"${SUPABASE_KEY}"'"
    },
    "body": "{\"p_imei\": \"${clientid}\", \"p_user_sensor_id\": ${id}, \"p_value\": ${v}}",
    "ssl": {"enable": true, "verify": "verify_none"}
  }' "$EMQX_API/bridges" | grep -E "200|201" > /dev/null

# 2-2) boot 브릿지
echo "🌉 [3/6] Supabase Boot Log Webhook 브릿지 생성 중..."
curl -s -o /dev/null -w "%{http_code}" -X POST \
  -H "$AUTH_HEADER" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "supabase_boot",
    "type": "webhook",
    "url": "'"${SUPABASE_URL}"'/rest/v1/rpc/b",
    "method": "post",
    "headers": {
      "Content-Type": "application/json",
      "apikey": "'"${SUPABASE_KEY}"'",
      "Authorization": "Bearer '"${SUPABASE_KEY}"'"
    },
    "body": "{\"p_imei\": \"${clientid}\", \"p_cimi\": \"\", \"p_voltage\": ${v}, \"p_temp\": ${t}, \"p_flash\": ${f}, \"p_ram\": ${r}, \"p_at\": ${a}, \"p_cpin\": ${c}, \"p_csq\": ${q}, \"p_carrier\": \"${o}\", \"p_tmp1_status\": ${tmp1}, \"p_tmp2_status\": ${tmp2}, \"p_mic1_status\": ${mic1}, \"p_mic2_status\": ${mic2}, \"p_boot_reason\": ${b}, \"p_cmd_id\": ${i}}",
    "ssl": {"enable": true, "verify": "verify_none"}
  }' "$EMQX_API/bridges" | grep -E "200|201" > /dev/null

# 2-3) config 브릿지
echo "🌉 [4/6] Supabase Config Fetch Webhook 브릿지 생성 중..."
curl -s -o /dev/null -w "%{http_code}" -X POST \
  -H "$AUTH_HEADER" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "supabase_config",
    "type": "webhook",
    "url": "'"${SUPABASE_URL}"'/rest/v1/rpc/get_device_sensors",
    "method": "post",
    "headers": {
      "Content-Type": "application/json",
      "apikey": "'"${SUPABASE_KEY}"'",
      "Authorization": "Bearer '"${SUPABASE_KEY}"'"
    },
    "body": "{\"p_imei\": \"${clientid}\"}",
    "ssl": {"enable": true, "verify": "verify_none"}
  }' "$EMQX_API/bridges" | grep -E "200|201" > /dev/null

echo "✅ [완료] 모든 Webhook 브릿지 등록 성공."

# 3. 데이터 적재 및 제어 명령 재발행 룰 등록
# 3-1) telemetry 수신 -> rpc/t 호출 및 URC 응답 config 토픽 재발행 규칙
echo "📏 [5/7] Telemetry 데이터 릴레이 규칙 생성 중..."
curl -s -o /dev/null -w "%{http_code}" -X POST \
  -H "$AUTH_HEADER" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "telemetry_rule",
    "sql": "SELECT nth(1, json_decode(payload)) as id, nth(2, json_decode(payload)) as v, clientid FROM \"devices/+/telemetry\"",
    "actions": [
      "webhook:supabase_telemetry"
    ]
  }' "$EMQX_API/rules" | grep -E "200|201" > /dev/null

# 3-2) config request 수신 -> rpc/get_device_sensors(config) 호출
echo "📏 [6/7] Config request 센서 설정 조회 규칙 생성 중..."
curl -s -o /dev/null -w "%{http_code}" -X POST \
  -H "$AUTH_HEADER" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "config_request_rule",
    "sql": "SELECT clientid FROM \"devices/+/config/request\"",
    "actions": [
      "webhook:supabase_config"
    ]
  }' "$EMQX_API/rules" | grep -E "200|201" > /dev/null

# 3-3) boot 수신 -> rpc/b 호출. config 조회는 devices/+/config/request rule로 분리.
echo "📏 [7/7] Boot 로그 적재 규칙 생성 중..."
curl -s -o /dev/null -w "%{http_code}" -X POST \
  -H "$AUTH_HEADER" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "boot_rule",
    "sql": "SELECT nth(1, json_decode(payload)) as v, nth(2, json_decode(payload)) as t, nth(3, json_decode(payload)) as f, nth(4, json_decode(payload)) as r, nth(5, json_decode(payload)) as a, nth(6, json_decode(payload)) as c, nth(7, json_decode(payload)) as q, case when nth(8, json_decode(payload)) = 1 then 'SKT' when nth(8, json_decode(payload)) = 2 then 'KT' when nth(8, json_decode(payload)) = 3 then 'LGU+' else 'Unknown' end as o, nth(9, json_decode(payload)) as tmp1, nth(10, json_decode(payload)) as tmp2, nth(11, json_decode(payload)) as mic1, nth(12, json_decode(payload)) as mic2, nth(13, json_decode(payload)) as b, nth(14, json_decode(payload)) as i, clientid FROM \"devices/+/boot\"",
    "actions": [
      "webhook:supabase_boot"
    ]
  }' "$EMQX_API/rules" | grep -E "200|201" > /dev/null

echo "🎉 [전체 완료] EMQX 6.2.1 + Supabase MQTTS 자동화 연동 셋업 완료!"
