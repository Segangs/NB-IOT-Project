# Device command state migration 검토 기준

## 상태

- artifact version: `20260725054445`
- live apply: 0
- Supabase·EMQX·Flask·Spaceship 변경: 0
- 적용 승인: migration diff·rollback·격리 rehearsal 검토 후 별도 승인
- local PostgreSQL rehearsal: 기존 additive baseline PASS, exact 11+11 normalization 변경 재실행 대기
- Supabase branch: 현재 조직의 Pro 미만 플랜 제한으로 생성 0·과금 0

## 적용 전

1. `prechecks/20260725054445_device_command_state_precheck.sql` 실행
2. 모든 `RAISE EXCEPTION` 미발생 확인
3. 운영 DB backup/restore point와 현재 schema artifact SHA-256 재확인
4. EMQX command consumer credential 방식 확정
5. `service_role` 이외 역할에 신규 RPC 노출 금지
6. `deviceCmds`의 `cmd=10,status=1` 정확히 11행과 이에 1:1 연결된 `sensorvalue.cmd=10` 정확히 11행 확인
7. `status=0`인 legacy command 또는 `1..4`·승인된 legacy `10` 이외 opcode 부재 확인
8. 기존 `deviceCmds.cmd=1,status=1` 행과 legacy 대상 `cmdId`에 연결된 기존 `sensorvalue.cmd=1` 행 각각 0건 확인

## up migration 예상 결과

- 기존 `public."deviceCmds"` table·행·sequence·index 보존
- 기존 `public.assign_device_command()`과 `trg_assign_device_command` 보존
- legacy trigger 접근 순서와 같은 `sensorvalue → deviceCmds` 순서의 `SHARE ROW EXCLUSIVE` table lock
- lock 획득 뒤 기존 normalized command 충돌과 legacy 대상의 추가 normalized sensor 행 재검증
- 신규 table 생성 전 같은 transaction에서 연결된 `sensorvalue` 11행과 `deviceCmds` 11행의 `cmd`를 `10 → 1` 순서로 정규화
- 두 legacy table의 update row count가 각각 정확히 11이 아닐 때 전체 transaction rollback
- 신규 table 생성 전 normalized `deviceCmds`와 연결 `sensorvalue`가 각각 정확히 11행이 아니면 전체 transaction rollback
- 신규 `public.device_command_state` companion table 생성
- 기존 `deviceCmds` 모든 행의 companion row 1:1 backfill
- legacy `timestamp without time zone`을 `Asia/Seoul` wall-clock으로 해석한 `timestamptz` 저장
- legacy `status=0`의 24시간 이내 행은 `queued`, `status=1`은 `delivered`
- 생성 후 24시간이 지난 legacy 행은 `expired`
- 신규 legacy 행을 companion table에 복제하는 `trg_sync_device_command_state` 생성
- 기존 `trg_assign_device_command`가 남아 있는 동안 `claim_device_command`는 SQLSTATE `55000`으로 거부
- 한 요청당 최대 1개 claim과 최대 5회 delivery/redelivery
- accepted/final ACK의 동일 cmd·phase·result·error 재시도에 같은 `ingested` receipt 반환
- 재시도 시 device timestamp가 달라도 첫 수신 timestamp를 보존하고 중복으로 수용
- accepted 뒤 동일 request 재시도에도 원 command response 재생
- 신규 table RLS 활성, policy 0, `anon`·`authenticated` table 권한 0
- `claim_device_command`·`ack_device_command`의 `service_role` 전용 실행 권한

## wire 결과

- claim 있음: `[request_id,cmd_id,opcode,job_id,ttl_seconds]`
- claim 없음: `[request_id,0,0,0,0]`
- ACK receipt: `[cmd_id,phase,result,receipt,error]`
- receipt code: `1=ingested`, `2=rejected`, `3=mismatch`
- receipt error: `0=none`, `1=invalid`, `2=unknown_command`, `3=state`, `4=mismatch`
- wire `cmd_id`는 uint32이며 ACK·receipt RPC는 `bigint`으로 수신
- 현재 command 원본인 legacy `deviceCmds.cmdId`는 PostgreSQL `integer`이므로 실제 발급 가능한 값은 `1..2147483647`

## 병행 경로 차단

- 이 up migration은 기존 `assign_device_command()`와 `trg_assign_device_command`를 역사적 legacy 경로로 그대로 보존
- legacy trigger는 companion TTL·accepted ACK를 모르므로 신규 command writer·consumer 활성화 차단
- `claim_device_command` 내부에서 legacy trigger 존재를 확인하고 구조적으로 실행 거부
- legacy telemetry claim reference-zero 확인과 별도 제거 migration 뒤에만 신규 command consumer 활성화
- 신규 destructive command writer도 같은 제거 migration 전에는 활성화 금지

## rollback 예상 결과

- `trg_sync_device_command_state`와 신규 function을 의존성 역순 제거
- 신규 `device_command_state` table 제거
- 기존 `deviceCmds` 행 보존
- 기존 `assign_device_command()`·`trg_assign_device_command` 보존
- 신규 companion 상태 이력 삭제
- rollback은 기존 legacy table의 `10 → 1` 정규화를 되돌리지 않으며 `deviceCmds.cmd=1`·`sensorvalue.cmd=1` 유지

## data-loss 경계

rollback은 기존 command 원본을 삭제하지 않고 `10 → 1` 정규화를 되돌리지 않지만, 신규 companion 상태 이력 삭제를 일으킨다. 여기에는 delivery attempt, lease, accepted/final ACK, device clock metadata가 포함된다. 따라서 live activation 뒤 rollback 전에는 companion table export와 사용자 별도 승인이 필요하다.

## 후속 gate

- 대기: PostgreSQL 17 sanitized exact 11+11 fixture의 precheck→up→verify→claim guard→down→legacy verify rehearsal
- EMQX command rule/action과 credential의 별도 설계·승인
- legacy telemetry command claim reference-zero와 `trg_assign_device_command` 제거 migration
- Flask command writer가 기존 `deviceCmds`와 companion trigger를 통과하는 회귀시험
- 실제 Pico command request/response/accepted/final/receipt E2E
- 14일 legacy telemetry-trigger reference-zero 확인 뒤 기존 command claim 제거 migration 별도 작성
