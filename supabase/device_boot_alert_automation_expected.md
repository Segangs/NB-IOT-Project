# Device Boot Alert Automation Expected Result

- 신규 `device_boot_logs` 정상 부팅 행의 기기부팅 알림톡 자동 등록
- Flash·RAM·AT·CPIN 상태가 모두 `0`인 부팅만 알림 대상으로 판정
- 기존 EMQX 부팅 RPC의 `request.path=/rpc/b`·`POST` 요청만 자동화 대상으로 허용
- 승인 템플릿 `bizp_2026071315003676625784727`과 온도이력·설정변경 링크 재사용
- 동일 기기의 5분 이내 반복 부팅 메시지 억제
- migration 시 과거 부팅 로그 backfill·기존 메시지 재발송 없음
- 알림 등록 실패 시 원래 부팅 로그 저장 유지
- 기존 `device_boot_logs` RLS 비활성·anon/authenticated INSERT 권한은 별도 승인 보안 작업으로 유지
- 기존 펌웨어·EMQX·Spaceship worker·온도 알림 계약 변경 없음
