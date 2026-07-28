#!/bin/sh
set -eu
umask 077
export MSG_SEND_CLAIM_ENABLED=true
export MSG_SEND_SEND_ENABLED=true

RUNNER="/home/yjijjnuzbr/project/msg_send_current/run_msg_send.sh"
SPOOL_ROOT="/home/yjijjnuzbr/project/msg_send_webhook_spool"

if [ "${MSG_SEND_CALLBACK_APPLY_ENABLED:-false}" = "true" ]; then
    "${RUNNER}" drain-callback-spool --spool-root "${SPOOL_ROOT}"
fi
exec "${RUNNER}" run-once
