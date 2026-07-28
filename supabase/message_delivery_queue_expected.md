# Message delivery queue migration 검토 기준

## 상태

- artifact version: `20260726005145`
- live apply: 0
- 기존 object 변경: 0
- Supabase branch·EMQX·Flask·Spaceship·provider 변경: 0
- 적용 승인: migration·rollback·격리 rehearsal 검토 후 별도 승인

## 적용 전

1. `prechecks/20260726005145_message_delivery_queue_precheck.sql` 실행
2. PostgreSQL 15 이상, `public.device`, `public.users`, `service_role` 확인
3. 신규 table·RPC 이름 충돌 0 확인
4. backup/restore point, 수신 동의, 승인 template/profile, 비용 cap 재확인
5. 운영 credential·전화번호·opaque token 원문을 검토 산출물에 기록 금지

## up migration 예상 결과

- lowercase 신규 table `alert_contact`, `message_policy`, `msg_send`,
  `message_link_token` 생성
- 기존 table·function·trigger·RLS·grant 변경 0
- 모든 업무 timestamp의 `timestamptz` 저장
- contact provenance와 명시적 수신 동의·철회 시각 기록
- source user/device/event/sequence와 power incident/template stage provenance 기록
- 기존 `users`·`device` FK 미생성 및 parent delete/trigger dependency 0
- `user_id`·`source_user_id`·`source_device_id`의 typed provenance 저장,
  queue insert 전 application-level 존재·소유 관계 검증
- `dedupe_key` unique 및 device·incident·template stage·contact unique 계약
- canonical queue 상태
  `pending → processing → accepted → waiting_result → sent`
- 재시도 상태 `retry_wait`, terminal 상태
  `sent`·`failed`·`suppressed`·`cancelled`
- `FOR UPDATE SKIP LOCKED`와 단일 UPDATE 기반 atomic bounded lease claim
- 전체 active queue를 잠그고 cardinality·expected ID·attempt 1·활성
  no-fallback 정책을 함께 검사하는 atomic exact one-shot claim
- claim별 UUID fence·monotonic generation 발급 및 제출 시작·완료 전이의
  owner/token/deadline 검증
- provider 호출 전 `submission start`의 active channel·attempt token·
  DB `clock_timestamp()` 기반 started_at 원자 저장, 저장 실패 시 provider 호출 0
- request 렌더링·access token 획득 등 message transport 호출 전 확정 실패의
  bounded `retry_wait`, transport 호출 후 예외의 durable ambiguity 보존
- accepted 응답 등록 실패·provider 응답 유실 시 기존 submission token 보존,
  lease 만료 뒤 신규 token 회수·재발송 0
- bounded `recover_msg_send_leases`의 만료 `processing` 재시도,
  만료 `pending`·`retry_wait`와 결과 대기 상한 초과 `waiting_result` 종결,
  제출 시작 상한 초과 `accepted`의
  `provider_submission_ambiguous_timeout` 종결
- 알림톡 접수와 최종 PUSH result 분리 저장
- provider request ID별 unique index와 제출 lease token 기반 callback fence
- durable submission token의 provider `message_key` 사용 및 submit 반환 전·
  claim lease 만료 후 PUSH callback의 atomic correlation
- 동일 submission request/token 등록 재호출은 fast callback이
  `sent`·`failed`·`retry_wait`·새 fenced `processing`으로 전진한 뒤에도
  현재 row를 `applied=false`, `duplicate=true`로 반환하고 상태를 미변경
- 동일 provider PUSH result 재수신 시
  `applied=false`, `duplicate=true`, `action=duplicate` 반환
- callback action
  `delivered`·`retry_scheduled`·`sms_fallback`·`failed`·`unknown`의
  명시적 typed 반환
- retryable failure는 budget·expiry 범위에서 `retry_wait` 우선 전환
- SMS fallback은 정책 허용·primary failure·expiry 여유 조건에서 새 UUID
  fence와 유효 lease를 가진 `processing` row로 원자 전환
- callback 비용의 `numeric(14,4)` 범위·scale 검증 및 단일 canonical 값 사용
- 성공 channel, fallback reason, provider request/result, cost class·금액 기록
- `accepted`·`waiting_result`의 channel별 submission identity, lease 상태,
  delivery 결과의 NULL-safe CHECK 계약
- `accepted` 제출 모호성과 `waiting_result`를 함께 반영한 provider lag health
- link token 원문 미저장 및 SHA-256 hash만 저장

## 보안 예상 결과

- 신규 public table 4개 RLS 활성, policy 0
- `PUBLIC`·`anon`·`authenticated` table·sequence 권한 0
- `service_role`의 `msg_send` 직접 권한은 `SELECT`·`INSERT`만 허용하고,
  나머지 신규 table은 `SELECT`·`INSERT`·`UPDATE` 허용
- 모든 신규 identity sequence는 `USAGE`·`SELECT`만 허용
- worker RPC `claim_msg_send`,
  `claim_exact_one_shot_msg_send`,
  `mark_msg_send_submission_started`,
  `mark_msg_send_submission_waiting_result`, `complete_msg_send_claim`,
  `recover_msg_send_leases`, `record_msg_send_push_result`의
  `service_role` 전용 EXECUTE
- 일곱 RPC만 정당화된 `SECURITY DEFINER`, `search_path=''`,
  fully-qualified relation 사용
- 신규 객체 외 grant/default privilege 변경 0

## rollback 예상 결과

- 일곱 신규 RPC와 네 신규 table의 의존성 역순 제거
- 기존 `device`, `users`, `alertsend`, command object 보존
- `CASCADE` 미사용
- 신규 message queue 데이터 삭제

## data-loss 경계

rollback은 기존 object를 변경하지 않지만 신규 contact consent snapshot,
policy snapshot, delivery attempt·lease, provider PUSH result, fallback·cost,
link token hash 이력을 삭제하는 경계. live activation 뒤 rollback 전 export와
사용자 별도 승인 필요.

## 후속 gate

- local PostgreSQL 17 격리 rehearsal
- Supabase advisors와 migration diff review
- worker DB adapter/RPC signature 통합 검증
- 승인 수신자 1명·승인 template·월 비용 cap 기반 실제 발송 별도 승인
- PUSH result·수신·link route 검증 뒤 SMS fallback 별도 승인
