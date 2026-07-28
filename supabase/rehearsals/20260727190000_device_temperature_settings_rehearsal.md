# 센서별 온도 설정 로컬 검증 기록

- 대상 migration: `20260727190000_device_temperature_settings`
- 검증일: `2026-07-27`
- 운영 Supabase 적용: 완료
- 운영 Ubuntu 배포: 완료

## 완료 검증

- Server Python 전체 테스트: `106/106` 통과
- Message worker 전체 테스트: `141/141` 통과
- Supabase migration 계약 테스트: `142/142` 통과
- 모바일 JavaScript 테스트: `4/4` 통과
- Python compileall: 통과
- Git whitespace 검사: 통과
- `pglast 8.3` SQL 구문 파싱: migration·precheck·verify·rollback·behavior `5/5` 통과
- 390px 모바일·1280px 데스크톱 브라우저 시각 검증: 통과
- 운영 precheck: TEMP 센서 `10`·고온 상태 `1`·processing outbox `0`
- 운영 migration: 통과
- migration history: `20260727190000`·statement 1개·MD5 `c662c324e9cee6a073e0c7dd8fb0151b`
- 운영 verify: 설정 행 `0`·고온 상태 `1`·processing outbox `0`
- 운영 behavioral rehearsal: 1개·2개 센서 저장·전체 rollback·최대 알림 1→3 변경 통과
- 운영 호환성 보정: `jsonb_object_keys` 사용·기본키 제약조건 직접 지정
- Ubuntu service: active·PID `53392`·NRestarts `0`
- 로컬/공개 HTTP: root·responsive JS `200`, 잘못된 설정 링크 `404`

## 제한 사항

- 로컬 Server 전체 재검증: 현재 macOS Python 환경의 Flask 미설치로 미수행
- 대체 근거: 배포 전 Ubuntu staging Server `106/106`·운영 Python import·HTTP smoke 통과
- 실제 알림톡 설정 링크 클릭·저장: 신규 raw token 미발급으로 미수행

## 운영 적용 영향

- `temperature_alert_preference` 테이블 1개 추가
- service-role 전용 설정 조회·저장 RPC 2개 추가
- 기존 온도 알림 trigger 함수의 최대 3회 고정값을 센서별 `1..3` 설정값으로 교체
- migration 중 `USER_SENSOR`·온도 알림 상태·outbox의 짧은 쓰기 잠금
- migration 자체의 기존 상한 온도·메시지·센서값 변경 없음

## Rollback 경계

- 신규 RPC·설정 테이블 제거
- 온도 알림 최대 횟수의 기존 3회 고정 로직 복원
- 사용자가 저장한 `USER_SENSOR.setTmpUpLimit` 값의 자동 복원 없음
