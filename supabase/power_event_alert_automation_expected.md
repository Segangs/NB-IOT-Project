# Power Event Alert Automation Expected Result

- MQTT `devices/{imei}/event`의 9개 정수 배열만 수신
- `adapter_removed`·`adapter_restored`·`power_shutdown` 이벤트 저장
- 기기·수신경로·이벤트·incident·sequence 기준 중복 저장 및 중복 발송 차단
- 어댑터 분리 직후 `adapter_removed` 알림톡 1건 등록
- 어댑터 복원 시 `adapter_restored` 알림톡 1건 등록
- 210초 종료 진입 시 `power_shutdown` 알림톡 1건 등록
- 온도이력·설정변경 링크 토큰 2개 생성
- 이벤트 저장 후 알림 큐 실패 시 원본 이벤트 유지
- RLS 활성 및 테이블 직접 anon/authenticated 접근 차단
- 서명된 EMQX 요청만 허용하는 별도 Vault 별칭 사용
- 300초 배터리 유지시간 provisional 표시
- 실제 어댑터 제거·LTE 전송·부저 부하 실측 후 확정
- migration·EMQX live apply·실발송은 별도 승인 후 수행
