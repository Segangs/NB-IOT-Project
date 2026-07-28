# 메시지 처리속도·순서 보장 예상 결과

## 적용 결과

- Spaceship 메시지 worker 1분 주기 실행
- 실행당 최대 20건 단일 순차 처리
- 50초 경과 후 새 메시지 claim 중단과 다음 실행 인계
- Bizppurio 동시 전송 1건 유지
- 같은 전원 사건의 선행 메시지 종료 후 후속 메시지 처리

## 데이터 영향

- 기존 `msg_send` 행의 값과 상태 변경 없음
- `claim_msg_send(text,integer,integer)` 함수 본문만 교체
- 전원 사건 선행 메시지 조회용 부분 인덱스 1개 추가
- 기존 lease fencing, 우선순위, 만료, 재시도 계약 유지

## 실패와 복구

- migration 실패 시 전체 트랜잭션 rollback
- 운영 문제 발생 시 제공된 rollback SQL로 기존 claim 함수 복원
- Spaceship 문제 발생 시 이전 release symlink와 crontab 복원

## 검증

- 적용 전 활성 queue 수 확인
- 적용 후 함수 본문·인덱스·service_role 전용 권한 확인
- 독립 사건 처리와 같은 전원 사건 순서 보장 확인
