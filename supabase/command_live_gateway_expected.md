# Command live gateway expected result

## 실행 상태

- live apply: 0
- 운영 Supabase·EMQX 변경: 0
- secret·token·credential 출력: 0
- PostgreSQL 17 sanitized rehearsal: 로컬 실행 환경 확인 후 재실행 대기

## 적용 전제

- `public.device_command_state` companion과
  `claim_device_command(text,bigint,bigint,integer)` 존재
- `ack_device_command(text,bigint,smallint,smallint,smallint,bigint,boolean)`
  존재
- `trg_sync_device_command_state` 활성 상태
- `trg_assign_device_command`의 exact `BEFORE INSERT FOR EACH ROW` 정의와
  `assign_device_command()` MD5 일치
- `deviceCmds.status=0` pending legacy 행 0건
- Vault의 `nb_iot_config_request_secret`,
  `nb_iot_emqx_publish_url`, `nb_iot_emqx_publish_key`,
  `nb_iot_emqx_publish_secret` 존재
- 신규 `nb_iot_command_gateway_secret`와 wrapper target 부재

## Up migration 기대 결과

- 기존 `nb_iot_config_request_secret` 값을 DB 내부에서만 복사한
  `nb_iot_command_gateway_secret` Vault alias 생성과 생성 UUID를 포함한
  migration provenance description 고정
- 원본 `nb_iot_config_request_secret` 값·이름·행 변경 0
- `publish_device_command_response(text,bigint,bigint,text) returns bigint`
  생성
- `publish_device_command_ack_receipt(text,bigint,smallint,smallint,smallint,bigint,boolean,text) returns bigint`
  생성
- 두 wrapper 모두 `SECURITY DEFINER`와 empty `search_path` 적용
- IMEI `^[0-9]{10,20}$`, uint32 범위, 필수 boolean·secret 검증
- request 결과 `[request_id,cmd_id,opcode,job_id,ttl_seconds]` 직렬화
- ACK 결과 `[cmd_id,phase,result,receipt,error]` 직렬화
- 두 내부 receipt의 5개 JSON element 모두 numeric type 고정과
  ACK receipt/error semantic 조합 고정
- 두 payload의 `octet_length <= 80` 강제
- 입력 IMEI와 내부 claim/ACK IMEI 및 MQTT topic identity의 단일
  `p_imei` 사용
- `devices/{imei}/cmd/response`와
  `devices/{imei}/cmd/ack/receipt`에 QoS 1, retain false 재발행
- Vault EMQX credential 기반 Basic Authorization과
  `net.http_post(... timeout_milliseconds := 5000)` 사용
- 원문 IMEI·request secret·EMQX credential 로그 출력 0
- wrapper 실행 ACL: `anon`·`service_role` 허용,
  `authenticated`·PUBLIC 거부
- 내부 claim/ACK 실행 ACL: `service_role` 전용 유지
- wrapper overload 0개 기준 생성, 생성 owner·definition MD5·direct ACL
  allowlist provenance marker 고정
- transaction 마지막의 `trg_assign_device_command` 제거
- rollback 자산 `assign_device_command()` 보존
- `trg_sync_device_command_state` 보존
- 두 wrapper의 `bigint` 반환값은 queued request ID only이며
  not HTTP or MQTT delivery success

## Verify 기대 결과

- `nb_iot_command_gateway_secret` 단일 존재와
  `nb_iot_config_request_secret` 값의 DB 내부 equality, UUID-equivalence,
  self-referential provenance description 일치
- 두 wrapper의 exact signature, `bigint` 반환, PL/pgSQL,
  `SECURITY DEFINER`, empty `search_path` 확인
- wrapper owner·definition provenance와 allowlist 외 direct grant 0건,
  내부 claim/ACK ACL 분리 확인
- `trg_assign_device_command` 부재와 `assign_device_command()` MD5 보존
- `trg_sync_device_command_state`의 exact
  `AFTER INSERT OR UPDATE OF status,sent_at,cmd,"deviceId" FOR EACH ROW`
  정의, condition·argument 0건
- 미등록 numeric probe IMEI의 no-command claim 결과
  `[request_id,0,0,0,0]`
- legacy gate SQLSTATE `55000` 미발생
- rollback transaction 안에서 response·ACK wrapper 실제 호출
- 반환된 서로 다른 queued request ID 2건으로
  `net.http_request_queue`의 exact URL·POST·Basic header·5000ms timeout,
  topic·QoS 1·retain false·payload·80-byte limit 확인
- verify transaction rollback에 따른 queue row·probe state 영속 변경 0

## Commit 이후 전달·복구 계약

- `net.http_post()` 반환값의 의미: queued request ID only,
  not HTTP or MQTT delivery success
- 운영 synthetic canary의 queued request ID와
  `net._http_response` 상관관계 확인 및 exact 2xx 응답 전 live enable 금지
- response 외부 전달 실패 시 단말의 다음 command request에 의한 재요청,
  기존 60-second lease 만료 뒤에만 reclaim 허용
- 기존 `delivery_attempts < 5` predicate에 따른 at most five 총
  delivery/redelivery, 5회 도달 뒤 자동 claim 종결
- ACK receipt 외부 전달 실패 시 같은 ACK의 후속 단말 재전송만 허용,
  기존 `ack_device_command()` idempotent receipt 재생으로 상태 전이 중복 0
- wrapper 호출 1회당 queue insert 최대 1건, no autonomous retry와
  비-2xx/timeout의 해당 호출 terminal 처리
- Task 2 신규 no persistent worker, helper function, request tracking schema
  원칙
- Task 2 자체의 eventual HTTP/MQTT delivery 보장 없음과 별도 운영
  재조정·승인 필요

## Rollback 기대 결과

- `sensorvalue` writer lock 획득 뒤 exact trigger 우선 복원

  ```sql
  create trigger trg_assign_device_command
  before insert on public.sensorvalue
  for each row
  execute function public.assign_device_command();
  ```

- trigger 복원 뒤 두 wrapper function 제거
- wrapper definition hash·owner·direct ACL과 alias
  UUID·description·source-value provenance를 먼저 fail-closed 검증
- drift 시 현행 객체 자동 삭제 0과 별도 운영 판단 요구
- provenance가 일치하는 이번 migration 생성
  `nb_iot_command_gateway_secret` alias 정확히 1건 제거
- 원본 `nb_iot_config_request_secret` 변경·삭제 0
- `assign_device_command()`, 내부 claim/ACK,
  `trg_sync_device_command_state`, companion table 보존
- Command companion ACK 이력 변경·삭제 0

## Rehearsal 순서

```text
companion up
→ gateway precheck
→ gateway up
→ gateway verify
→ gateway down
→ legacy trigger verify
```

PostgreSQL 17 전 단계 PASS 전에는 운영 적용 승인 근거로 사용 금지.
운영 live enable 전 synthetic canary의 `_http_response` exact 2xx 확인 필수.
