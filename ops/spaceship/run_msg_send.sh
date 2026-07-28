#!/bin/sh
set -eu

PROJECT_ROOT="/home/yjijjnuzbr/project"
RELEASE_ROOT="${PROJECT_ROOT}/msg_send_current"
SECRET_ROOT="${PROJECT_ROOT}/.secrets"

set -a
. "${PROJECT_ROOT}/.env"
set +a

SUPABASE_SECRET_KEY="$(tr -d '\r\n' < "${SECRET_ROOT}/supabase_worker_key")"
BIZPPURIO_SENDERKEY="$(tr -d '\r\n' < "${SECRET_ROOT}/bizppurio_senderkey")"
BIZPPURIO_FROM="$(tr -d '\r\n' < "${SECRET_ROOT}/bizppurio_from")"

export SUPABASE_URL="https://yzorfvgpmkwnjpdfyqsk.supabase.co"
export SUPABASE_SECRET_KEY
export BIZPPURIO_ACCOUNT="${BIZPPURIO_ID:?missing BIZPPURIO_ID}"
export BIZPPURIO_PASSWORD="${BIZPPURIO_PASSWORD:?missing BIZPPURIO_PASSWORD}"
export BIZPPURIO_FROM
export BIZPPURIO_SENDERKEY
export BIZPPURIO_BASE_URL="https://api.bizppurio.com"
export BIZPPURIO_AT_COST_KRW="8.0000"
export BIZPPURIO_SMS_COST_KRW="10.0000"
export MSG_SEND_CLAIM_ENABLED="${MSG_SEND_CLAIM_ENABLED:-false}"
export MSG_SEND_SEND_ENABLED="${MSG_SEND_SEND_ENABLED:-false}"
export MSG_SEND_SMS_FALLBACK_ENABLED="${MSG_SEND_SMS_FALLBACK_ENABLED:-false}"
export MSG_SEND_CALLBACK_APPLY_ENABLED="${MSG_SEND_CALLBACK_APPLY_ENABLED:-false}"
export MSG_SEND_RESULT_MODE="${MSG_SEND_RESULT_MODE:-callback}"
export MSG_SEND_MAX_CONCURRENCY="1"
export MSG_SEND_MAX_MESSAGES_PER_RUN="${MSG_SEND_MAX_MESSAGES_PER_RUN:-20}"
export MSG_SEND_MAX_CYCLE_SECONDS="${MSG_SEND_MAX_CYCLE_SECONDS:-50}"
export MSG_SEND_WORKER_ID="spaceship-msg-send-1"
export MSG_SEND_HTTP_TIMEOUT_SECONDS="10"
export PYTHONPATH="${RELEASE_ROOT}"
export PYTHONDONTWRITEBYTECODE="1"

exec /opt/alt/python311/bin/python3.11 -m msg_send "$@"
