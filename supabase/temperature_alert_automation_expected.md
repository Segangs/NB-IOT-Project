# TEMP 자동 알림 계약

- 기준 입력: `public.sensorvalue`의 신규 TEMP 행
- 최초 정상: 상태 기준선만 저장, 메시지 없음
- 최초 초과 또는 정상→초과: `temperature_high` 1회차 1건
- 초과 지속: 마지막 초과 알림 생성 후 20분마다 최대 3회차
- 3회차 이후 초과 지속: 추가 메시지 없음
- 초과→정상: 같은 incident의 `temperature_recovered` 1건
- 정상 지속: 추가 메시지 없음
- 재초과: 새 incident의 `temperature_high` 1회차 1건
- MIC 및 비정상 숫자: 상태·outbox 변경 없음
- 메시지 dedupe prefix: `temperature:<deviceId>`
- 메시지 dedupe 전체: `temperature:{deviceId}:{userSensorPk}:{incidentId}:{eventKey}:{notificationOrdinal}`
- `msg_send` incident/template 중복 방지: 같은 `source_event_sequence` 재생성 차단, 서로 다른 20분 TEMP telemetry의 2·3회차 허용
- 데이터 저장 trigger: 상태와 outbox만 기록
- 외부 발송: Spaceship worker가 drain RPC로 기존 `msg_send` 경계에 이관
- 발송 채널: Alimtalk 전용, SMS fallback 비활성
- 접근 권한: RLS 활성, service role 전용
