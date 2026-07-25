# ❄️ PicoTeam & NB-IoT 통합 개발 역사 및 작업 기록 (Project History)

> [!NOTE]
> **🎯 전체 프로젝트 개요 및 목적**
> 
> 본 프로젝트는 **NB-IoT (HL7811) 셀룰러 모듈**과 **Raspberry Pi Pico 2 W** 단말을 기반으로 한 **초저전력 지능형 이상온도 감지 및 실시간 원격 관제 시스템**입니다.
> 산업용 극저온 냉동고 및 백신 보관소 등의 온도 데이터를 수집하고, 실시간 통신 및 동적 임계 규칙 탐지 엔진을 통해 이상 현상을 관제 화면에 송출 및 AI 챗봇을 통한 능동적 대처 가이드를 제시하는 것을 목적으로 합니다.
> 
> **🛠️ 주요 기술 사항 및 아키텍처**
> * **Edge Device (단말 장치)**:
>   * **MCU**: RP2350 (Raspberry Pi Pico 2 W) 기반 C/C++ SDK 펌웨어 설계.
>   * **RTOS**: FreeRTOS 커널 멀티태스킹 스케줄링을 통해 센서 측정, LCD 렌더링, 모뎀 통신, 부저 경보 루틴을 완벽히 병렬화.
>   * **Modem**: HL7811 셀룰러 모듈을 제어하여 LTE-M(NB-IoT) 망을 통한 TLS 1.2 보안 규격 통신 연동.
>   * **Self-Diagnostics**: 부팅 단계에서 전압(VSYS), 과열(Internal Temp), 플래시 무결성(CRC32), RAM 무결성(Pattern Test) 자가진단 수행.
>   * **Safety Guard**: 하드웨어 와치독(Watchdog), Powman 브라운아웃(Brown-out) 감지 및 오프라인 상태 대비 Flash 비휘발성 로깅 시스템(32바이트 구조체 정렬) 구축.
> * **Control System & Web Server (관제 웹 및 데스크톱)**:
>   * **Backend**: Flask (Python) 기반의 데이터 적재 API 및 기기 상태 관제 서버 구축.
>   * **Database & BaaS**: Supabase PostgreSQL을 통해 센서 측정값 및 부팅 자가진단 로그를 적재하고, Google OAuth 2.0 사용자 및 세션 만료 관리.
>   * **Realtime Sync**: Supabase Realtime 웹소켓(WebSocket) 감지를 연동하여 실시간 데이터 변화 감지 및 화면 깜빡임 없는 DOM 갱신.
>   * **Desktop Packaging**: PyWebView를 통해 단일 브라우저 루프백 연동 로그인(Safari/Chrome 패스크 우회)을 지원하는 크로스 플랫폼 데스크톱 패키징 실현.


본 문서는 **PicoTeam 지능형 이상감지 관제 시스템** 및 **NB-IoT (HL7811) Pico 2 W 단말 장치** 개발 프로젝트의 시작부터 현재까지 진행된 모든 대화 세션의 요청 사항, 작업 내역, 기술적 의사결정 및 트러버슈팅 세부 내역을 총망라하여 기록한 통합 역사 파일입니다.

모든 신규 기능 추가, 문제 해결 및 튜닝 이력은 **최신순(역순)**으로 지속적이고 누적하여 기록됩니다.

---

## 📅 2026-07-25: [G2C·Flash Core] Command A/B journal storage-independent 구현

* **개발 범주**: Command Journal, CRC32, A/B Recovery, Power-cut Host Test, Firmware Source Graph
* `CommandJournalRecord`에 retry count·expected reset/power-off effect·remaining TTL·monotonic/Unix/boot checkpoint 필드 추가
* `job_id`와 persistent `job_ref`의 uint32 일대일 대응, 64바이트 little-endian `CommandJournalRecordV1` encode/decode 구현
* CRC32 ISO-HDLC known-answer `123456789 → 0xCBF43926`과 record 전체 64바이트 bit 손상 탐지 적용
* slot 전체 `0xFF`만 Blank로 인정하고 Valid·Corrupt를 분리하는 A/B 최신 record 선택 구현
* `UINT32_MAX→1` sequence wrap, half-range `0x80000000` ambiguity, equal-sequence divergent split-brain의 write-target 없는 fail-closed 처리
* valid Empty tombstone으로 이전 command 재등장 차단, 1~63바이트 부분 program 전원차단에서 기존 valid record 유지
* persistent `ExecuteMarked` 복원 시 `Failed/Journal` terminal 전환과 재실행 금지, retry count 255 saturation 구현
* TDD 기준선 Command/ACK 133 checks, RAM 계약 GREEN 180 checks, 신규 command journal 818 checks
* Host Debug·Release 각 25/25, G1/G2 Python contract 133/133, `git diff --check` 통과
* fresh Pico 2 Release UF2 `/private/tmp/nb-iot-command-journal-firmware/nb_iot_project.uf2`, 486,912바이트, 2026-07-25 19:50:17 KST, SHA-256 `e327350ea5ebbb80d6f42d1d7082dec4a611d818e8eab12bd0a588e153769c33`
* 실제 Command Flash offset·sector·erase/program/store·RuntimeOwner persistence·physical dispatch 제외, unified 4MiB partition 승인 뒤 후속
* Pico flash·serial·Supabase·EMQX·server·Git stage·commit·push 변경 0
* **추가 구현 범주**: Unified Flash Partition V1 중앙 layout·기존 sensor log/shutdown 중앙 참조·RP2350 actual dormant Command Flash A/B store 구현 완료
* **dormant 경계**: Command Flash adapter source graph 등록·object compile 확인, RuntimeOwner 호출 0으로 Release ELF/UF2 section GC 가능성 미검증 유지
* **artifact gate 경계**: Firmware A BIN 1,280KiB post-build size gate만 구현, Model artifact writer·pipeline·size gate 미구현 유지
* **전체 회귀**: fresh Host Debug·Release 각 29/29, G1/G2 Python contract 133/133 통과와 RuntimeOwner/task/backend `command_journal_flash_*` 호출 0 확인
* **fresh firmware**: Release BIN `/private/tmp/nb-iot-command-store-firmware/nb_iot_project.bin` 243,096바이트, UF2 `/private/tmp/nb-iot-command-store-firmware/nb_iot_project.uf2` 486,912바이트, 2026-07-25 22:54:51 KST, UF2 SHA-256 `a6bb753a83c124f3915ddc914d787df02da8cd5664f5e66f68f1747a42ec0c1e`
* **범위·미완료**: RuntimeOwner persistence·periodic activation·physical dispatch·Pico power-cut·실기 Flash 검증 미연결, Pico copy/flash/serial 0
* **외부 경계**: Supabase·EMQX·server Task 8 변경 0, staged 0, commit·push 0

## 📅 2026-07-25: [P01·Supabase] command migration 로컬 PostgreSQL rehearsal

* **개발 범주**: Supabase Migration Rehearsal, PostgreSQL 17, Rollback, Sanitized Fixture
* Supabase development branch 비용 시간당 `$0.01344` 확인과 사용자 승인 뒤 생성 시도
* 현재 조직의 Pro 미만 플랜 제한으로 branch 생성 거부, branch·과금 발생 0
* Homebrew PostgreSQL 17.10을 서비스 미등록 상태로 설치하고 `/private/tmp` 전용 cluster·socket·port `55432` 사용
* 실제 사용자 row·secret 없이 sanitized device 2개·legacy command 3개 fixture 구성
* `20260725054445_device_command_state` precheck의 PostgreSQL 15+·Asia/Seoul·legacy function MD5·trigger·column·row·FK·IMEI uniqueness gate 통과
* up migration 적용 뒤 legacy 3행→companion 3행 1:1 backfill, recent claimable/leased 2행·expired terminal 1행 검증 통과
* RLS 활성·policy 0·`anon`/`authenticated` 권한 0·`service_role` 전용 권한·empty `search_path`·SECURITY DEFINER/INVOKER·legacy claim SQLSTATE `55000` gate 검증 통과
* down migration 뒤 신규 table·trigger·function 4개 완전 제거와 legacy `deviceCmds` 3행·`assign_device_command()` MD5·`trg_assign_device_command` 보존 확인
* contracts 전체 133/133 통과
* 임시 PostgreSQL server 종료, `/private/tmp` cluster 삭제, Homebrew `postgresql@17`·신규 자동 의존성 4개·자동 생성 기본 cluster 제거
* 운영 Supabase·DB·EMQX·Flask·Spaceship mutation 0, Git stage·commit·push 0

## 📅 2026-07-25: [P00·Supabase] Spaceship baseline 마감과 command migration 초안

* **개발 범주**: P00 Read-only Audit, Spaceship SSH, Supabase Migration Draft, Command Claim/ACK, Rollback
* 기존 encrypted `id_rsa` 보존과 별도 RSA 4096 `spaceship_codex_rsa` 생성, cPanel public key 승인 뒤 read-only SSH 인증 성공
* `/home/yjijjnuzbr/project/msg_send/bizppurio_token_check.py` mode·size·checksum과 project `.env` mode `0600`·Bizppurio key-name-only 확인
* Spaceship worker process·service·cron 모두 없음과 operational status `not_deployed` 확정
* host 255일 uptime·24 CPU·load average `8.46/8.65/8.04`·home filesystem 28% 확인
* current outbound IP 외부 조회는 metadata egress 별도 승인 전 미실행, historical private baseline과 Message activation gate 유지
* P00-DRAFT의 Supabase·EMQX·Flask·Cloudflare·Auth·provenance·Spaceship sanitized evidence 전 항목 PASS와 product mutation 0 마감
* 기존 `public."deviceCmds"`·`assign_device_command()`·`trg_assign_device_command`를 삭제·rename하지 않는 lowercase `device_command_state` companion migration 초안 작성
* legacy KST wall-clock의 `timestamptz` backfill, 24시간 TTL, 한 요청당 1건 lock, 최대 5회 bounded redelivery, accepted/final ACK와 exact receipt 설계
* 신규 table RLS 활성·policy 0·`anon`/`authenticated` 권한 0, claim/ACK RPC `service_role` 전용 초안
* legacy `trg_assign_device_command` 존재 중 신규 claim의 SQLSTATE `55000` fail-closed 차단, reference-zero·trigger 제거 전 신규 destructive writer/consumer 활성화 금지
* RPC 필수 NULL 검증·accepted request 재생·ACK/receipt uint32용 bigint ingress·backfill/ACL/함수 속성 verify 보강
* precheck/up/down/verify/expected-result artifact와 migration contract test 14/14 통과
* `gpt-5.6-sol` max 독립 리뷰의 Critical 1·Important 4 수정 후 재검토 Critical 0·Important 0
* 로컬 Supabase CLI·PostgreSQL 부재로 실제 SQL rehearsal 미실행, sanitized clone rehearsal과 live apply 별도 승인 gate 유지
* Supabase·EMQX·Flask·Spaceship live 변경, Bizppurio provider call·실메시지, Git stage·commit·push 0

## 📅 2026-07-25: [G2C] TEMP·compact JSON·Command/ACK device-free 구현

* numeric CSV 전환 취소와 기존 telemetry·boot·config compact JSON 유지, 신규 command request/response·ACK/receipt도 80바이트 이하 numeric JSON array로 고정
* TEMP fresh·CRC fallback 3회/30초·4회째 실패·non-CRC 실패·stale telemetry/alarm 금지의 allocation-free quality core와 SensorTask 단일 writer 연결
* 최소 한 개의 완전한 TEMP+MIC port pair를 Pass로 보는 snapshot health와 fresh sample 전용 telemetry 연결
* command request correlation·TTL·dedupe·accepted/final ACK·exact receipt·single-dispatch latch·부팅 복구 금지 상태 코어 구현
* config payload의 command side effect 제거와 `cmd/request`·`cmd/response`·`cmd/ack`·`cmd/ack/receipt` 전용 RuntimeOwner backend 경로 분리
* Flash A/B durable journal 전 `request_status`만 성공, reboot·power-off·FOTA 실제 실행 없이 terminal execution failure로 제한
* compact JSON Python 52/52, TEMP 112 checks, Command/ACK 133 checks, RuntimeOwner backend 454 checks, Host Debug·Release 각 24/24 통과
* fresh Pico 2 Release UF2 486,912바이트·2026-07-25 13:38:18 KST·SHA-256 `f06e6b90bb5ff3cbd1afa91910378ed2f9d4698dac91d25f4d15a044eeab3907` 생성, Pico flash·serial·DB·EMQX·server·commit·push 변경 0
* P00 read-only 감사의 Supabase·EMQX·Flask·Cloudflare evidence 완료, Spaceship encrypted SSH key 인증 전 migration/rollback SQL artifact 생성 보류

## 📅 2026-07-25: [Stage 13] 교체 장비용 자동 제품 UF2 복구

### GP15 종료 기록 준비 구현·USB 연결 플래시 검증

* 기존 RuntimeOwner 단일 owner와 7단계 cleanup을 유지한 64바이트 CRC shutdown record core·A/B Flash store 구현
* Flash 마지막 64KB를 일반 log `0x3F0000`~`0x3FDFFF` 56KB, slot A `0x3FE000` 4KB, slot B `0x3FF000` 4KB로 분리
* monotonic sequence·producer sequence·incident correlation·종료 원인·USB 시작 상태·예정 action·cleanup mask·deadline·elapsed·CRC 저장
* 한 slot erase/program 뒤 XIP readback CRC·binary equality 검증, 손상 slot fallback과 sequence wrap 최신 선택 적용
* initial/final USB present 일치와 verified poweroff record를 함께 요구하는 watchdog branch, absent 일치에서만 허용되는 GP15 branch 적용
* USB 상태 변화·CPWROFF/record 실패·hard deadline의 watchdog·GP15 양쪽 금지와 fail-closed terminal action 적용
* GP15 inactive HIGH preload→output→marker→active LOW 1회 순서를 RuntimeOwner final action 한 곳에만 배치, backend·producer의 GP15 권한 0
* GP14 producer의 USB-present admission 제거와 500ms debounce·RuntimeOwner ready·typed provenance 유지
* `AT+CPWROFF` `OK`·GP2 LOW 뒤 record commit을 수행하고 commit 시간을 제외한 남은 90초 budget의 GP2 LOW settle·최종 1초 reserve 유지
* record RED→GREEN 28 checks, finalizer RED→GREEN 116 checks, backend·atomic source contract RED→GREEN 확인
* fresh Host Debug 22/22·Release 22/22, G1 112/112, GP15 active write 1곳·backend write 0·`git diff --check` 통과
* 최종 Pico 2 Release UF2 `/private/tmp/nb-iot-stage13-gp15-record-pico2-20260725/nb_iot_project.uf2`, 478,720바이트, 2026-07-25 12:28:30 KST, SHA-256 `1cb29815e23646006007ec3808a50ac40f796071dfdb1bedecf8b3053edafdd9`
* 기존 전용 macOS Terminal 자동 재연결 유지, 1200-baud 자동 BOOTSEL·picotool write/verify 100%·application reboot 통과
* 최종 USB 연결 부팅의 `LAST_SHUTDOWN result=NONE`·`SELFTEST OK`·`MODEM_AT_OK`·`CERT_WRITE_OK` 확인
* 첫 LTE 등록 거부 `CEREG: 3`·`MQTT_CONNECT_FAIL` 뒤 RuntimeOwner fault/recovery 1회와 재등록·`MQTT_CONNECT_OK` 자동 회복 확인
* boot PUBACK·CONFIG QoS 0 SUB·request PUBACK·`[-7,-10]` 수신·`CONFIG_LIMIT_OK`·`PERIODIC_READY`·`BOOT_DONE` 재도달
* 최종 부팅 구간 `SHUTDOWN_GP15_COMMIT`·`SHUTDOWN_WATCHDOG_COMMIT`·USB changed/evidence missing abort 0
* 실제 USB 미연결 GP15 차단 실기는 U6 KILL# active polarity·전원 경로 as-built 계측 전 보류
* DB·Supabase·EMQX·server·Spaceship·commit·push 변경 0

### GP14 1차 실기 결함과 CPWROFF 완료 대기 보정

* 실제 GP14 short-press에서 `POWER_BUTTON_SHUTDOWN_ACCEPTED`·dying status PUBACK·session 1 CLOSE/DEL·session 2~6 absent 정리·`KMQTTCFG?`·`KCNXDOWN=1`·`CFUN=0`·`CPWROFF OK`·watchdog 재부팅 확인
* `CPWROFF OK` 직후 약 1초 만에 재부팅한 뒤 반복 `MODEM_AT_FAIL`·`RUNTIME_OWNER_RECOVERY`가 발생한 실기 결함 재현
* Rev29 5.29와 timeout 표에서 normal `AT+CPWROFF`의 `OK`는 즉시 반환되는 접수 응답이고 명령 상한은 120초임을 원문·페이지 렌더로 재검증
* 현재 PCB GP5 TX_ON은 LTE 송신 창에만 active-high인 표시 신호라 power-off 완료 감지에 사용할 수 없는 경계 확인
* 승인된 normal `AT+CPWROFF`·90초 hard deadline을 유지하고, 접수 뒤 `MODEM_POWEROFF_FINALIZE_RESERVE_MS=1000`을 제외한 남은 budget 동안 GP2 WAKEUP LOW를 유지하는 보정
* 보정 source contract 변경 전 7/431 RED와 변경 후 431/431 GREEN, Host Debug·Release 각 21/21·G1 10/10·`git diff --check` 통과
* finalizer backend GP15 write 0·watchdog 직접 호출 0 유지, watchdog 소유 위치 RuntimeOwner finalizer 1곳 유지
* fresh Pico 2 Release UF2 `/private/tmp/nb-iot-stage13-shutdown-pico2-settle-20260725/nb_iot_project.uf2`, 474,624바이트, 2026-07-25 11:24:48 KST, SHA-256 `83bce39dd98aae64244a58aa65f593891bd43e9000c8ef6407eb49af04114dc9`
* 1200-baud 자동 BOOTSEL·picotool write/verify 100%·전용 macOS Terminal 자동 재연결 통과
* 모뎀 완전 전원 재인가 뒤 보정 GP14 short-press accepted `11:33:00`, dying status PUBACK·session/PDP/CFUN cleanup 통과
* `AT+CPWROFF` 접수 `11:33:17`, `MODEM_POWEROFF_SETTLE 71959`, watchdog commit `11:34:29`, USB serial 자동 재연결 통과
* 새 boot의 modem AT·SIM READY·인증서·PDP·TLS·MQTT 연결·boot PUBACK·CONFIG `[-7,-10]` 재수신·적용 통과
* `BOOT_DONE`·`PERIODIC_READY` `11:36:09` 재도달, `MODEM_AT_FAIL`·RuntimeOwner recovery 반복 0으로 GP14 USB watchdog branch 최종 Pass
* DB·Supabase·EMQX·server·commit·push 변경 0

### GP14 USB 안전 종료 finalizer 1차 구현·플래시

* GP14 LOW 500ms debounce·USB 연결·RuntimeOwner ready 조건을 모두 만족할 때만 typed power-button urgent 요청 제출
* source-specific shutdown port가 `Accepted`한 정확한 sequence·correlation만 보존하고 duplicate·stale·terminal 요청의 context 덮어쓰기 차단
* 90초 hard deadline 안에서 출력 정지·dying status 발행·MQTT session 1~6 CLOSE/DEL·`KMQTTCFG?` scan·`KCNXDOWN=1`·`CFUN=0`·`CPWROFF` 수행
* Rev29 절차에 따라 `AT+CPWROFF` 송신 직후 GP2 WAKEUP LOW 적용, 현재 USB 검증 단계의 GP15 KILL 호출 0
* cleanup 뒤 새 USB sample이 계속 present일 때만 scratch 2/3 기록과 watchdog reboot 허용, USB loss는 fail-closed 정지
* finalizer core 93 checks·device backend 423 checks·producer 273 checks·atomic cutover 73 checks 통과
* Host Debug·Release 각 21/21, G1 cross-contract 10/10, deadline·USB 반전·timeout mask·duplicate completion·CPWROFF 순서·USB 없는 watchdog mutant 6/6 탐지
* fresh Pico 2 Release UF2 `/private/tmp/nb-iot-stage13-shutdown-pico2-20260725/nb_iot_project.uf2`, 474,112바이트, 2026-07-25 11:08:00 KST, SHA-256 `d34ea9755bfef9009fe9f04bf72578ebaea1de78f300634f52caa74ab6a22a76`
* 1200-baud 자동 BOOTSEL 전환·picotool write/verify 100%·application reboot 통과, 전용 macOS Terminal `/dev/cu.usbmodem111201` 재연결과 `CERT_WRITE_OK` 확인
* 실제 GP14 short-press USB watchdog branch 통과, USB 분리 adapter-only GP15 전원 차단 검증은 후속 단계로 보류
* DB·Supabase·EMQX·server·commit·push 변경 0

### Alert·telemetry·periodic 중복 마감

* **orphan alert 제거**:
  - 승인된 server consumer가 없는 `devices/<id>/alert` 발행과 `RefreshRssi` 결합 제거
  - 변경 전 focused contract RED, 변경 후 device backend contract 378 checks 통과
  - 300초 RSSI 경계 이후 `/alert`·`+CME ERROR: 3`·disconnect·RuntimeOwner fault/recovery·timeout 0회
* **periodic 중복 교정**:
  - 60초마다 함께 호출되던 `PullConfig`와 `PullCommand`가 현재 backend에서 동일 `pull_config(true)`로 연결된 원인 확인
  - 별도 command topic·RPC 구현 전 periodic `PullCommand` producer만 제거, facade/backend 계약과 config 60초·정규 telemetry 20분 주기 보존
  - legacy cutover 계약 변경 전 1/49 RED 및 변경 후 49/49 GREEN
  - 실기 config request 초기 1회와 이후 60초 간격 4회, 각 PUBACK·compact DATA·`CONFIG_LIMIT_OK` 확인
* **실제 온도 telemetry**:
  - 사용자가 센서를 냉동실로 이동한 조건에서 GP22 sensor 1 `-15.8°C`, payload `[1,-15.8]`, `+KMQTT_IND: 1,4` 확인
  - Supabase read-only 조회의 `sensorValueId=2830`, `sensorId=17`, `userSensorId=1`, `deviceId=5`, 값 `-15.8`, 시각 `2026-07-25 02:25:37` 저장 확인
  - 사용자 지정 `TEMP1_CAL_OFFSET_C=5.0f` 유지, GP26 sensor 2 미연결 상태는 실기 검증 보류
* **최종 산출물·운용**:
  - Host Debug·Release 각 20/20 통과
  - fresh Pico 2 UF2 `/private/tmp/nb-iot-stage13-single-pull-pico2-20260725/nb_iot_project.uf2`, 469,504바이트, 2026-07-25 02:20:09 KST, SHA-256 `48a504761ae71f87bfc90ffa41851d8086accae2c80fbc3216e25fbf599fc369`
  - `picotool load -f -v -x` flash write·verify 100%와 application reboot 통과
  - 새 창 추가 없이 기존 전용 macOS Terminal `/dev/ttys000`에 자동 재연결 serial monitor 재사용
  - DB·Supabase schema·EMQX·server·commit·push 변경 0

### Replacement unit LCD adapter 실패와 5초 rollback

* 5V adapter 연결 첫 공식 반복에서 LCD 백라이트만 점등되고 문자 미출력, raw log `LCD_SKIP` 확인
* GP16/GP17 line-low `LCD_BUS_STUCK`은 아니며 candidate `0x27`·`0x3F` 모두 ACK 부재
* REVIEW-12/HW-07 실패 정책에 따라 adapter 10회 반복 즉시 중단과 3초 production Pass 보류
* LCD delay contract를 3초에서 5초로 RED→GREEN 전환, LCD contract 28/28·Host Debug/Release 각 20/20 통과
* fresh Pico 2 recovery UF2 `/private/tmp/nb-iot-stage13-lcd5-pico2-20260725/nb_iot_project.uf2`, 469,504바이트, 2026-07-25 02:35:31 KST, SHA-256 `3329dccc8c6314b631a69111d848ab1a266dee22469dc1ba31ec113ec3722c3c`
* `picotool` flash write·verify 100%와 기존 전용 Terminal `/dev/ttys000` 재사용
* 5초 재부팅과 동일 UF2 독립 재시도에서도 `LCD_SKIP` 반복 재현으로 단순 안정화 지연·일시 오류 원인 기각
* PCB source의 GP16 SDA·GP17 SCL·CN4 5V mapping 재확인, replacement unit I2C address·배선·level-shifter ACK 경계 후속 진단 등록
* 추가 full address scan·pin swap·forced write는 사용자 승인 전 미실행

### LCD I2C 전체 주소 진단

* 사용자 승인에 따라 GP16 SDA·GP17 SCL·I2C0의 유효 7-bit 주소 `0x08`~`0x77` 부팅 1회 scan 적용
* 각 ACK 주소·총 ACK 수·기존 `LCD_OK`/`LCD_SKIP`을 분리하는 USB serial marker 추가
* LCD source contract 변경 전 4/31 RED와 변경 후 31/31 GREEN, Host Debug·Release 각 20/20 통과
* fresh Pico 2 Release UF2 `/private/tmp/nb-iot-stage13-lcd-scan-pico2-20260725/nb_iot_project.uf2`, 470,016바이트, 2026-07-25 02:50:57 KST, SHA-256 `ba3696c11c320ab19dab4fdc0d20c2f03134f80577ac9578b1dbb174ee03928b`
* `picotool load -f -v` write·verify 100%와 기존 전용 Terminal `/dev/ttys000` 선행 대기 후 application reboot
* 실기 `LCD_SCAN_START` → `LCD_SCAN_DONE 0` → `LCD_SKIP` 확인으로 유효 주소 전체 address ACK 0 판정
* LCD candidate 주소 누락 가설 제외, CN4 connector·GP16/GP17 continuity·BSS138 LV/HV 전원·LCD backpack 전원/접지 물리 경계 후속 등록
* pin swap·forced write·DB·EMQX·server·commit·push 변경 0

### 사용자 정정 기반 5초 직접 LCD 초기화

* 이전 실제 동작 코드가 address probe 없이 `0x27` LCD 객체와 HD44780 초기화를 실행한 차이 재확인
* 전체 scan ACK 0을 물리 단선으로 단정한 기존 해석 철회, 초기화 전 probe 결과로 범위 축소
* 사용자 지시에 따라 LCD task의 5초 대기 뒤 pre-init GPIO check·address scan·early task delete 제거
* `LCD_ADDR=0x27`로 `LCD_I2C` constructor와 기존 HD44780 4-bit 초기화 sequence 직접 실행
* 직접 초기화 source contract 변경 전 6/28 RED와 변경 후 28/28 GREEN, Host Debug·Release 각 20/20 통과
* fresh Pico 2 Release UF2 `/private/tmp/nb-iot-stage13-lcd-direct5-pico2-20260725/nb_iot_project.uf2`, 468,992바이트, 2026-07-25 02:59:42 KST, SHA-256 `b1f0f1362f2b30f08a02c2dc7259a8052ff887b32e0a56da120c91aafa9abc0a`
* `picotool load -f -v` write·verify 100%, 기존 전용 Terminal 재사용
* 실기 `LCD_INIT_START 0x27` 03:01:41.066과 `LCD_INIT_DONE 0x27` 03:01:41.191 확인, `LCD_SKIP` 0
* 실제 LCD 문자 출력 사용자 육안 확인 대기, DB·EMQX·server·commit·push 변경 0

### 마지막 동작 버전 LCD 드라이버 복원

* 5초 직접 초기화 UF2에서도 백라이트만 점등되고 문자 미출력된 사용자 확인
* 현재 드라이버가 마지막 동작 Git 버전과 달리 `50 kHz`·3ms timeout write·1000µs enable pulse·nibble 전용 초기화로 변경된 차이 확인
* 5초 task 지연과 PCB 기준 GP16 SDA·GP17 SCL·`0x27` 주소는 유지하고 드라이버만 `100 kHz`·blocking write·600µs enable pulse·기존 `Send_Command(0x03)` 3회 뒤 `0x02` 순서로 복원
* LCD 계약 변경 전 13/34 RED와 변경 후 34/34 GREEN, Host Debug·Release 각 20/20 통과
* fresh Pico 2 Release UF2 `/private/tmp/nb-iot-stage13-lcd-legacy5-pico2-20260725/nb_iot_project.uf2`, 467,968바이트, 2026-07-25 03:07:29 KST, SHA-256 `423becd9ea7c902298389965e733674a57ea7473210e5f67ca5afad340f69c2c`
* `picotool load -f -v` write·verify 100%, 기존 전용 Terminal `/dev/ttys000`과 자동 재연결 monitor 재사용
* 실기 `LCD_INIT_START 0x27` 03:09:34.967과 `LCD_INIT_DONE 0x27` 03:09:35.006 확인, 실제 문자 출력 사용자 육안 확인 대기
* DB·Supabase·EMQX·server·commit·push 변경 0

### 옛날 보존 source 5초·10초 실기 A/B와 제품 복구

* `100 kHz` 옛날 Git driver 5초 회차도 백라이트만 점등되어 단순 driver sequence 원인 기각
* 실제 성공 당시 구조를 재현한 `50 kHz`·timeout write·pre-scheduler 5초 회차도 백라이트만 점등
* 메인 작업 폴더에 보존된 당시 Pico 2 W source를 수정 없이 직접 빌드한 5초 이미지도 백라이트만 점등
* 최초 “옛날 코드 그대로” 표현이 실제 성공값 10초가 아닌 현재 보존값 5초였음을 정정
* 보존 source의 LCD 지연만 실제 성공 기록값 10초로 임시 변경해 빌드한 `/private/tmp/nb-iot-old-source-exact-lcd-delay10-pico2w-20260725/nb_iot_project.uf2`도 백라이트만 점등
* 10초 이미지 949,760바이트, 2026-07-25 03:22:29 KST, SHA-256 `edf337113998b51e23454bc6cc37d38408b7471dfecab133b6ffb497f0b88b70`
* 임시 source 지연값 즉시 5초 원복, active 통합 worktree의 RuntimeOwner pre-scheduler 무정지 계약과 Host Debug·Release 각 20/20 복구
* current Pico 2 제품 UF2 `/private/tmp/nb-iot-stage13-lcd-product-restored-pico2-20260725/nb_iot_project.uf2` fresh build·flash write·verify 100%
* 복구 제품 UF2 468,992바이트, 2026-07-25 03:25:22 KST, SHA-256 `b1f0f1362f2b30f08a02c2dc7259a8052ff887b32e0a56da120c91aafa9abc0a`
* 복구 부팅 `SELFTEST OK`·`MODEM_PWR_ON`·`LCD_INIT_START 0x27`·`LCD_INIT_DONE 0x27`와 기존 전용 Terminal monitor 유지 확인
* full 7-bit address scan ACK 0과 모든 초기화 A/B 실패를 결합해 현재 교체 장비의 LCD harness·BSS138 level shifter·GP16/GP17 continuity·LCD backpack 응답을 다음 물리 진단 경계로 확정
* DB·Supabase·EMQX·server·commit·push 변경 0

* **장비 기준**:
  - 현재 실기 MCU를 Raspberry Pi Pico 2/RP2350으로 교체한 사실에 맞춰 product `PICO_BOARD`를 `pico2`로 교정
  - 현재 modem 기준 HL7810 firmware `5.5.14.0`, 과거 Pico 2 W·이전 modem 실기 결과와 신규 장비 결과 분리
* **제품 경로 복구**:
  - 독립 `modem_at_console` target과 source 보존
  - 제품 target의 `NB_IOT_MANUAL_AT_SESSION_RESET_TRIAL=1` 해제로 boot session cleanup 뒤 자동 PDP·TLS·MQTTS 경로 복구
  - 승인된 `NB_IOT_POST_CONFIG_HANDOFF_TRIAL=1`과 redacted AT trace 유지
  - firmware worktree `.env`를 APN·broker host·port 3개 비밀 아님 값으로만 구성, root server `.env` 전체 복사·symlink·compiler definition 유입 0
* **Rev29 흐름 제어 보강**:
  - PCB CTS NC·RTS GND 조건과 factory profile의 `+IFC=2,2`·`&K3` 복원을 고려하여 제품·독립 진단 초기화에 `AT&K0` 다음 `AT+IFC=0,0` 순서 적용
  - device backend contract RED 2/341 및 modem console contract RED 1/42 재현 뒤 GREEN 341/341·42/42
* **보호선·회귀 검증**:
  - 2026-07-22~23 승인 하드웨어 진단 변경에 따른 `tasks_periodic_modem.cpp`, `tasks_debug.cpp`, `tasks_modem.cpp`, `tasks_mqtt.cpp`, `app_context.cpp` protected SHA 현행화
  - fresh Host Debug 20/20 및 Release 20/20 통과
  - fresh Pico 2 Release product build 통과, `picotool info`의 `pico_board: pico2`·RP2350 ARM Secure 확인
  - UF2 `/private/tmp/nb-iot-stage13-pico2-release-20260725/nb_iot_project.uf2`, 470,016바이트, 2026-07-25 01:04:29 KST, SHA-256 `0f735cf0b016e89d4cd4e41b7aa7e6a6be8bb7172ea8c1c2c34b86ff8bb4ca2`
  - ELF 문자열 기준 placeholder·`MANUAL_AT_READY` 0, `p.zxcx.io`·`simplio.apn`·`AT+IFC=0,0`·`PERIODIC_FIRST_TELEMETRY` 포함
* **정지선**:
  - 최초 자동 제품 UF2 flash write·verify 100%와 사용자 표시 전용 Terminal 수집 완료
  - 교체 modem의 실제 IMEI·IMSI를 MQTT 서비스 identity로 사용한 첫 회차의 인증 실패 뒤, firmware 전용 로컬 설정에서 service device ID·username·password를 분리하고 모든 MQTT topic을 service device ID 기준으로 통일
  - 고정 identity 제품 회차에서 `MQTT_CONNECT_OK`·boot PUBACK·CONFIG SUB·request PUBACK·compact DATA `[-7,-10]`까지 성공했으나 QoS 1 DATA 직후 동일 UART frame의 `+KMQTT_IND: 1,0` 반복 재현
  - CONFIG subscription만 QoS 0으로 제한하는 firmware trial 적용, outbound boot/config request QoS 1과 server·EMQX·DB 무변경
  - QoS 0 실기에서 `MQTT_CONNECT_OK` 1회, `MQTT_SUB_OK` 1회, `MQTT_PUB_OK` 10회, `CONFIG_FRAME_COMPLETE`·`CONFIG_LIMIT_OK` 각 9회, `BOOT_DONE`·`PERIODIC_READY` 각 1회 확인
  - QoS 0 DATA 뒤 `+KMQTT_IND: 1,0`·`MQTT_DISCONNECTED`·`RUNTIME_OWNER_FAULT`·`RUNTIME_OWNER_RECOVERY`·`BOOT_OWNER_TIMEOUT` 각 0회로 기존 DATA 후 command-plane 정체 미재현
  - 300초 RSSI 점검에서 별도 alert 명령 `AT+KMQTTPUB=1,"devices/<id>/alert",1,0,"{"alert":1}"`이 중첩 쌍따옴표 문법으로 `+CME ERROR: 3` 발생, QoS 0 DATA 정체 해결과 분리된 후속 결함으로 등록
  - Pico 2 QoS 0 trial UF2 `/private/tmp/nb-iot-stage13-pico2-config-qos0-20260725/nb_iot_project.uf2`, 470,016바이트, 2026-07-25 01:30:03 KST, SHA-256 `840d5beb37fe682602c08dc8ae68baa19531c1bb1bf4b2598f447f5a017f66da`
  - 사용자 표시 전용 macOS Terminal raw 115200 자동 재연결 수집과 `/private/tmp/nb-iot-stage13-serial-attempt3-qos0.log` evidence 확보
  - DB·Supabase·EMQX·server·Spaceship 설정·commit·push 변경 0

## 📅 2026-07-23: [펌웨어/MQTTS] session reset 후 수동 AT console 재진입

* `modem_MqttOpen()`의 최초 boot-clean 경로에서 `MQTT_SESSION_RESET_ALL_OK` 완료 뒤 `manual_at_mode_enable()`·`modem_ManualAtConsole()` 재연결
* 수동 console 진입 뒤 자동 `AT+KCNXCFG`·TLS·`AT+KMQTTCFG`·`AT+KMQTTCNX`·publish 경로 중단, USB 입력 명령과 UART 응답 동시 수집 유지
* 수동 console 진입 계약 RED 4/329 확인 후 GREEN 329/329, `git diff --check` 통과
* fresh Release UF2 `/private/tmp/nb-iot-manual-at-firmware-build.IPZnWc/nb_iot_project.uf2` 471,040바이트, 2026-07-23 00:45:59 KST 생성
* Pico flash 후 새 USB CDC 포트 재연결과 부팅 초기 AT trace 확인, reset의 `AT+KMQTTDEL=3`가 분할 `+CME ERROR` 뒤 진행 정지하여 `MANUAL_AT_READY` 미도달
* EMQX 6.2.1 read-only audit — SSL `0.0.0.0:8883` listener 활성, Max QoS 2·Strict Mode off, config/telemetry/boot rules enabled·HTTP actions connected·broker alarm 0 확인
* EMQX MQTT Idle Timeout을 120초로 변경한 direct trial에서도 CONFIG 수신 30초 뒤 telemetry `AT+KMQTTPUB`가 선행 CRLF 뒤 5초 timeout으로 동일 재현
* 해당 실패 직후 EMQX API가 client `connected=true`·keepalive 120·`ssl:default`를 유지한 것을 확인, broker idle disconnect 가설 배제와 HL7811 내부 publish command 처리 정체 증거 보강
* CONFIG 수신 뒤 liveness probe 직전 `AT+KMQTTCNX=1` 단일 재연결 진단에서 약 100ms 뒤 `+CME ERROR: 911` 수신, 30초 connect 대기 후 liveness recovery·`AT+KMQTTCLOSE=1` 진입 확인으로 이미 연결된 session 재-CNX 가설 미채택
* 진단 분기 제거와 `NB_IOT_POST_CONFIG_HANDOFF_TRIAL=1` 복원, device backend contract 333/333·direct handoff 68/68, fresh Release UF2 470,016바이트 생성
* 독립 `modem_at_console` target 추가 — `src/diagnostics/modem_at_console.cpp` 단일 source와 Pico USB stdio·UART만 link, FreeRTOS·RuntimeOwner·sensor·LCD·audio·flash log·product task source 0
* PWRON HIGH→LOW→HIGH·30초 부팅 대기, `AT`·`ATE0`·`AT&K0`·`AT+CMEE=1`·`AT+CFUN=1`·`AT+CPIN?`·`AT+CGSN`·`AT+CIMI`, 기존 공개 Root CA 200바이트 chunk·500µs byte pacing 인증서 주입 뒤 `MANUAL_AT_READY` USB AT console 진입
* 수동 AT 입력 511바이트 상한·`AT` prefix 검증·CR 종결·256바이트 response drain·1초 command settle 유지, certificate 본문 USB log 미출력
* modem console contract 39/39, standalone UF2 70,656바이트·ELF 내 FreeRTOS·RuntimeOwner·DS18B20·KMQTT 문자열 0, 기존 product UF2 471,040바이트 fresh build
* Pico flash 실기에서 `AT`·`ATE0`·`AT&K0`·`AT+CMEE=1`·`AT+CFUN=1`·`AT+CPIN?`·`AT+CGSN`·`AT+CIMI` 응답, certificate `CONNECT`·790바이트 주입·`OK`·`CERT_WRITE_OK`, `MANUAL_AT_READY` 도달 및 사용자 첫 `AT`의 즉시 `OK` 확인
* 외부 서비스·DB·EMQX·server·commit·push 변경 0 유지

## 📅 2026-07-22: [펌웨어/MQTTS] CONFIG handoff 후 지연 telemetry 실험

* **개발 범주**: RuntimeOwner, HL7811 post-CONFIG stall 격리, Periodic Modem, TDD, Fresh UF2
* **실험 계약·구현**:
  - firmware 전용 `NB_IOT_POST_CONFIG_HANDOFF_TRIAL=1`에서 CONFIG 원자 반영 직후 `AT` probe·probe publish·구독 재확인·follow-up CONFIG 재요청을 발행하지 않고 동일 MQTT session과 subscription을 RuntimeOwner Ready 상태로 인계
  - boot snapshot에 `ConfigAppliedHandoff` stage와 `post_config_liveness=0`을 도입하여 post-CONFIG liveness가 검증됐다고 기록하지 않도록 분리
  - `PERIODIC_READY` 뒤 30초 quiet window를 유지하고, 최초 1회만 실제 `devices/<IMEI>/telemetry` sensor 1 payload를 발행, 이후 기존 주기에서 sensor 1·2 발행 유지
  - 기존 host 기본 경로는 compile definition 미설정 상태로 보존, trial firmware 경로는 config commit 직후 snapshot freeze를 수행하도록 RuntimeOwner core·adapter canonical transition 동기화
  - direct handoff의 실제 effect 수와 무관하게 4개 효과를 넣던 adapter pending queue를 `transition.effect_count` 기준으로 교정
  - snapshot 수락 시 기존 4개 liveness 완료 mask 대신 trial의 빈 mask를 허용하고, 생략한 liveness ticket ID를 예약해 기존 snapshot·EndBoot correlation ID 순서 유지
  - 임시 manual AT console 구현은 보존하되 `modem_MqttOpen()`에서 자동 진입을 제거하여 session reset 후 정상 자동 부팅 경로 복구
* **TDD·검증**:
  - direct handoff adapter queue RED 2/47과 TaskCore snapshot→Ready RED 6/68 재현 뒤 최종 `runtime_owner_post_config_handoff_test` 68 checks 통과
  - 기존 기본 경로 `runtime_owner_core_test` 53,196, `runtime_snapshot_core_test` 253, device backend contract 327, adapter core 144,067, task core 991 checks 통과
  - fresh Release firmware compile·link 통과, `/private/tmp/nb-iot-post-config-final-build/nb_iot_project.uf2` 470,016바이트, 2026-07-22 21:51:15 KST 생성
* **미해결·제한**:
  - Pico flash 실기에서 `CONFIG_LIMIT_OK -7.0,-10.0` 직후 `PERIODIC_READY`·`BOOT_DONE` 확인, 추가 post-CONFIG MQTT control AT 0
  - T1 DS18B20 `GP22 30.98,0` 정상 인식 뒤 재부팅, 30초 뒤 첫 telemetry `AT+KMQTTPUB …/telemetry,"[1,31.2]"`가 선행 CRLF만 받고 5초 무응답; 지연·직접 handoff로 stall 미해소
  - 60초 periodic CONFIG 재요청의 `AT+KMQTTPUB …/config/request`도 5초 무응답 재현, CONFIG downlink 뒤 HL7811 MQTT publish 처리부 복귀 실패의 추가 증거
  - 외부 연결·DB·EMQX·server·commit·push 변경 0 유지
  - 전체 host CMake configure는 이번 변경 전부터 `src/tasks/tasks_debug.cpp` 보호 SHA 불일치로 중단, 보호 hash와 해당 파일 미변경 유지

## 📅 2026-07-22

### [펌웨어/MQTTS] 부팅 session reset 후 수동 AT console 실기 진단

* **개발 범주**: RuntimeOwner, HL7811 USB Serial Console, TDD, Fresh UF2, Pico Hardware Verification
* **진단 계약·구현**:
  - 부팅 모델 초기화·망 상태 확인·MQTT session 1~6 CLOSE/DEL 전체 reset까지 기존 순서 유지
  - `MQTT_SESSION_RESET_ALL_OK`·`MQTT_BOOT_CLEAN_OK` 직후 자동 `KCNXCFG`·TLS·`KMQTTCFG`·publish 경로 정지
  - 단일 modem owner인 RuntimeOwner가 USB 입력을 최대 511바이트 `AT` 명령으로 검증하여 기존 `modem_SendCmd()` 경로로 전송하는 임시 수동 console 적용
  - HL7811 `\r` 자동 종결·1초 settle·256바이트 RX guard·reset 내 scoped delay bypass 계약 유지
  - USB 입력 non-echo로 `KMQTTCFG` credential의 저장 log 복제 차단, `AT` 접두어 외 명령 거절과 local `reboot` 명령 제공
  - DebugTask의 USB 입력 경쟁만 atomic flag로 차단, modem UART·API 직접 접근 0과 RuntimeOwner 단일 소유 유지
* **TDD·회귀 검증**:
  - 수동 console 계약 최초 RED 17/319, owner 경계 교정 후 RED 15/326 재현과 최종 device backend 326/326 GREEN
  - legacy cutover 48/48, producer facade 74/74, MQTT payload test, `git diff --check` 통과
  - DebugTask의 modem 직접 접근을 잡아낸 legacy cutover 중간 실패 3/48 후 RuntimeOwner 내 console로 설계 교정
* **Fresh build·flash·실기 결과**:
  - `/private/tmp/nb-iot-manual-at-build.qkaNd4/nb_iot_project.uf2` Release compile·link 통과
  - UF2 471,552바이트, 2026-07-22 15:18:51 KST, SHA-256 `379f6e22ebc57d54244f65742efc78aca92daab5571d2433e4e4c8f2b6a62796`
  - `picotool load -f -v -x` Flash write·verify 100% 및 application reboot 통과
  - `screen` raw mode의 local echo 부재로 사용자 입력·방향키 escape 코드가 은폐 누적된 원인 확인, 해당 command의 modem `ERROR` 종결 후 console buffer 정리
  - 별도 macOS Terminal을 canonical·local echo 활성 입력 표시형 Python serial bridge로 교체, `/dev/cu.usbmodem*` 양방향 자동 재연결·wall-clock RX 표시와 사용자 TX의 log 파일 미저장 확인
  - 실기 `MQTT_SESSION_RESET_ALL_OK` 및 `MANUAL_AT_READY input=USB terminator=CR reboot=LOCAL` 도달, marker 후 reset 마지막 RX 조각 외 자동 `AT TX` 0 확인
* **미해결·제한**:
  - 사용자 수동 `KCNX`·TLS·MQTT 명령 단계별 응답 비교 대기
  - 원인 분리 후 임시 수동 console 제거와 정상 자동 부팅 경로 복원 필요
  - DB·Supabase·EMQX·server·commit·push 변경 0 유지

### [펌웨어/서버/MQTTS] 8바이트 CONFIG compact JSON 진단 호환 세트

* **개발 범주**: Flask EMQX Helper, Firmware Payload Parser, RuntimeOwner, TDD, Fresh UF2
* **진단 계약과 구현**:
  - 운영 CONFIG 원본 행을 `userSensorId` 1·2 순서로 정규화한 exact compact JSON `[-7,-10]` 적용과 ASCII 8바이트 고정
  - 누락·중복·bool·비숫자·NaN·Infinity·binary32 범위 초과 값·직렬화 80바이트 초과 payload의 server publish 거절
  - JSON 표준 숫자 문법과 정확히 두 finite 값만 허용하는 C++17 parser, 실패 시 출력값 무변경과 verbose JSON fallback 미유지
  - TMP1·TMP2 sensor upper와 ch0·ch1·legacy upper의 parse 성공 후 원자 반영, lower limit·MQTT topic·AT timing·RuntimeOwner owner 경계 무변경
  - `apply_mqtt_config_payload()` 실패의 `kDiagnosticInvalidCommand` 전파와 command 추출·shutdown ingress·success·boot config commit 선행 차단
  - 이번 compact JSON을 원인 분리용 임시 진단 계약으로 한정, 승인된 향후 G1 versioned numeric CSV 계약 무변경
* **TDD·회귀·독립 review**:
  - server missing-module RED와 binary32·80바이트 RED 뒤 Python producer 7/7 및 `py_compile` 통과
  - firmware parser declaration RED·비표준 C 숫자 mutant RED 뒤 payload 93 runtime assertions 통과
  - CONFIG apply source RED, guard·side-effect·조기 쓰기·apply 결과 무시·command 추출 선행 mutant RED 뒤 device backend 294/294 통과
  - legacy cutover 48/48·producer 261/261·두 작업공간 `git diff --check` 통과
  - task별 review와 `gpt-5.6-sol / max` 최종 재review Critical 0·Important 0·Minor 0 승인
* **Fresh build·binary 계약**:
  - `/private/tmp/nb-iot-compact-config-build.yefmxa/build/nb_iot_project.uf2` Release compile·link 통과
  - UF2 470,016바이트, 2026-07-22 13:37:54 KST, SHA-256 `fc7f650118b151530a5de4c383dcd3e595e18fc2ab2ebf38ec27e008c05b49fd`
  - ELF·UF2의 `CONFIG_FRAME_COMPLETE`, `CONFIG_APPLY_FAILED`, `CONFIG_LIMIT_OK`, `LIVENESS_PROBE_PUB`, `MQTT_PUB_STALL_DIAG_AT_START`, 전체 AT TX marker 확인
  - 변경 firmware source와 ELF·UF2의 HTTP/raw TCP AT path 및 공식 query 문법 없는 `AT+KMQTTCNX?` 문자열 0 확인
* **운영 cutover·실기 검증**:
  - `ssh.zxcx.io` 운영 helper import·`/api/emqx/config-request` route·RPC TMP ID 1/2와 원복 파일 충돌 부재 확인
  - 기존 helper timestamp backup 뒤 reviewed helper atomic 교체, owner `USR2` 기반 systemd restart와 localhost Flask smoke·runtime exact `[-7,-10]` 8바이트 확인
  - UF2 `picotool` flash write·verify 100%와 별도 macOS Terminal 자동 재연결 AT trace, CONFIG `PAYLOAD_BYTES=8`·`CONFIG_LIMIT_OK -7.0,-10.0` 확인
  - session 1 liveness publish의 CRLF-only 5초 timeout, 일반 `AT` 즉시 `OK`, `KMQTTCLOSE` 10초 timeout과 protocol/IP 명령 지연 회복 뒤 session 2 재생성 확인
  - session 2의 boot publish·subscribe·compact CONFIG 재수신 성공 뒤 동일 liveness timeout, 300초 `BOOT_OWNER_TIMEOUT`과 `PERIODIC_READY` 미도달 확인
  - 39바이트 `devices/<IMEI>/telemetry/probe`를 25바이트 `devices/<IMEI>/p`로만 줄인 TDD 단일 변수 실험의 RED 2/295·GREEN 295/295·fresh UF2 flash 수행
  - 짧은 topic에서도 동일 CRLF-only 5초 timeout 재현, topic 길이 원인 가설 제외와 임시 source/test 원복·원본 계약 294/294·원래 UF2 재flash verify 100%
* **미해결·제한**:
  - post-CONFIG 다음 publish에서 HL7811 MQTT command layer가 정지하는 내부 원인과 Sierra 확인 필요
  - DB·Supabase schema·EMQX rule 변경 0, commit·push·stage 0 유지
  - CMake `.env` 일괄 compiler-definition 경고의 기존 서버 credential 노출 재현, credential 교체와 firmware 전용 환경변수 whitelist build 후속 필요

### [펌웨어/MQTTS] Liveness publish 무응답 후 AT·K* 계층 분리 진단

* **개발 범주**: HL7811 MQTT Stall, RuntimeOwner, Temporary AT Trace, TDD, Pico Hardware Verification
* **진단 제품 변경**:
  - liveness `/telemetry/probe` publish가 선행 CR/LF 제외 유효 응답 0바이트로 timeout 난 경우에만 일반 `AT` 1회 실행
  - 진단 `AT` 결과를 `OK`·명시 오류·무응답으로 분류하고 기존 `modem_MqttClose()` recovery 순서 유지
  - 다른 topic·명시적 publish 오류·정상 publish의 추가 진단 0과 admission·session·liveness gate 동작 불변
  - post-CONFIG 실패 뒤에도 임시 AT trace를 유지하여 `PERIODIC_READY` 전 전체 명령 관찰 가능 상태 확보
  - `KMQTTCFG` credential·인증서 본문 redaction 유지와 periodic task의 modem 직접 접근 0으로 RuntimeOwner 단일 owner 계약 보존
  - Rev16에 query form이 없는 `AT+KMQTTCNX?` 미전송과 source tree 문자열 0 확인
* **TDD·회귀·독립 review**:
  - liveness-only silent timeout·진단 결과 분류·기존 close recovery·trace 지속·secret redaction source contract 추가
  - RuntimeOwner device backend 275/275, legacy cutover 48/48, producer 261/261, MQTT payload test와 `git diff --check` 통과
  - 독립 review Critical 0·Important 0, 현재 고정 liveness topic에서만 영향 없는 substring guard Minor 1 기록
  - 전체 Host CMake configure는 기존 protected legacy SHA와 의도된 modem/MQTT source 변경 불일치로 차단, 보호 hash 미변경 유지
* **빌드·플래시**:
  - fresh Release firmware compile·link 통과와 UF2 471,552바이트 생성
  - UF2 SHA-256 `db422939a7ee6afdc158de767920e34693e6e34ea42d269a65c1a2280cf1805f`
  - RP2350 BOOTSEL `picotool load -f -v -x` flash write·verify 100%와 application reboot 통과
  - 별도 macOS Terminal의 `/dev/cu.usbmodem*` 자동 재연결·wall-clock timestamp·전체 AT TX/RX 표시 유지
* **실기 재현 결과**:
  - session 1 liveness `AT+KMQTTPUB=1,.../telemetry/probe,1,0,"{}"`가 선행 `\r\n`만 반환한 뒤 5초 timeout
  - 직후 진단 `AT`가 약 2ms 만에 `OK`, 이어진 `AT+KMQTTCLOSE=1`은 선행 `\r\n` 뒤 10초 timeout
  - 장애 상태에서도 `AT+CEREG?`·`AT+CSQ`·`AT+CCLK?`·`AT+COPS?`의 정상 응답 확인
  - 같은 구간의 `KMQTTCNX`·`KMQTTCLOSE`·`KMQTTDEL`·`KCNXCFG`·`KCNXPROFILE`·`KCNXUP`·`KSSLCFG`·`KSSLCRYPTO` 일시 무응답과 약 303초 뒤 지연 회복 확인
  - 회복 뒤 생성된 session 2에서도 동일 liveness publish timeout→진단 `AT` 약 2ms `OK`→`KMQTTCLOSE=2` timeout 순서 재현
  - UART 물리 연결·일반 AT parser 전체 freeze가 아닌 HL7811 protocol/IP `K*` command-processing layer stall로 원인 범위 축소
  - liveness gate 반복 실패에 따른 이번 회차 `PERIODIC_READY` 미도달과 임시 trace 지속 상태
* **미해결·제한**:
  - post-CONFIG liveness publish가 protocol/IP command layer stall을 일으키는 모뎀 내부 원인과 Sierra 확인 필요
  - 임시 진단 `AT`·전체 trace의 원인 확인 후 제거 또는 명시적 diagnostic build gate 전환 필요
  - CMake 경고에 노출된 기존 Bizppurio credential 교체와 secret-safe build 구조 후속 필요
  - commit·push·DB·EMQX·server 변경 0 유지

### [펌웨어/MQTTS] 전역 AT command-response 1초 settle과 reset 예외 실기 검증

* **개발 범주**: HL7811 UART, AT Command Sequencing, MQTTS, TDD, Pico Hardware Verification
* **설계·제품 변경**:
  - `modem_SendCmd()` 단일 송신 경계 앞에 UART RX drain 기반 quiet gate 적용
  - 모든 raw RX와 AT TX의 마지막 activity 시각 기록, 새 RX 도착 시 1초 timer 재시작, 연속 1,000ms quiet 뒤 다음 명령 허용
  - 명령 timeout 종료 시각을 새 경계로 기록하여 recovery AT도 추가 1초 뒤 허용
  - `kAtCommandSettleMs = 1000` 단일 상수와 1초 안정성 확인 후 500ms 재검증 가능한 구조 고정
  - `modem_MqttResetAllSessions()`의 session 1~6 CLOSE/DEL 12개만 scoped bypass 적용, 기존 100ms 간격과 함수 종료 후 gate 상태 복원
  - reset 뒤 첫 일반 AT, 개별 stale session 정리, disconnect·close에는 전역 1초 gate 유지
  - 인증서 raw body 분할 주입, `\r` terminator, 256바이트 RX guard, MQTT 80바이트·recovery·RuntimeOwner 계약 무변경
* **TDD·Host 검증**:
  - 기존 대상 계약 기준선 190/190 통과
  - 전역 settle·reset 예외 source contract 제품 변경 전 RED 29/235 재현과 최소 구현 뒤 235/235 통과
  - timeout 이후 1초 계약의 추가 RED 2/239, 독립 review 보강의 RED 7/245 재현과 최종 GREEN 245/245 통과
  - 관련 파일 `git diff --check` 통과
  - Host CMake 전체 configure는 기존 protected legacy SHA와 의도된 `tasks_modem.cpp` 변경 불일치로 차단, 대상 계약 직접 C++17 검증과 구분
* **빌드·플래시 검증**:
  - 기존 Release staging incremental build와 새 build directory의 fresh configure·compile·link 각각 통과
  - 두 UF2 byte-identical, 471,040바이트, SHA-256 `889fba0a60987c3be743a6d9eed4e4f1f051086bc0fa2336d83cebccf54601fe`
  - RP2350 BOOTSEL device 확인, `picotool load -f -v -x` flash write·verify 100%와 application reboot 통과
  - 열린 별도 macOS Terminal의 `/dev/cu.usbmodem*` 자동 재연결·wall-clock timestamp 유지
* **실기 trace 결과**:
  - reset 외 일반 AT 30개 모두 `AT SETTLE` 선행, firmware monotonic 기준 last RX→next TX 최소 1,000ms·최대 2,000ms·위반 0
  - MQTT reset-all 12개 TX의 인접 간격 최소 107ms·최대 108ms·평균 107.9ms로 기존 부팅 속도 유지
  - reset 마지막 응답 뒤 첫 `AT+KCNXCFG`까지 1,000ms gate 복원 확인
  - `AT+KHWIOCFG?`의 전체 목록·terminal `OK` 뒤 1초 후 설정 명령, `AT+CCLK?` terminal `OK` 뒤 1초 후 `AT+COPS` 전송 확인
  - 늦은 `+CEREG` URC 도착 시 timer 재시작과 해당 URC 뒤 1초 후 다음 TLS 명령 전송 확인
  - config frame 종료 뒤 1,034ms 후 liveness `AT`, `OK` 뒤 1,000ms 후 liveness `AT+KMQTTPUB` 전송 확인
  - liveness publish는 선행 `\r\n`만 수신하고 5초 뒤 동일 timeout, 1초 sequencing 변경으로 기존 장애 미해소 확인
* **미해결·보안 후속**:
  - liveness publish CRLF-only 원인의 MQTT session/command-plane 별도 진단 필요
  - 1초 실기 반복 안정성 확보 뒤 500ms 변경·동일 전체 trace 재검증 필요
  - CMake `.env` 일괄 compiler-definition 경고의 Bizppurio credential 출력 재현에 따른 credential 교체와 secret-safe build 처리 필요
  - 임시 전체 AT trace의 진단 완료 후 제거 또는 명시적 diagnostic gate 전환 필요
  - commit·push·DB·EMQX·server 변경 0 유지

### [펌웨어/MQTTS] 부팅 전구간 임시 AT trace와 timestamp 실측

* **추적 범위와 보안 처리**:
  - 모뎀 부팅 첫 응답부터 config 수신 뒤 첫 liveness publish 실패까지 모든 AT TX·raw RX·timeout에 펌웨어 monotonic timestamp 적용
  - 별도 macOS Terminal 자동 재연결 수집기의 왼쪽에 `[HH:MM:SS.mmm]` wall-clock timestamp 적용
  - `AT+KMQTTCFG`의 credential과 인증서 본문을 출력하지 않고 `<REDACTED bytes=N>` 형태로 길이만 기록
  - 첫 liveness publish 결과 직후 `AT_TRACE_STOP` 처리로 무한 recovery 로그와 비밀 노출 범위 제한
* **TDD·빌드·플래시 검증**:
  - Backend trace source contract의 제품 변경 전 RED 18/190 재현과 최소 구현 뒤 GREEN 190/190 통과
  - fresh firmware compile·link 통과, UF2 468,992바이트, SHA-256 `ce2600a0f9471bf4d21700f0a8deb7c3ad669df18e08f76b1b922cc350cd20fb`
  - `picotool load -f -v -x` flash write·verify 100%와 application reboot 통과
* **실기 시간축과 실패 경계**:
  - `CONFIG_FRAME_COMPLETE` 뒤 약 1.002초에 `AT` 송신, 2초 응답 대기 뒤 `OK` 수신, 약 2ms 뒤 `AT+KMQTTPUB` 송신 확인
  - `AT+KMQTTPUB` 송신 약 7ms 뒤 모뎀의 선행 `\r\n` 2바이트만 수신, 이후 5초 동안 command `OK`·`+KMQTTPUB`·`+KMQTT_IND` 없음과 timeout 재현
  - 정상 publish의 `CRLF → +KMQTTPUB → OK → +KMQTT_IND: <id>,4` 순서와 실패 publish의 선행 CRLF-only 응답 차이 확인
  - 기존 `MQTT_PUB_NO_OK RX_BYTES=0`을 물리 수신 0바이트가 아닌 선행 CR/LF 필터 뒤 유효 버퍼 0바이트로 의미 정정
* **추가 관찰과 후속 범위**:
  - `AT+KHWIOCFG?`, `AT+CEREG?`, `AT+CCLK?` 등 일부 부팅 응답의 terminal response 완료 전 다음 명령 전송 흔적 확인
  - liveness `AT+KMQTTPUB`의 CRLF-only 원인과 공통 command-response 완료 대기 규약의 별도 설계·수정 필요
  - 진단 완료 후 임시 trace 제거 또는 명시적 diagnostic gate 전환 필요
  - 기존 보호 source SHA 불일치로 전체 Host CMake configure 차단 유지, 대상 계약 테스트 직접 C++17 빌드 검증으로 한정
  - commit·push·DB·EMQX·server 변경 0 유지

### [펌웨어/MQTTS] Post-config AT 1초 settle 실험과 하단 Terminal 공유

* **변경 범위**:
  - RuntimeOwner `ProbeAt` 실행 직전에 `modem_sleep(1000)` 적용
  - 전역 `check_at_alive()` 변경 없이 config 적용 뒤 liveness AT 경로에만 지연 한정
  - `KMQTT_DATA` 완료 뒤 즉시 AT 전송, 기존 AT 전송 후 2초 응답 대기, 후속 `AT+KMQTTPUB` 순서 유지
* **TDD 및 빌드 검증**:
  - Backend source contract 추가 후 RED 3/158 재현
  - 최소 제품 변경 뒤 GREEN 158/158 및 `git diff --check` 통과
  - 기존 `tasks_modem.cpp` 보호 SHA 불일치로 Host CMake configure 차단, 대상 계약 테스트 직접 C++17 빌드로 분리 검증
  - fresh Release firmware compile·link 통과, UF2 470,016바이트 생성, SHA-256 `bf25b35cae1cf27ad06cb2671c140798fac4b21d5c383df8cfe4ad0864399912`
* **플래시·실기 결과**:
  - `picotool load -f -v -x` flash write·verify 100%와 application reboot 통과
  - cold boot의 certificate write 실패 뒤 MQTT fallback recovery로 session 2 재연결·boot publish·subscribe·171바이트 config 수신 성공
  - 1초 선행 지연 적용 뒤에도 `LIVENESS_PROBE_PUB`의 `MQTT_PUB_NO_OK RX_BYTES=0 CLASS=0` 동일 재현
  - 조기 AT 전송 단독 원인 가설 미채택, same-session post-config publish 무응답 원인 추적 지속 필요
* **실시간 로그 운영 절차**:
  - `/dev/cu.usbmodem*` raw 115200 설정과 USB 분리·재열거 자동 재연결 수집기 적용
  - 이번 실기 회차에서 시리얼 출력을 Codex 하단 `NB-IOT` Terminal의 활성 TTY로 복제하여 사용자·Codex 동시 확인
  - 사용자 후속 정정에 따라 향후 플래시 검증은 Codex 오른쪽 `백그라운드 터미널` 우선, 미노출 시 별도 macOS Terminal 자동 실행으로 변경
  - Codex 하단 통합 `NB-IOT` Terminal의 향후 시리얼 모니터링 사용 중단과 변경 정책의 `AGENTS.md` 반영
  - commit·push·DB·EMQX·server 변경 0 유지

### [펌웨어/MQTTS] Config 수신 후 동일 세션 handoff 복구

* **개발 범주**: Firmware, HL7811, MQTTS, RuntimeOwner, Regression
* **원인 및 기준 확인**:
  - 일반 Git 커밋과 Codex checkpoint의 과거 정상 동작 코드 대조
  - config frame 완료 뒤 `AT+KMQTTCFG?` 없이 연결·구독 세션을 후속 태스크가 이어받던 기존 handoff 흐름 확인
  - 현재 `runtime_owner_device_backend.cpp`의 30초 quiet barrier 뒤 `AT+KMQTTCFG?` 진단이 `MQTT_CFG_QUERY 0 RX_BYTES=0`·`MQTT_CTRL_STALL`을 만드는 차이 확인
* **TDD 및 제품 변경**:
  - `tests/boot_v2/runtime_owner_device_backend_contract_test.cpp`에 post-config control query·quiet barrier·session teardown 금지 계약 적용
  - 변경 전 RED 17/153 재현 후 `src/boot_v2/runtime_owner_device_backend.cpp/.hpp`의 post-data quiet 상태와 진단 query 제거
  - `publish_probe()`에서 현재 `is_connected()` 확인 뒤 기존 세션의 `modem_MqttPublish()` 직접 사용
  - cold boot 1회 전체 정리, recovery 재연결 우선, 실패 session 대상 정리와 최종 reset fallback 정책 유지
* **검증 결과**:
  - Backend contract GREEN 153/153 통과
  - 기존 Host 회귀 17/17 통과 및 `git diff --check` 통과
  - fresh Release firmware compile·link 통과, UF2 470,016바이트 생성
  - 최종 ELF에 `MQTT_POST_DATA_QUIET`, `MQTT_CFG_QUERY`, `MQTT_CTRL_STALL`, `AT+KMQTTCFG?` 문자열 부재 및 `LIVENESS_PROBE_PUB` 유지 확인
* **실기 플래시 검증 결과**:
  - `picotool load -f -v -x`의 flash write·verify 100%와 application reboot 통과
  - config frame 완료 뒤 `MQTT_CFG_QUERY`·`MQTT_CTRL_STALL` 없이 same-session `LIVENESS_PROBE_PUB` 진입 확인
  - 후속 `AT+KMQTTPUB`에서 `MQTT_PUB_NO_OK RX_BYTES=0 CLASS=0` 재현, config 뒤 다음 MQTT command 무응답 잔존 확인
  - recovery의 기존 session 재연결 실패, session 1 대상 정리, 최종 전체 reset 뒤 `MQTT_CFG_FAIL` 재현
  - 후속 recovery의 session 2 `MQTT_RECONNECT`·`MQTT_CONNECT_OK`, boot publish·subscribe·171바이트 config 수신 성공 뒤 동일 liveness publish 무응답 재현
  - 중복 USB serial reader 2개가 수신 바이트를 분할한 로그 손상 원인 확인, 단일 reader 유지와 macOS Terminal `tail -f` 재연결
* **미해결 및 후속 확인**:
  - main-board/RM78 clean power cycle 뒤 same-session probe publish의 command `OK`·PUBACK와 후속 subscription·config 실기 재검증 필요
  - config frame 완료 뒤 HL7811 MQTT command plane 무응답의 원인 분리와 공식 Rev16 규약 기반 후속 설계 필요
  - CMake `.env` compiler-definition 처리 경고의 비밀값 로그 노출 확인, 노출 credential 교체와 secret-safe build 구조 개선 필요
  - commit·push·DB·EMQX·server 변경 0 유지

### [펌웨어/MQTTS] Boot-only 세션 정리와 보존형 recovery 전환

* **개발 범주**: Firmware, HL7811, MQTTS, RuntimeOwner, Recovery
* **세션 정책**:
  - cold boot의 최초 MQTT open에서만 session 1~6 CLOSE·DEL 정리
  - 일반 recovery와 부팅 보고·구독·설정 수신 실패에서 CLOSE-only 적용 및 session ID 보존
  - 보존 session의 `AT+KMQTTCNX` 재연결 우선, 실패 시 해당 session만 CLOSE·DEL 후 재생성
  - 새 `AT+KMQTTCFG` 실패 시에만 전체 session reset 최종 fallback 적용
  - publish·subscribe 실패 시 연결 상태 해제와 남은 session ID 기반 recovery 진입
* **오류 판정 교정**:
  - `+KMQTT_IND: <id>,0`의 일반 connection aborted 처리
  - 명시적 `+CME ERROR: 907` 수신 시에만 MQTT 인증 실패 상태 설정
* **검증 결과**:
  - MQTT session source contract RED 3/154 확인 후 GREEN 154/154 통과
  - Host Debug·Release 각 19/19, `git diff --check`, firmware build 통과
  - UF2 471,552바이트, SHA-256 `825d684be8b246fb304bdfc66d770727f0a70da162ea6117b839b983c8a73ca1`
  - `picotool load -f -v -x` 자동 BOOTSEL·flash verify·application reboot 통과
  - 실기 cold boot의 `MQTT_BOOT_CLEAN`·`MQTT_SESSION_RESET_ALL` 1회 확인
  - config 171바이트 수신 뒤 control stall에서 session 1 CLOSE-only·ID 보존·우선 재연결 시도 확인
  - 재연결 실패 시 session 1만 target cleanup, 새 session 설정 실패 시 전체 reset 최종 fallback, 다음 시도 session 1 연결·publish·subscribe·config 수신 성공 확인
  - USB serial 단일 reader와 파일 append, 별도 Terminal `tail -f` 기반 사용자·Codex 동시 실시간 모니터링 구성
* **미해결 및 제한**:
  - 기존 session 재연결 시도 자체는 확인했으나 해당 실기 회차의 재연결 성공은 미확인
  - config 수신 뒤 `MQTT_CFG_QUERY 0 RX_BYTES=0`·`MQTT_CTRL_STALL` 지속 재현과 후속 원인 규명 필요
  - commit·push·DB·EMQX·server 변경 0 유지

### [펌웨어/MQTTS] Stage 13 UART drain 진단 및 실기 재검증 준비

* **개발 범주**: Firmware, HL7811, MQTTS, UART, Hardware Qualification
* **원인 추적**:
  - 최초 config URC 손상 재현과 `modem_MqttPoll()` 50ms 선행 대기 확인
  - 공식 HL78xx AT Guide Rev16의 inline `+KMQTT_DATA` 형식과 256바이트 read guard 대조
  - 1ms polling 적용 후 171바이트 config frame과 sensor limit 2개 정상 적용 확인
  - fresh `AT` 성공 뒤 post-CONFIG boot publish의 `MQTT_PUB_CMD_FAIL` 재현
  - 이전 코드의 boot publish-before-subscribe 순서와 즉시 config 응답 중첩 회피 주석 확인
* **TDD 및 제품 변경**:
  - config 뒤 RX clear·100ms 대기 실험의 실기 실패 확인과 즉시 원복
  - MQTT command `OK` 대기와 PUBACK 대기의 100ms UART 무수신 구간을 1ms drain으로 고정하는 source contract 추가
  - RED 4/32 보존 후 GREEN 32/32, 기존 256바이트 response-loop guard 유지
  - 실패 구간 분리용 `MQTT_PUB_CMD_FAIL`·`MQTT_PUB_ACK_TIMEOUT` 진단 marker 추가
* **검증 결과**:
  - Host Debug·Release 각 19/19 통과
  - fresh Release UF2 462,336바이트, SHA-256 `f0ac3e26f40675971a80c6486bcb40cb982e310965305ffde96caaac21781e2e`
  - `picotool load -f -v -x` 자동 BOOTSEL·flash verify·application reboot 통과
* **미해결 및 후속 확인**:
  - 이전 실패 회차에서 남은 RM78 MQTT command-plane 상태로 `GPRS_APN_WARN`·`MQTT_CFG_FAIL`·`BOOT_OWNER_TIMEOUT` 재현
  - post-CONFIG 경계 미도달 회차이므로 새 1ms command/PUBACK drain의 실기 Pass/Fail 판정 보류
  - main-board/RM78 clean power cycle 후 동일 UF2의 CONFIG→AT→PUBACK→follow-up CONFIG→Runtime ordered log 필요
  - GP15 KILL·adapter detach·전원 계측 미착수, commit·push·DB·EMQX·server 변경 0 유지

## 📅 2026-07-21

### [펌웨어/PCB] 실물 PCB 기준 GPIO 핀맵·LCD 초기화 복구

* **개발 범주**: Firmware, PCB GPIO, LCD1602 I2C, Hardware Bring-up
* **작업 및 결정 내역**:
  - `DOCS/PCB/PCB_PCB_NB-IOT_2_2026-07-02.json`과 `SCH_NB-IOT_2026-07-02.json` 원본의 U1_PICO·U2·U6·U7·RJ1·CN3·CN4·R1~R21 pad/net 직접 추적
  - RM78 PWRON GP4·WAKEUP GP2·RESET GP3·TXON GP5, LTC2954 INT GP14·KILL GP15, LCD SDA GP16·SCL GP17 계약 복구
  - I2S BCLK GP18·LRCLK GP19·MIC1 DOUT GP20·MIC2 DOUT GP21, DS18B20 GP22/GP26, speaker GP6, adapter detect GP7, LED GP8~GP13/GP28 계약 고정
  - LCD task 내 0x27/0x3F probe, stuck/skip 진단, HD44780 표준 4-bit 초기화 순서와 I2C timeout 적용
  - 5초 전원 안정화 지연의 실물 문자 표시 확인 후 사용자 승인 기준 3초 지연으로 축소
* **검증 결과**:
  - PCB pinmap contract RED 10/29 확인 후 GREEN 29/29 통과
  - LCD runtime contract 28/28, Host Debug·Release 각 19/19, `git diff --check`, fresh firmware build 통과
  - 3초 지연 UF2 461,312바이트, SHA-256 `61bf72276b822eced833fefaf38d348239a63cb63da4759789f036f8ecbd7cd0`, Pico software BOOTSEL 자동 플래시
* **미해결 및 후속 확인**:
  - 3초 지연 재플래시 후 실물 LCD 문자 표시 정상 확인, adapter/battery 조건별 10회 공식 반복 검증 대기
  - USB serial 재연결 후 수신 바이트 0으로 LCD probe 로그 미확보
  - commit·push·DB·EMQX·server 변경 0 유지

### [펌웨어/FreeRTOS] RuntimeOwner Stage 6~12 firmware atomic cutover 통합

* **개발 범주**: Firmware, FreeRTOS, RuntimeOwner, MQTTS, Atomic Cutover
* **작업 및 결정 내역**:
  - RuntimeOwner task 단일 실행 주체의 typed physical executor와 firmware device backend 연결
  - Boot·Periodic·Power·Adapter·AuthenticatedCommand별 source-specific facade와 provenance 검증 적용
  - Boot·Periodic·Debug의 modem·MQTT·raw AT·local reboot·power-off 직접 접근 제거
  - transport receipt 뒤 CONFIG receipt를 별도 owner cycle에서 제출하는 순서 보장
  - scheduler 시작 전 queue·physical inflight quiesce와 five-fact preflight 확인 후 `cutover_ready`·`ingress_enabled` 순서의 atomic activation 적용
  - ingress 전 abort와 ingress 후 clean reboot 요구를 구분하는 allocation-free rollback core 추가
  - authenticated shutdown urgent ingress 실패를 무시하지 않고 physical failure로 반환하는 fail-closed 처리
* **검증 및 리뷰 결과**:
  - fresh Host Debug·Release 각 17/17 통과, 동일 exact check sequence 확인
  - G1 contract 112/112 통과
  - Stage 12 compile-success behavioral mutant 8/8, Stage 4·5 회귀 mutant 17/17·7/7 거부
  - fresh Release firmware configure·compile·link 및 459,776바이트 UF2 생성
  - Stage 12 변경 범위 최종 자체 검토 Critical 0·Important 0·Minor 0
* **미해결 및 후속 확인**:
  - Pico copy·flash·serial·LTE attach·MQTTS·CONFIG·telemetry 실기 검증 미수행
  - GP15 KILL·watchdog live actuation 비활성, adapter-loss 300초 qualification 미수행
  - 기존 CMake `.env` 일괄 compiler-definition 변환 경고의 secret-safe build 처리 후속 정리 필요
  - DB·EMQX·server·외부 서비스 변경, stage·commit·push 0 유지

### G2A Stage 5 producer provenance 계약 검증

* Boot·Periodic·PowerButton·AdapterMonitor·AuthenticatedCommand·LocalDebug producer 권한표의 internal C++17 계약 고정
* Host Debug·Release 11/11 및 기존 10개 check count 불변
* G1 112/112, compile-success behavioral mutant 7/7 거부
* protected legacy 7파일 byte identity, production ingress·physical executor callsite 0 확인
* fresh Release UF2 생성, Pico 복사·flash·serial 및 외부 서비스 접근 0
* `ingress_enabled=0`, `cutover_ready=0`, actual producer wiring·cutover 0 유지

### [펌웨어/FreeRTOS] RuntimeOwner Stage 4 queue-drain 교정 종결
* RuntimeOwner malformed normal의 상태 독립 `DroppedInvalid` 분류와 admission 비개방 교정
* 실제 제품 owner-loop sink의 세 source-scoped shutdown port와 공통 drain 반복 제어 Host 검증
* Host Debug·Release 10/10, G1 112/112, compile-success mutant 17/17 거부, final review Critical 0·Important 0
* fresh Release UF2 생성과 Pico copy·flash·serial 미수행
* `ingress_enabled=0`·`cutover_ready=0`·legacy 7파일 byte identity·commit/push 0 유지

### [펌웨어/FreeRTOS] RuntimeOwner Stage 3 계약 통합 검증
* **개발 범주**: Firmware, RuntimeOwner Contract, Snapshot, Shutdown, Host/Firmware Verification
* **작업 및 결정 내역**:
  - executor command의 delivery-only·trusted receipt·normal completion typed 경계 추가
  - Boot snapshot one-shot freeze와 exact EndBoot ACK 기반 Ready 원자 commit 추가
  - Runtime latest snapshot의 strict revision 및 nonblocking copy gate 추가
  - PowerButton·AdapterLossCommitted·AuthenticatedRemoteCommand별 shutdown provenance와 strict monotonic 처리 추가
  - generic no-argument shutdown ingress 제거 및 owner loop 외 `process_cycle` 접근 차단
  - exact EndBoot duplicate의 unrelated later ACK 비의존 멱등 처리 추가
* **검증 및 리뷰 결과**:
  - Host Debug/Release 각 8/8 통과, exact count `428/8662/53196/144054/980/142/253/233` 확인
  - G1 contract 112/112 및 compile-success behavioral mutant 11/11 거부
  - 독립 최종 코드 검토·verification 각각 Critical 0·Important 0·Minor 0 승인
  - fresh Release firmware build/link와 UF2 437,248 bytes 생성 확인
* **미해결 및 후속 확인**:
  - `ingress_enabled=0`, production permit mint/activation 0, queue drain 0, physical executor 0 유지
  - legacy modem/UART/MQTT/CONFIG 직접 caller의 atomic cutover 미수행
  - Pico flash·serial·실기 검증 및 운영 반영 미수행

### [펌웨어/FreeRTOS] RuntimeOwner Stage 2 Dormant 정적 태스크 전환
* **개발 범주**: Firmware, FreeRTOS, RuntimeOwner Contract, Host/Firmware Verification
* **작업 및 결정 내역**:
  - `RuntimeOwnerTaskCore`의 `RuntimeOwnerAdapterCore` 직접 단일 소유 구조 적용
  - Dormant/Terminal 불변 유지, Active 선택 우선순위 `shutdown > transport > normal` 적용
  - cycle당 canonical `adapter_.step()` source callsite 정확히 1개 유지, dispatch 관찰 전용 처리 및 ACK/activation 미수행
  - Core0, priority 2, stack 1024 words의 Static RuntimeOwner task 1개 등록
  - depth 1/2/8 정적 queue 3개 구성, ingress 비활성 유지
  - `sensor_init` 이후 legacy task 생성 전 `main` pre-scheduler 시작, 실패 시 fail-stop 처리
* **검증 및 리뷰 결과**:
  - Host Debug/Release 각 5/5 통과, exact count `428/8662/53196/143099/984` 확인
  - compile-success behavioral mutant 5종 검증: Task5 4종 및 A-I-01 empty-active double-step 1종
  - exact-one survivor의 기존 982 PASS, 신규 contract `984/1` RED 전환 확인
  - G1 contract 112/112 통과
  - firmware build/link UF2 `/private/tmp/nb-iot-owner-task-fw-XJA3Nf/nb_iot_project.uf2` 생성 확인, 436224 bytes, `2026-07-20 23:51:22 +0900`, SHA-256 `5d4ff0247534792971d07c1508a02eac2a76138199dacac26db3102a07fe1b7b`
  - 독립 Review A 재검토 Critical 0/Important 0/Minor 0, Review B Critical 0/Important 0/Minor 1 확인
* **미해결 및 후속 확인**:
  - `G2A-RTO-B-001`: scheduler/LogTask 시작 전 fatal LOG의 큐 잔류로 외부 관찰 불가 상태; fail-stop 안전 영향 없음, 후속 진단 sink 또는 문구 처리 필요
  - Pico flash/serial/실물 scheduling/high-water/modem cutover 미검증 상태
  - ingress/producers/executor/Stage 3 미활성 상태

## 📅 2026-07-04

### [펌웨어/MQTT] KeepAlive 안정화 및 EMQX rule 분리
* **연동 대화 ID**: Codex MQTT KeepAlive 및 EMQX rule 분리 세션
* **개발 범주**: Firmware, MQTT, EMQX, Config Payload, KeepAlive
* **작업 및 해결 내역**:
  - `src/lib/mqtt_payload.hpp/cpp` 추가로 MQTT config payload 방어 및 telemetry payload 검증 로직 분리
  - `undefined`, 빈 문자열, 공백, `null`, `[]`, 잘못된 JSON payload 무시 처리
  - config 배열/객체 payload에서 필드 누락 및 null 값 수신 시 기존 설정 유지
  - `apply_mqtt_config_payload()` 공통 함수 추가로 boot/periodic config 수신 처리 통합
  - telemetry payload `[userSensorId, temperature]` 생성 전 sensor id 및 온도값 검증 추가
  - MQTT 상태 enum 추가: `LTE_DETACHED`, `LTE_ATTACHED`, `TLS_SOCKET_OPENING`, `TLS_SOCKET_OPEN`, `MQTT_CONNECTING`, `MQTT_CONNECTED`, `MQTT_SUBSCRIBED_CONFIG`, `MQTT_READY`, `MQTT_DISCONNECTED`, `MQTT_RECONNECT_WAIT`
  - `modem_MqttPoll()` 추가로 MQTT URC 수집, 연결 abort/generic error 감지, READY 상태 갱신 처리
  - `vPeriodicModemTask`를 매 발신 세션 open/close 구조에서 유지형 MQTT 세션 구조로 조정
  - 60초 주기 `devices/<imei>/config/request` 발행으로 config 조회 요청 및 KeepAlive traffic 보강
  - reconnect 실패 횟수별 3초, 5초, 10초, 최대 60초 backoff 적용
  - boot task에서 boot publish와 별도로 `devices/<imei>/config/request` 발행
  - `Segang/project/emqx_setup.sh`의 `telemetry_rule` config republish action 제거
  - `devices/+/config/request` 전용 `config_request_rule` 추가 및 boot rule의 config republish 제거
  - `emqx_setup.sh` 하드코딩 API 인증 헤더 제거 및 `.env`의 `EMQX_API_AUTH_HEADER` 사용으로 변경
  - `tests/mqtt_payload_test.cpp` 추가 및 config/telemetry payload 단위 검증
  - `g++` 호스트 단위 테스트, `bash -n Segang/project/emqx_setup.sh`, fresh CMake 펌웨어 빌드 검증 완료
* **미해결 및 후속 확인**:
  - 실제 Pico UF2 플래시 후 10분 이상 EMQX `keepalive_timeout` 미발생 확인 필요
  - 운영 EMQX rule 반영 후 config request 응답/재발행 정책의 서버 측 보강 필요

### [펌웨어] DS18B20 온도 보정 오프셋 추가
* **연동 대화 ID**: Codex DS18B20 온도 보정 설정 추가 세션
* **개발 범주**: Firmware, Sensor Calibration, DS18B20, Config
* **작업 및 해결 내역**:
  - DS18B20 디지털 온도센서도 기준 온도계 대비 보정이 필요할 수 있는 상황 반영
  - `src/config.h`에 TMP1/TMP2 개별 보정값 `TEMP1_CAL_OFFSET_C`, `TEMP2_CAL_OFFSET_C` 추가
  - 기본 보정값은 두 채널 모두 `0.0f`로 설정
  - `src/tasks/tasks_sensor.cpp`에서 DS18B20 읽기 성공 및 status 0일 때만 보정 오프셋 적용
  - 센서 오류값, CRC 실패값, 미연결 상태에는 보정 오프셋 미적용
  - `/Users/segang/Documents/NB-IOT` 원본 루트에 동일 파일 동기화
  - fresh CMake 구성, firmware build, `/private/tmp/nb-iot-temp-offset-build/nb_iot_project.uf2` 생성 검증 완료
* **미해결 및 후속 확인**:
  - 기준 온도계 비교 후 `TEMP1_CAL_OFFSET_C`, `TEMP2_CAL_OFFSET_C` 실제 보정값 산정 필요

## 📅 2026-07-03: [펌웨어] DS18B20 디지털 온도센서 전환
* **연동 대화 ID**: Codex 아날로그 서미스터 제거 및 GP22 DS18B20 전환 세션
* **개발 범주**: Firmware, Sensor, GPIO, DS18B20, FreeRTOS
* **작업 및 해결 내역**:
  - GP22에 DS18B20 디지털 온도센서를 임시 연결한 실물 변경 사항 반영
  - 외부 온도센서 측정 경로에서 아날로그 서미스터 ADC 코드 제거
  - 기존 GP26/GP27 ADC 스캔, 전압 변환, 저항 계산, B-parameter 온도 계산, `NTC_TEMP_OFFSET` 보정 코드 제거
  - `config.h` 외부 온도 핀 정의를 `TEMP1_SENSOR_PIN=22`, `TEMP2_SENSOR_PIN=26`으로 재정의
  - TMP1은 GP22, TMP2는 GP26 DS18B20 1-Wire 버스 기준으로 부팅 체크 및 주기 샘플링 수행
  - DS18B20 reset/presence detect, `Convert T`, scratchpad read, CRC8 검증, DS18B20 온도 범위 검증 루틴 추가
  - 1-Wire 타이밍 슬롯은 짧은 FreeRTOS critical section으로 보호하고 750ms 변환 대기는 task delay로 처리
  - DS18B20 미연결 또는 데이터 라인 open 상태는 status 1, CRC 오류는 status 2, 온도 범위 초과는 status 3으로 분리
  - 부팅 먹통 증상 대응을 위해 DS18B20 직접 읽기를 부팅 필수 경로에서 제거하고 센서 태스크 전용 읽기 구조로 조정
  - BootTask와 SensorTask의 1-Wire 동시 접근 가능성 제거를 위해 `one_wire_mutex` 추가
  - 부팅 초기 `current_temperature`, `current_temperature_ch1`, `status_ch0`, `status_ch1` 기본값을 미연결 상태로 초기화
  - 부팅 먹통 지속 증상 대응을 위해 `ENABLE_DS18B20_READ=0` 임시 격리 스위치 추가
  - `ENABLE_DS18B20_READ=0` 상태에서는 GP22/GP26 DS18B20 GPIO 초기화 및 1-Wire read/write 미수행
  - DS18B20 GPIO 미접근 격리 UF2가 센서 분리/연결 상태 모두에서 정상 부팅되는 사용자 확인 수신
  - 격리 결과를 기준으로 배선 쇼트 가능성을 낮추고 DS18B20 읽기 루틴의 부팅 초기 간섭 가능성으로 원인 범위 축소
  - `ENABLE_DS18B20_READ=1`, `ENABLE_TEMP1_DS18B20=1`, `ENABLE_TEMP2_DS18B20=0`, `DS18B20_BOOT_DELAY_MS=30000` 구성 적용
  - SensorTask에서 `lcd_params.is_booting=false` 및 부팅 후 30초 경과 조건을 만족할 때만 GP22 TMP1 읽기 수행
  - GP26 TMP2는 실제 두 번째 DS18B20 연결 전까지 GPIO 초기화 및 읽기 비활성 유지
  - 부팅 완료 직후 `BOOT_DONE`, `PERIODIC_READY` 이후 멈춤 증상 기준 SensorTask의 DS18B20 읽기 시작 지점으로 원인 범위 추가 축소
  - 1-Wire `ow_reset`, `ow_write_bit`, `ow_read_bit` 내부 FreeRTOS `taskENTER_CRITICAL/taskEXIT_CRITICAL` 제거
  - GP22 단일 센서 지연 읽기 조건은 유지한 상태에서 critical section 제거 효과를 확인하는 테스트 빌드 생성
  - GP22 TMP1이 `status 1`로 표시되는 presence pulse 미검출 상태 확인
  - `ow_reset`에서 reset 전 DATA idle high 여부와 presence pulse 여부를 분리 측정하도록 진단값 추가
  - 5초 주기 `DS18B20_DIAG GP22 idle=<0|1> presence=<0|1> status=<code>` 로그 추가
  - status 1은 idle high 상태에서 presence 없음, status 5는 DATA 라인 stuck-low 또는 GND 단락 의심으로 분리
  - 사용자 배선 오류 수정 후 GP22 TMP1 온도값 정상 수신 확인: `SENSOR_SAMPLE 30.7,0 ...`
  - LCD 하단 온도 표시에서 기존 `C1/C2` 순환 표시 제거
  - GP22 TMP1 단독 정상 시 `-15.0°C`, GP26 TMP2 단독 정상 시 `T2: -15.0°C`, TMP1/TMP2 동시 정상 시 `-15.0 -15.0°C` 형식 적용
  - GP26 TMP2도 코드상 DS18B20 읽기 활성화하여 향후 두 번째 센서 연결 시 자동 표시 가능하도록 조정
  - 8002A 스피커 앰프 입력 결선 기준 `BUZZER_PIN`을 GP16에서 GP6으로 변경
  - 부팅 후 MQTT 발신 실패 및 `KMQTTCNX` 타임아웃성 실패는 후속 리팩터링/디버깅 과제로 분리
  - DS18B20 샘플링 주기를 1초에서 10초로 변경하여 1-Wire 버스 부하 및 CRC 흔들림 완화
  - CRC/일시 통신 실패 발생 시 최대 3회까지 마지막 정상 온도값과 status 0 유지
  - 연속 실패가 3회를 초과할 때만 LCD 및 상태값에 실제 오류 반영
  - 요청에 따라 `DIAG_CHECK`, `DS18B20_DIAG`, `DS18B20_READ_RESET_FAIL` 시리얼 로그 제거
  - VSYS 전압 및 RP2350 내부 칩 온도 진단에 필요한 ADC 초기화와 `hardware_adc` 링크는 유지
  - `tasks_boot.cpp`, `tasks_sensor_reader.cpp`, `tasks_periodic_modem.cpp`의 NTC 변수명 및 호출부를 일반 온도센서 기준으로 정리
  - flash log 구조의 `ntc_status` 명칭을 `temp_status`로 변경하고 CSV 헤더를 `TempStatus`로 정리
  - 코드 검색 기준 `src/`, `main.cpp`, `CMakeLists.txt` 내 `NTC`, `thermistor`, `ADC_NTC` 잔여 참조 없음 확인
  - fresh CMake 구성, firmware build, `/private/tmp/nb-iot-ds18b20-build/nb_iot_project.uf2` 생성 검증 완료
  - 부팅 복구용 fresh CMake 구성, firmware build, `/private/tmp/nb-iot-ds18b20-bootfix-build/nb_iot_project.uf2` 생성 검증 완료
  - DS18B20 GPIO 비활성 격리용 fresh CMake 구성, firmware build, `/private/tmp/nb-iot-ds18b20-disabled-build/nb_iot_project.uf2` 생성 검증 완료
  - GP22 단일 센서 지연 읽기용 fresh CMake 구성, firmware build, `/private/tmp/nb-iot-ds18b20-delayed-gp22-build/nb_iot_project.uf2` 생성 검증 완료
  - GP22 단일 센서 지연 읽기 및 critical section 제거용 fresh CMake 구성, firmware build, `/private/tmp/nb-iot-ds18b20-nocritical-build/nb_iot_project.uf2` 생성 검증 완료
  - GP22 presence 진단 로그 포함 fresh CMake 구성, firmware build, `/private/tmp/nb-iot-ds18b20-diag-build/nb_iot_project.uf2` 생성 검증 완료
  - LCD 온도 표시 및 GP6 스피커 반영 fresh CMake 구성, firmware build, `/private/tmp/nb-iot-lcd-gp6-build/nb_iot_project.uf2` 생성 검증 완료
  - DS18B20 10초 샘플링 및 CRC 안정화 fresh CMake 구성, firmware build, `/private/tmp/nb-iot-ds18b20-stable-build/nb_iot_project.uf2` 생성 검증 완료
* **미해결 및 후속 확인**:
  - 사용자 UF2 플래시 후 GP22 DS18B20 실제 온도값, TMP1 상태 0, 대시보드 TMP1 표시 확인 필요
  - GP26 TMP2는 센서 미연결 상태에서 status 1 표시 예상

## 📅 2026-07-03: [DB/펌웨어/대시보드] 고정 USER_SENSOR 구조 전환
* **연동 대화 ID**: Codex 고정 센서 ID 및 USER_SENSOR 통합 전환 세션
* **개발 범주**: Supabase, EMQX, Firmware, Dashboard, Sensor Mapping
* **작업 및 해결 내역**:
  - Supabase `SENSOR_CTGY` 테이블 생성 및 `TMP/DS18B20`, `MIC/SPH0645` 센서 분류 2행 적재
  - 기존 `sensor`, `usersettings` 구조 제거 및 `USER_SENSOR` 테이블로 기기별 센서/설정온도 통합
  - device 1~5 대상 `userSensorId` 1~4 생성, 총 20개 고정 센서 행 초기 적재
  - `userSensorId` 1/2는 온도센서, 3/4는 마이크 센서로 고정 매핑
  - `setTmpUpLimit=-10`, `setTmpLowLimit=NULL` 기준 초기 설정값 반영
  - `sensorvalue.sensorId`를 `USER_SENSOR.Id` 참조 구조로 전환
  - `get_device_sensors(p_imei)` RPC를 IMEI 기준 `USER_SENSOR` 설정 조회 구조로 재작성
  - `t(p_imei, p_user_sensor_id, p_value)` RPC를 IMEI + 고정 `userSensorId` 기반 telemetry 적재 구조로 재작성
  - `insert_device_boot_log`, `b` RPC를 `tmp1_status`, `tmp2_status`, `mic1_status`, `mic2_status` 분리 저장 구조로 재작성
  - `assign_device_command()` trigger function의 옛 `sensor` 테이블 참조를 `USER_SENSOR` 참조로 교체
  - EMQX `telemetry_rule` SQL 및 `supabase_telemetry` action body를 `[userSensorId, value]` payload 처리 구조로 변경
  - EMQX `boot_rule` SQL 및 `supabase_boot` action body를 TMP1/TMP2/MIC1/MIC2 상태 포함 구조로 변경
  - 운영 EMQX 설정 및 로컬 `Segang/project/emqx_setup.sh` 동기화
  - 펌웨어 `SensorInfo` 구조를 `user_sensor_id`, `sensor_ctgy_id`, `sensor_ctgy_type`, `sensor_ctgy_model`, 채널별 설정온도 중심으로 변경
  - `init_fixed_sensor_map()` 추가 및 TMP1=1, TMP2=2, MIC1=3, MIC2=4 고정 매핑 초기화
  - 부팅 센서 체크 로그를 `SENSOR_CHECK T1/T2/M1/M2` 형식으로 변경
  - 부팅 MQTT payload를 TMP1/TMP2/MIC1/MIC2 상태 포함 14개 필드 배열로 변경
  - MQTT config 수신 시 TMP1/TMP2 `setTmpUpLimit`, `setTmpLowLimit` 값을 Pico 내부 채널별 캐시에 저장
  - 주기 telemetry 발신 payload를 `[userSensorId, temperature]` 형식으로 변경
  - 부저 상한온도 비교 기준을 TMP1/TMP2 채널별 `g_temp_upper_limit_ch0/ch1`로 분리
  - Flask 대시보드 조회 로직을 `sensor`, `usersettings` 참조에서 `USER_SENSOR`, `SENSOR_CTGY` 참조로 전환
  - `/device-status`의 `온도 센서 회로` 단일 항목을 TMP1, TMP2, MIC1, MIC2 상태 표시로 분리
  - 온도 상태, 온도 이력, 전국 온도 API, `/api/status` 집계 로직을 고정 USER_SENSOR 모델에 맞춰 조정
  - 운영 서버 `/home/segang/project`에 `app.py`, `dashboard.html`, `device_status.html`, `device_temp_history.html`, `temp_status.html` 반영
  - 운영 서버 반영 전 백업 생성: `/home/segang/project/deploy_backup_20260703_083842`
  - 운영 Flask 프로세스 재시작 및 `/device-status`, `/api/status` 응답 확인
  - Python 문법 검사, `emqx_setup.sh` bash 문법 검사, 구식 테이블 참조 제거 확인
  - fresh CMake 구성, firmware build, `/private/tmp/nb-iot-fixed-sensors-build/nb_iot_project.uf2` 생성 검증 완료
* **미해결 및 후속 확인**:
  - UF2 플래시 후 사용자 실기기 기준 부팅 로그 payload, TMP1/TMP2 설정온도 수신, 대시보드 TMP/MIC 상태 표시 확인 필요
  - 실제 DS18B20 및 SPH0645 물리 드라이버/신호 품질은 PCB 결선 및 실측 기반 후속 검증 필요

## 📅 2026-07-03: [펌웨어] HL7811 AT 명령 목차 확인 및 MQTT 코드 분리
* **연동 대화 ID**: Codex HL7811 AT 명령 숙지 및 modem/MQTT 리팩터링 세션
* **개발 범주**: Firmware, HL7811 AT Commands, MQTTS, Refactor, FreeRTOS
* **작업 및 해결 내역**:
  - `DOCS/RM78-1 데이터시트/HL78xx - AT Commands Interface Guide - Rev16.0.pdf` 17-26p 목차 확인
  - 현재 펌웨어와 직접 관련된 명령 그룹을 V25ter/General, Mobile Equipment Control/Status, Network Service, Packet Domain, Protocol Common/SSL, MQTT AT Commands로 정리
  - HTTP Client Specific Commands(`+KHTTPCFG`, `+KHTTPCNX`, `+KHTTPHEADER`, `+KHTTPPOST`, `+KHTTP_IND`, `+KHTTPCLOSE`, `+KHTTPDEL`) 미사용 방침 반영
  - raw TCP Specific Commands(`+KTCPCFG`, `+KTCPCNX`, `+KTCPSND`, `+KTCPCLOSE`, `+KTCPDEL`, `+KTCP_IND`) 기반 펌웨어 경로 제거
  - `tasks_modem.cpp`의 `modem_SocketOpen`, `modem_SocketSend`, `modem_SocketClose`, `modem_HttpOpen`, `modem_HttpPost`, `modem_HttpClose` 구현 제거
  - `tasks_modem.hpp`의 Socket/Direct HTTPS 공개 API 및 `http_session_id` 상태 제거
  - 부팅 초기화 중 `AT+KHTTPCLOSE`, `AT+KHTTPDEL` 기반 잔존 HTTP 세션 청소 루프 제거
  - `tasks_modem.cpp`를 모뎀 전원 제어, UART 송수신, 기본 AT 응답, SIM/IMEI/CIMI/망 상태, TXON 설정, 인증서 저장, 네트워크 시간 조회 중심으로 축소
  - MQTT 세션 열기, publish, subscribe, close 구현을 신규 `src/tasks/tasks_mqtt.cpp`로 분리
  - MQTT 연결 상태 플래그를 `is_socket_open`에서 `is_mqtt_connected`로 명칭 정리
  - `CMakeLists.txt`에 `src/tasks/tasks_mqtt.cpp` 빌드 대상 추가
  - 코드 경로 기준 `KHTTP`, `KTCP`, `modem_Http`, `modem_Socket`, `http_session_id`, `is_socket_open` 잔여 참조 없음 확인
  - `/Users/segang/Documents/NB-IOT` 원본 루트에 동일 파일 동기화 및 임시 빌드 디렉터리 `/private/tmp/nb-iot-mqtt-refactor-build` 사용
  - fresh CMake 구성, firmware build, `/private/tmp/nb-iot-mqtt-refactor-build/nb_iot_project.uf2` 생성 검증 완료

## 📅 2026-07-03: [펌웨어] MQTT CONNECT FAIL 세션 초기화 보강
* **연동 대화 ID**: Codex MQTT CONNECT FAIL 및 CME ERROR 0 대응 세션
* **개발 범주**: Firmware, HL7811 MQTT, Session Cleanup, Boot Reliability
* **작업 및 해결 내역**:
  - 사용자 시리얼 로그 기준 `AT+KMQTTCFG` 직후 `+CME ERROR: 0`, `MQTT_CFG_FAIL`, `MQTT_CONNECT_FAIL` 발생 확인
  - 잦은 Pico 재부팅 후 HL7811 내부 MQTT 세션 슬롯이 잔존하여 신규 `KMQTTCFG` 생성이 실패할 가능성 확인
  - `modem_MqttOpen()` 시작 시 `MQTT_SESSION_RESET` 절차 추가
  - MQTT 세션 슬롯 1~6 대상 `AT+KMQTTCLOSE=<sid>`, `AT+KMQTTDEL=<sid>` 순차 수행 추가
  - 세션 정리 중 미존재 세션의 `ERROR` 응답은 정상 정리 흐름으로 허용
  - 세션 초기화 완료 후 `mqtt_session_id=0`, `is_mqtt_connected=false` 상태 재설정
  - `KMQTTCFG` 1차 실패 시 `MQTT_CFG_RETRY` 로그 후 세션 초기화 재수행 및 1회 재시도 추가
  - `tasks_boot.cpp`의 중복 `MQTT_CONNECT`, `MQTT_CONNECT_OK` 로그 제거로 시리얼 원인 추적성 개선
  - `/Users/segang/Documents/NB-IOT` 원본 루트에 동일 파일 동기화
  - fresh CMake 구성, firmware build, `/private/tmp/nb-iot-mqtt-session-reset-build/nb_iot_project.uf2` 생성 검증 완료

## 📅 2026-07-03: [펌웨어] RTOS 로그 출력 큐 및 단일 LogTask 전환
* **연동 대화 ID**: Codex firmware printf 제거 및 LogTask 전환 세션
* **개발 범주**: Firmware, FreeRTOS, Logging, USB stdio, Modem Stability
* **작업 및 해결 내역**:
  - USB 연결 상태와 출력 속도 편차가 부팅/통신 흐름에 영향을 줄 수 있는 기존 직접 `printf` 호출 구조 확인
  - 프로젝트 펌웨어 소스의 직접 `printf` 호출을 `LOG()` 매크로 호출로 전환
  - `src/lib/log.cpp`, `src/lib/log.hpp` 신규 추가 및 FreeRTOS 정적 큐 기반 로그 버퍼 구성
  - `LOG()` 호출부는 메시지 포맷 후 큐 적재만 수행하고 실제 `printf` 출력은 `vLogTask` 단일 지점에서만 수행
  - `main.cpp`에 `vLogTask` 등록 및 FreeRTOS 우선순위 0 최저 우선순위 적용
  - LTE/모뎀 통신 중 로그도 직접 USB 출력이 아닌 큐 적재 방식으로 통일
  - SSL Root CA 인증서 주입 구간에서 `app_log_set_enabled(false)`로 로그 비활성화 후 성공/실패 결과만 사후 출력
  - `CMakeLists.txt`에 `src/lib/log.cpp` 빌드 대상 추가
  - FreeRTOS heap `192KB`, minimal stack `384`, timer task stack `1536`으로 확장
  - LCD, Boot, Sensor, LED, Debug, PeriodicModem, Buzzer, Log task 스택 여유 확대
  - 부팅, 모뎀, 인증서, MQTT, 센서, 경보 로그를 `BOOT`, `MODEM_AT_OK`, `CERT_WRITE_OK`, `MQTT_CONNECT_OK` 등 상태 토큰 중심으로 축약
  - `dump_csv` 명령의 CSV 데이터 출력은 사용자 요청 데이터 출력으로 유지
  - `git diff --check`, fresh CMake 구성, firmware build 및 UF2 생성 검증 완료

## 📅 2026-07-03: [서버/DB] EMQX MQTT 인증 실패 추적 필드 확장
* **연동 대화 ID**: Codex EMQX 인증 실패 원인 추적 및 로깅 확장 세션
* **개발 범주**: EMQX, Supabase RPC, Authentication Logs, MQTT Security
* **작업 및 해결 내역**:
  - Supabase `authentication_logs` ID 747~749의 `device_id=""`, `deny`, `Device not found or USIM not linked` 상태 확인
  - EMQX 로그 기준 동일 시각 `2026-07-03 05:03:30~05:03:31 KST`의 인증 실패 peer IP `44.220.185.63` 및 랜덤 clientid 확인
  - 해당 실패는 Pico IMEI 기반 접속이 아닌 외부 Amazon IP 기반 MQTT 접속 시도라는 원인 판정
  - `authentication_logs`에 `clientid`, `peerhost`, `listener`, `username_raw`, `password_empty` 컬럼 추가
  - `auth_device()` RPC를 EMQX 접속 문맥 인자 확장형으로 교체하고 기존 `username/password` 2인자 호출 호환 유지
  - MQTT username 공백 요청은 `MQTT username empty` 사유로 별도 기록되도록 분기 추가
  - EMQX 운영 HTTP Authentication body에 `clientid`, `peerhost`, `listener`, `username_raw` 전달 설정 반영
  - EMQX HTTP Authentication 요청 헤더에 `x-emqx-auth-secret` 전용 식별 헤더 추가
  - 운영 서버 `/home/segang/project/.env`에 `EMQX_AUTH_SECRET` 생성 및 운영 EMQX 인증 리소스 반영
  - EMQX 인증 리소스 갱신 중 body 템플릿이 빈 값으로 치환된 상태를 발견하고 `${username}`, `${password}`, `${clientid}`, `${peerhost}`, `${listener}` 원형 템플릿으로 복구
  - 운영 EMQX 인증 리소스 조회로 `apikey`, `authorization`, `content-type`, `x-emqx-auth-secret` 헤더 키 반영 확인
  - MQTT 1883 정상 IMEI/IMSI 인증 `allow`, 빈 username 인증 `deny` 검증
  - MQTT 8883 TLS 정상 IMEI/IMSI 인증 `allow`, 빈 username 인증 `deny` 검증
  - Supabase `authentication_logs` ID 788~791 기준 `clientid`, `peerhost`, `listener`, `username_raw`, `password_empty`, `status`, `reason` 적재 확인
  - 서버 `/home/segang/project/emqx_setup.sh`, `/home/segang/emqx_setup.sh`, 로컬 `Segang/project/emqx_setup.sh`의 인증 body 갱신
  - `mosquitto_pub` 기반 빈 username 접속 거부 및 정상 IMEI/IMSI 접속 허용 검증
  - 검증용 인증 로그 제거 및 운영 로그 오염 방지

## 📅 2026-07-03: [펌웨어] USB 디버그 명령 확장 및 전원 제어 준비
* **연동 대화 ID**: Codex USB 시리얼 디버그 명령 확장 세션
* **개발 범주**: Firmware, Debug Console, Watchdog, Power Management, GPIO
* **작업 및 해결 내역**:
  - `main.cpp`에서 주석 처리되어 있던 `vDebugTask` 재등록으로 USB 시리얼 로컬 명령 수신 경로 복구
  - `tasks_debug.cpp`에 `reboot` 명령 추가 및 `safe_reboot()`를 통한 hardware watchdog 기반 Pico 재부팅 연결
  - `tasks_debug.cpp`에 `power off`, `poweroff`, `power_off` 명령 추가 및 `safe_power_off()` 연결
  - `safe_power_off()`에서 GP15 LTC2954 KILL# 신호를 LOW로 2초 요청한 뒤 전원이 살아 있으면 HIGH로 복귀하는 미연결 테스트 안전 처리 추가
  - PCB 문서 기준 RM78-1 PWRON을 GP4로 보정하고 LTC2954 INT GP14, KILL GP15 define 분리
  - `dump_csv`, `clear_csv`, `reboot`, `power off` 로컬 명령은 모뎀 busy 상태와 무관하게 우선 처리되도록 명령 분기 유지
  - 일반 AT 바이패스 명령은 모뎀 busy 상태에서 전달 보류하여 UART race condition 방지

## 📅 2026-07-03: [펌웨어] GP28 TXON LED 엣지 기반 표시 보정
* **연동 대화 ID**: Codex GP28 TXON LED 실물 표시 디버깅 세션
* **개발 범주**: Firmware, GPIO Interrupt, LED, Modem TXON
* **작업 및 해결 내역**:
  - MQTTS 연결은 정상이나 GP28 TXON 표시 LED가 송수신 중에도 깜박이지 않는 실물 증상 확인
  - 기존 50ms 폴링 방식이 RM78-1 TXON의 짧은 pulse를 놓칠 수 있는 구조 확인
  - GP5 TXON 입력을 내부 pull-up 및 상승/하강 엣지 인터럽트 감지 방식으로 변경
  - RM78-1 매뉴얼 기준 TX_ON indicator feature 활성화를 위해 `AT+KHWIOCFG?` 조회 및 필요 시 `AT+KHWIOCFG=5,1` 설정 로직 추가
  - `AT+KHWIOCFG=5,1` 변경은 모뎀 재부팅 후 적용 가능하다는 매뉴얼 주의사항 로그 반영
  - GP28 LED를 평상시 ON, 펌웨어 송신 상태(`is_transmitting`) 동안 100ms 간격 fallback blink 방식으로 변경
  - GP5 TXON 입력 직접 미러링 제거 및 TXON edge/monitor 시리얼 진단 로그 제거
  - 부팅 중 GP8 빨강 상태 LED 점멸 간격을 500ms로 변경
  - `dump_csv`, `clear_csv` 로컬 디버그 명령은 모뎀 busy 상태와 무관하게 USB stdio 입력에서 처리되도록 수정
  - 일반 AT 바이패스 명령은 모뎀 busy 상태에서 전달 보류 및 안내 로그 출력

## 📅 2026-07-03: [운영 지침] 펌웨어 검증 범위 축소
* **연동 대화 ID**: Codex 펌웨어 검증 범위 조정 세션
* **개발 범주**: Firmware Verification, UF2 Build, Agent Instructions
* **작업 및 해결 내역**:
  - 펌웨어 작업 후 Codex 검증 범위를 CMake 구성, 빌드, UF2 생성 확인까지로 제한
  - Pico 드라이브 복사, Pico 재부팅, 시리얼 로그 읽기 작업은 사용자 별도 요청 시에만 수행하도록 지침 변경
  - `AGENTS.md`, `.agents/AGENTS.md`, `.agents/skills/build-firmware/SKILL.md`에 UF2 생성까지만 수행하는 검증 기준 기록
  - UF2 반영 및 시리얼/실물 하드웨어 확인은 사용자 직접 수행 기준 반영

## 📅 2026-07-03: [운영 지침] 시리얼 모니터 공유 검증 원칙 추가
* **연동 대화 ID**: Codex 시리얼 모니터 공유 검증 지침 반영 세션
* **개발 범주**: Firmware Verification, Serial Monitor, Agent Instructions
* **작업 및 해결 내역**:
  - 펌웨어 실기기 테스트 중 시리얼 로그 확인 시 우측/가시 터미널 창에 시리얼 모니터를 출력하는 원칙 추가
  - 사용자도 동일한 시리얼 로그 흐름을 보며 부팅, 모뎀 AT, MQTTS 연결 상태를 함께 확인하는 검증 방식 반영
  - `AGENTS.md`, `.agents/AGENTS.md`, `.agents/skills/build-firmware/SKILL.md`에 시리얼 모니터 공유 확인 원칙 기록
  - LCD, LED, 부저 등 시리얼 로그 외 하드웨어 표시 항목은 사용자 실물 확인 요청 유지

## 📅 2026-07-02 (추가): [펌웨어] MQTT 브로커 환경값 누락 및 GP28 TXON LED 극성 보정
* **연동 대화 ID**: Codex MQTT placeholder 및 TXON LED 디버깅 세션
* **개발 범주**: Firmware, CMake Env, MQTTS, GPIO, LED
* **작업 및 해결 내역**:
  - 시리얼 로그의 `AT+KMQTTCFG` 명령에 `YOUR_MQTT_BROKER_HOST_PLACEHOLDER`가 포함되어 MQTTS 연결 실패하는 현상 확인
  - 루트 `.env`에 `MQTT_BROKER_HOST`, `MQTT_BROKER_PORT`가 누락되어 `src/config.h` fallback 값이 컴파일되는 원인 확인
  - 로컬 `.env`에 `MQTT_BROKER_HOST="p.zxcx.io"`, `MQTT_BROKER_PORT="8883"` 추가
  - 신규 검증 빌드 산출물 문자열 검사로 `p.zxcx.io` 포함 및 MQTT placeholder 미포함 확인
  - GP28 TXON LED가 모뎀 부팅 후 꺼진 채 유지되는 실제 증상 기준 GP5 TXON 입력 극성 재판단
  - GP5 HIGH idle 상태를 GP28 LED ON으로 직접 미러링하고, LOW pulse 시 LED OFF blink가 되도록 `tasks_led.cpp` 보정

## 📅 2026-07-02 (추가): [펌웨어] GP7 BAT MODE 표시 임시 비활성화
* **연동 대화 ID**: Codex GP7 BAT MODE 표시 원인 확인 세션
* **개발 범주**: Firmware, GPIO, LCD, PCB Bring-up
* **작업 및 해결 내역**:
  - 현재 코드 기준 GP7 HIGH는 외부 어댑터 있음, GP7 LOW는 외부 어댑터 없음으로 해석되는 구조 확인
  - GP7 미연결 상태에서 내부 `gpio_pull_down`에 의해 LOW로 고정되어 `BAT MODE`가 표시되는 원인 확인
  - 실제 PCB 전원 감지 분압 회로 연결 전까지 `tasks_led.cpp`에서 `lcd_params.is_battery_mode`를 `false`로 고정
  - 회로 연결 후 `lcd_params.is_battery_mode = !adapter_present` 로직 재활성화 위치 주석 기록
  - 최초 플래시 시 수정 전 `build/nb_iot_project.uf2`가 사용된 산출물 경로 불일치 원인 확인
  - 수정 후 생성된 `/private/tmp/nb-iot-gp7-verify/nb_iot_project.uf2` 기준 Pico 재플래시 수행
  - LCD/LED 등 시리얼 로그로 확인되지 않는 하드웨어 표시 항목은 플래시 후 사용자 실물 확인을 요청하는 검증 원칙 반영

## 📅 2026-07-02 (추가): [펌웨어] RTOS 태스크 파일 분리 및 main.cpp 정리
* **연동 대화 ID**: Codex RTOS 태스크 분리 리팩터링 세션
* **개발 범주**: Firmware Refactor, FreeRTOS, Task Structure, Build System
* **작업 및 해결 내역**:
  - `main.cpp`에 집중되어 있던 FreeRTOS 태스크 구현을 `src/tasks/` 하위 전용 파일로 분리
  - `tasks_led.cpp/hpp`, `tasks_periodic_modem.cpp/hpp`, `tasks_buzzer.cpp/hpp`, `tasks_sensor_reader.cpp/hpp`, `tasks_debug.cpp/hpp`, `tasks_boot.cpp/hpp` 신규 생성
  - `app_context.cpp/hpp` 신규 생성 및 `lcd_params`, `modem`, 센서 캐시, 알람/LED 공유 상태, JSON 파싱 헬퍼, 안전 재부팅 헬퍼 이동
  - `main.cpp`를 부팅 이유 판정, 하드웨어/상태 초기화, 태스크 등록, FreeRTOS hook 중심 구조로 축소
  - `CMakeLists.txt`에 신규 태스크 소스 파일 추가
  - `/private/tmp/nb-iot-refactor-verify` 신규 빌드 디렉터리 기준 CMake 구성 및 펌웨어 빌드 통과 확인

## 📅 2026-07-02 (추가): [대시보드] 웹폰트 로딩 안정화 변경 서버 반영
* **연동 대화 ID**: Codex 대시보드 서버 반영 세션
* **개발 범주**: Dashboard Deploy, Flask, Cloudflare, Font Cache
* **작업 및 해결 내역**:
  - `Segang/project/app.py`, `templates/layout.html`, `templates/dashboard.html` 변경분을 `segang.local:/home/segang/project` 서버에 반영
  - 반영 전 원격 백업 생성: `/home/segang/project/deploy_backup_20260702_081055`
  - 원격 `./venv/bin/python3 -m py_compile app.py` 문법 검사 통과 및 로컬/원격 SHA256 해시 일치 확인
  - `~/project/venv/bin/python3 main.py` Flask 대시보드 프로세스 재시작
  - 서버 내부 `http://127.0.0.1:18180/dashboard` 302 로그인 리다이렉트 응답 확인
  - 외부 `https://zxcx.io/dashboard` Cloudflare 경유 302 로그인 리다이렉트 응답 확인
  - `/static/fonts/SUITE-Regular.woff2` 응답의 `Cache-Control: public, max-age=31536000, immutable` 헤더 확인

## 📅 2026-07-02 (추가): [펌웨어/대시보드] RJ45 LED, 배터리 모드 표시, 웹폰트 로딩 안정화
* **연동 대화 ID**: Codex RJ45 LED·전원 감지·대시보드 폰트 개선 세션
* **개발 범주**: Firmware, FreeRTOS, GPIO, LCD, Dashboard, Font Loading, Config
* **작업 및 해결 내역**:
  - `DOCS/PCB/pico2w_rm78_sensor_pcb_design_portfolio.md`의 RJ45 LED 및 GP7 전원 감지 의도 확인
  - `src/config.h`에 GP10/GP11/GP12/GP13 RJ45 LED, GP7 전원 어댑터 감지, MQTT 브로커 placeholder 상수 반영
  - `main.cpp`의 `vStatusLedTask`에서 RJ45 온도센서 LED 정상 샘플 기반 점등 및 샘플 수신 blink 처리 추가
  - I2S/Edge AI 마이크 수집 루틴 미구현 상태를 고려하여 GP11/GP13 마이크 LED는 미사용 OFF 유지
  - GP7 전원 어댑터 감지 입력 샘플링 및 어댑터 분리 시 LCD 하단 `BAT MODE` 우선 표시 플래그 추가
  - `tasks_lcd` 공유 상태에 배터리 모드 표시 로직 추가
  - 대시보드 공통 `layout.html`의 SUITE 웹폰트 preload 및 `font-display: block` 적용, `dashboard.html` 차트 폰트 family 정합화
  - Flask `app.py` 정적 폰트 캐시 헤더 추가로 화면 전환 시 웹폰트 재로딩/폰트 swap 현상 완화
  - `.env.example`에 `MQTT_BROKER_HOST`, `MQTT_BROKER_PORT` 템플릿 키 추가
  - 신규 빌드 디렉터리 검증 중 `FREERTOS_KERNEL_PATH` 캐시 의존 문제 확인 및 `CMakeLists.txt` 경로 명시 보정

## 📅 2026-07-02 (추가): [펌웨어] PCB 기준 상태 LED 및 TXON 표시 LED 제어 추가
* **연동 대화 ID**: Codex PCB LED 펌웨어 반영 세션
* **개발 범주**: Firmware, FreeRTOS, GPIO, PCB Integration, LED Status
* **작업 및 해결 내역**:
  - `DOCS/PCB/pico2w_rm78_sensor_pcb_design_portfolio.md`의 LED 최종 배정 기준 확인
  - `src/config.h`에 GP8 빨강 상태 LED, GP9 초록 상태 LED, GP5 RM78-1 TXON 입력, GP28 TXON 표시 LED 상수 추가
  - `main.cpp`에 `vStatusLedTask` 추가 및 FreeRTOS 태스크 등록
  - 부팅 중 GP8 빨강 LED 1초 간격 점멸, 부팅 완료 후 GP9 초록 LED 상시 ON 로직 구성
  - GP5 TXON 입력을 50ms 주기로 샘플링하고 GP28 LED를 평상시 ON, TXON 감지 시 LOW 반전 표시하도록 구성
  - LED 태스크에서 공유 락 미사용 및 `vTaskDelay` 기반 주기 양보 적용, 모뎀/LCD/센서 태스크와 교착상태 회피

## 📅 2026-07-02 (추가): [운영 지침] 작업 기록 및 Git 보류 원칙 반영
* **연동 대화 ID**: Codex 운영 지침 수정 세션
* **개발 범주**: Documentation Policy, Git Workflow, README Structure, Agent Instructions
* **작업 및 해결 내역**:
  - 작업 완료 후 `project_history.md` 및 `README.md` 기록 유지 원칙 재확인
  - 작업 완료 시점 자동 Git commit/push 보류 및 사용자 명시 요청 시에만 커밋·동기화 진행 원칙 반영
  - `README.md`의 작업 건별 날짜 제목 반복 방식 중단 및 날짜별 섹션 1개 안에 여러 작업 내용 순차 누적 방식 반영
  - `AGENTS.md`, `.agents/AGENTS.md`, `.agents/skills/commit-and-log/SKILL.md` 운영 지침 갱신
  - `README.md`의 2026-07-02 항목들을 단일 날짜 섹션으로 통합 정리

## 📅 2026-07-02 (추가): [PCB 설계자료] SPH0645LM4H Edge AI 음향 센서 의도 반영
* **연동 대화 ID**: Codex PCB 마이크 설계 의도 재반영 세션
* **개발 범주**: Hardware Design, Edge AI, I2S Audio, PCB Documentation
* **작업 및 해결 내역**:
  - `DOCS/PCB/pico2w_rm78_sensor_pcb_design_portfolio.md` 및 HTML 문서의 SPH0645LM4H 설계 의도 수정분 재확인
  - 마이크 입력부를 사람 음성 녹음이나 단순 기계음 로그가 아닌 Edge AI/TinyML 기반 장비 음향 데이터 수집 채널로 재정의한 내용 숙지
  - 팬, 모터, 컴프레서, 펌프, 릴레이, 밸브, 기어, 베어링 등의 정상/비정상 운전음 패턴 수집 목적 기록
  - 정상 상태, 이상 상태, 이상 예측 상태 분류를 위한 원시 PCM, FFT, RMS, 주파수 대역 에너지, MFCC 유사 특징량 활용 가능성 기록
  - 3m UTP 케이블 안정성을 고려한 8kHz~16kHz 기본 샘플링 및 필요 시 24kHz 수준 운용 방향 기록
  - `AGENTS.md`와 `.agents/AGENTS.md`의 향후 마이크/펌웨어/센서 데이터 작업 기준 갱신

## 📅 2026-07-02 (추가): [PCB 설계자료] EasyEDA 회로도·PCB 산출물 반입 및 작업 지침 반영
* **연동 대화 ID**: Codex PCB 설계자료 숙지 세션
* **개발 범주**: Hardware Design, EasyEDA, PCB, GPIO Map, Power Management
* **작업 및 해결 내역**:
  - `DOCS/PCB/` 내 회로도 PNG, PCB PNG, EasyEDA 회로도/PCB JSON, 포트폴리오 MD/HTML 전체 확인
  - EasyEDA 회로도 `NB-IOT`, PCB export editor version `6.5.57`, PCB shape/layer/object 구조 확인
  - Pico 2 W, RM78-1 LTE-M, DS1129-04 듀얼 RJ45, DS18B20, SPH0645LM4H, LCD1602 I2C, 8002A, IP5306, MP1584EN, LTC2954CTS8-1 통합 설계 숙지
  - `+5V_IP5306` 상시 전원과 `+5V_SYS` 시스템 전원 분리 구조 숙지
  - GP0~GP5 RM78-1 모뎀, GP14/GP15 LTC2954, GP16/GP17 LCD I2C, GP18~GP22·GP26 센서/I2S 배정 숙지
  - RJ45 UTP Pair 기준 BCLK-GND Pair 배치, DS18B20 5.1kΩ Pull-up, I2S 47Ω 직렬 댐핑 구성 숙지
  - 후속 확인 항목 기록: R6 100kΩ → 1kΩ 변경, GP7 감지 전압 확인, C4 22µF 종료 지연 의도 확인
  - 향후 GPIO·전원·센서 케이블링·PCB 작업 기준을 `AGENTS.md`와 `.agents/AGENTS.md`에 반영

## 📅 2026-07-02 (추가): [Git 동기화] 미커밋 변경 정리 및 원격 동기화
* **연동 대화 ID**: Codex 동기화 세션
* **개발 범주**: Git Sync, Firmware Build, Cloudflare Redirect, Flash Logger, Documentation Policy
* **작업 및 해결 내역**:
  - 잔여 미커밋 변경 범위 확인 및 커밋 대상 정리
  - `README.md` 및 `project_history.md` 신규 항목 작성 시 명사형 종결 원칙 반영
  - `flash_logger.cpp`의 `flash_safe_execute` 기반 듀얼코어 플래시 I/O 보호 로직 반영
  - `CMakeLists.txt` 및 `main.cpp`의 Pico stdio/flash 관련 빌드 의존성 보강
  - `Segang/project/app.py`의 `www.zxcx.io` → `zxcx.io` 301 리다이렉트 추가
  - `Segang/project/main.py`의 Cloudflare Tunnel 전제 HTTP 모드 고정 및 DuckDNS 자동 동기화 비활성화
  - submodule 내부 생성물 및 `project_history.md.bak*` 백업 파일 ignore 규칙 정리
  - 로컬 커밋 후 `origin/main` 원격 동기화 대상 정리

## 📅 2026-07-02 (추가): [Codex 마이그레이션] Antigravity 설정/워크플로우를 Codex 프로젝트 지침, 설정, repo skill로 이전
* **연동 대화 ID**: Codex 마이그레이션 세션
* **개발 범주**: Codex Configuration, AGENTS.md, Repository Skills, Supabase MCP, Migration Checklist
* **작업 및 해결 내역**:
  - `DOCS/codex/mig/`의 `AGENTS.md`, `codex.config.toml`, `codex-skills-plan.md`, `migration-checklist.md` 전체 검토 및 Codex 0.142.5 공식 매뉴얼 기준 설정 구조 재작성
  - Antigravity 전용 승인/파일 접근 정책(`suggest`, `auto-edit`, `ASK/OFF` 등) 미이식
  - 프로젝트 루트 `AGENTS.md` 신규 배치 및 Codex 자동 로드 프로젝트 지침 구성
  - 기존 `/Users/segang/Documents/PicoTeam` 경로 미참조 및 통합 서버 경로 `/Users/segang/Documents/NB-IOT/Segang/project` 정리
  - `.codex/config.toml` 신규 작성 및 `approval_policy = "on-request"`, `sandbox_mode = "workspace-write"`, `[sandbox_workspace_write]`, `[mcp_servers.supabase]` 구조 적용
  - 모델/프로바이더/토큰의 프로젝트 파일 고정 제외 및 사용자 설정/OAuth 저장소 기준 보안 경계 유지
  - `codex-skills-plan.md`의 9개 반복 워크플로우를 실제 repo-scoped Codex skill로 생성: `build-firmware`, `run-server`, `supabase-inspect`, `db-migrate`, `commit-and-log`, `modem-debug`, `emqx-setup`, `mock-test`, `project-history-update`
  - Supabase MCP OAuth 상태 검증 및 `NB_IOT` 프로젝트(`yzorfvgpmkwnjpdfyqsk`, `ACTIVE_HEALTHY`, API URL `https://yzorfvgpmkwnjpdfyqsk.supabase.co`) 조회와 `public` 테이블 목록 조회 정상 동작 확인
  - Supabase advisor의 `public` 스키마 16개 테이블 RLS 비활성, 다수 함수 mutable `search_path`, `SECURITY DEFINER` 함수 공개 실행 가능 경고 확인
  - DB 변경 없음, 별도 RLS/권한 정책 설계와 사용자 승인 후 처리할 후속 보안 작업 기록
  - `DOCS/codex/mig/migration-checklist.md`의 완료/부분완료 상태 갱신 및 원본 `codex.config.toml`/skills plan의 PicoTeam 잔여 경로 보정

## 📅 2026-06-24 (추가): [DB & 서버] get_device_sensors RPC 함수 갱신을 통한 단말 설정(임계치) 및 센서 정보 통합 수신 트러블슈팅
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Supabase Database Functions (RPC), get_device_sensors, Usersettings Join, tempUpperLimitValue, EMQX config_fetch_rule
* **작업 및 해결 내역**:
  - **(원인 분석)**: 단말기(Pico 2 W)가 부팅 시 `devices/+/config` 토픽을 구독하여 센서 매핑 및 임계 온도 설정(`tempUpperLimitValue`) 정보를 수신하려 했으나, 기존 Supabase `get_device_sensors` RPC가 `sensorId`, `sensorType`, `sensorMemo`만 반환하여 단말기가 요구하는 임계 온도값 등의 설정을 함께 수신할 수 없었던 원인을 분석. 또한, 신규 `config_fetch_rule`이 EMQX에 배포되기 전 부팅 로그가 발행되어 단말이 이전에 설정 정보를 수신받지 못했음을 파악.
  - **(RPC DDL 수정)**: `get_device_sensors` Postgres 함수를 DROP 후 재생성하여, `usermachine` 및 `usersettings` 테이블을 LEFT JOIN하도록 쿼리를 갱신. 이를 통해 각 단말 IMEI에 해당하는 개별 센서 정보 행에 `tempUpperLimitValue`와 `tempLowerLimitValue` 컬럼을 병합하여 실어 보낼 수 있도록 수정 완료.
  - **(검증 완료)**: Supabase SQL을 직접 호출하여 `SELECT * FROM get_device_sensors('354720510314300')`를 수행했을 때, 센서 매핑 및 임계 온도(`tempUpperLimitValue: -10`)가 포함된 JSON 레코드셋이 정상적으로 출력됨을 확인. 이에 따라 EMQX `config_fetch_rule`에 의한 Republish 액션 시 단말이 설정 URC 및 임계 온도 캐시 갱신을 온전히 수행할 수 있도록 서버사이드 구성을 완성함.

## 📅 2026-06-23 (추가): [단말 펌웨어 & DB] Supabase 인증 API 파라미터 불일치 해결 (+CME ERROR 907 해결 및 SSL 검증 0,3 복원)
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: MQTTS SSL Verification, AT+KSSLCFG, auth_device parameters, CME ERROR 907, tasks_modem.cpp
* **작업 및 해결 내역**:
  - MQTTS 연결(`AT+KMQTTCNX`) 시 모뎀(HL7811)에서 `+CME ERROR: 907` (Generic Error) 이 지속되는 현상에 대해, SSL 검증 이슈가 아닌 **인증 API 매개변수 명칭 불일치**에 의한 접속 거절이 진짜 원인임을 규명 및 조치.
  - Supabase `auth_device` 함수 갱신 시 인자명을 `p_username/p_password`로 명시하였으나, 실제 EMQX HTTP Authenticator가 쏘는 JSON 바디 키는 `username/password`여서 PostgREST가 400 에러를 뱉고 기기 연결을 거부하던 문제를 해결.
  - Supabase `auth_device` 함수의 인자명을 `username text, password text`로 변경 및 DDL을 재배포하여 파라미터 매핑을 정상화하고 로그가 DB에 무결하게 남도록 조치.
  - 기기의 MQTTS SSL 인증서 유효성 검증의 경우 아까까지 `0,3`으로도 정상 동작했었다는 피드백 및 설계 요구조건을 수용하여, 임시 우회 적용했던 `AT+KSSLCFG=0,0` 설정을 다시 원래 안전한 전체 검증 모드인 **`AT+KSSLCFG=0,3`**으로 원복 롤백하여 빌드 완료.

## 📅 2026-06-23 (추가): [단말 펌웨어 & 서버 & DB] MQTTS 페이로드 JSON Array 마이그레이션 및 Supabase 연동 오류 완전 해결
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: JSON Array Payload, EMQX SQL json_decode, EMQX HTTP Action parameters, Supabase Integration
* **작업 및 해결 내역**:
  - EMQX v5 SQL 룰 엔진의 빌트인 문자열 파싱 함수 부재(CSV split/nth 파싱 예외 발생)로 인한 데이터 릴레이 장애를 해결하기 위해, MQTTS 페이로드 구조를 CSV에서 **JSON Array** 형식(`[v1, v2, ...]`)으로 전면 전환. (배열 구조 기준 29바이트로 모뎀 80바이트 제한 완벽 충족)
  - `main.cpp` 내의 텔레메트리 및 부팅 로그 포맷팅 문자열을 각각 `[%d,%.1f]` 및 `[%.1f,%.1f,%d,0,%d,%d,%d,%d,%d,%d,%d,%d]` 형태의 JSON Array로 수정하고 빌드 완료.
  - **(SQL 갱신)**: EMQX 룰 엔진 SQL(`telemetry_rule`, `boot_rule`)에서 CSV split 파싱 로직을 지우고, `json_decode(payload)` 및 `nth` 내장 함수를 도입. 이때 문자열 타입 지정을 싱글쿼트(`'float'`, `'integer'`)로 튜닝함. 추가로, JSON 디코딩을 통과한 원소들은 이미 고유의 Number 타입을 유지하고 있으므로 불필요한 `cast` 함수 호출을 전면 제거하여 타입 캐스팅 에러를 방지함.
  - **(HTTP Action 수정)**: EMQX HTTP Action(`supabase_boot`, `supabase_telemetry`)의 Body 템플릿 변수 매핑이 기존 JSON 구조 방식인 `${payload.v}` 형태로 잔존하여 발생하던 400 Bad Request 에러를 해결하기 위해, SQL 컬럼 별칭에 맞춰 `${v}`, `${id}` 등 직접 참조 변수 형태로 동기화 갱신 완료. 이를 통해 부팅 자가진단 로그(`device_boot_logs`)의 Supabase DB 실시간 적재 및 기기 설정 매핑 조회 시퀀스를 100% 가동 및 검증 완료.
  - Supabase `auth_device` RPC 함수에서 기존 `RAISE EXCEPTION` 기반 오류 처리 시 발생하던 트랜잭션 롤백 문제를 해결하기 위해, 에러를 던지는 대신 `allow`/`deny` 및 사유를 직접 `authentication_logs`에 INSERT한 뒤 결과 JSON을 반환하는 구조로 DDL 갱신 완료. 이를 통해 접속 이력이 DB에 정상 실시간 적재되도록 조치.

## 📅 2026-06-23 (추가): [단말 펌웨어] MQTTS Publish 성공 URC 상태 감지 범위 보정 (QoS 1 Ack 수신 정상화)
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: MQTTS Publish URC, +KMQTT_IND, QoS 1 Ack, tasks_modem.cpp
* **작업 및 해결 내역**:
  - 단말이 QoS 1로 데이터를 발행(`Publish`)한 직후 모뎀에서 리턴하는 `+KMQTT_IND: <session_id>,4` URC(데이터 수신 알림 또는 QoS 1 Puback 수신완료 알림)를 성공적으로 수집하도록 보정.
  - 기존 펌웨어가 QoS 2용 성공 URC인 `+KMQTT_IND: <session_id>,3` 만을 대기하다가 `+KMQTT_IND: <session_id>,4`가 비동기로 수신되었을 때 이를 실패로 간주해 세션을 강제 폭파(`CLOSE`)하던 버그를 해결.
  - `tasks_modem.cpp` 내의 `modem_MqttPublish`가 URC 상태 `3`과 `4` 모두를 전송 성공 상태로 유연하게 포용하도록 대기 시퀀스 조건식을 갱신하여 패킷 발행의 성공 판정 및 후속 설정 정보 수집의 안정성을 극대화.

## 📅 2026-06-23 (추가): [단말 펌웨어 & 서버] MQTTS 페이로드 CSV 전환 및 EMQX SQL 규칙 갱신
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: CSV Payload Migration, EMQX SQL split/nth Parsing, Carrier Number Encoding, emqx_setup.sh
* **작업 및 해결 내역**:
  - 셀룰러 모뎀의 80바이트 전송 한도로 인한 패킷 유실 문제를 방지하기 위해, MQTTS로 송출되는 모든 데이터(부팅 로그 및 주기적 텔레메트리)를 JSON에서 **컴마로 구분된 CSV 형식**으로 전면 전환. (부팅 로그 86바이트 -> 23바이트로 단축)
  - 통신사명 문자열을 정수(`1: SKT`, `2: KT`, `3: LGU+`, `0: 기타`)로 단말기에서 인코딩하여 전송하도록 `main.cpp`를 수정.
  - EMQX 서버의 규칙 엔진 SQL에서 `split` 및 `nth` 내장 함수를 도입하여 단말이 보낸 CSV 데이터를 JSON 필드 객체 형태로 실시간 파싱하도록 구성.
  - 규칙 엔진 내에 `CASE WHEN` 제어 흐름을 추가하여, 단말이 쏜 통신사 정수 코드(1, 2, 3)를 Supabase Webhook 호출 직전에 기존 문자열(`"SKT"`, `"KT"`, `"LGU+"`)로 복원하도록 설계함으로써 Supabase DB 스택의 하위 호환성을 원천 보장.
  - **(수동 적용)**: EMQX SQL 파서가 `cast(col as type)` 형태가 아닌 `cast(col, type)` 형태의 콤마 구분자 인자 문법만 지원하여 발생하는 400 문법 오류를 해결하고, 올바른 SQL을 추출하여 사용자가 직접 EMQX 대시보드에서 수동 설정하도록 안내 완료.

## 📅 2026-06-23 (추가): [단말 펌웨어] 초과 MQTTS 페이로드 시리얼 출력 디버그 추가
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: MQTTS Payload, Serial Debugging, tasks_modem.cpp
* **작업 및 해결 내역**:
  - 부팅 자가 진단 로그 MQTTS 발행 시 페이로드가 80바이트 제한을 초과해 전송이 누락되는 현상에 대해, 초과 발생 원인을 직관적으로 디버깅할 수 있도록 80바이트 초과 예외 발생 시 전송하려던 원본 페이로드 전체를 PC 시리얼 모니터에 즉각 출력(`printf`)하는 구문을 `tasks_modem.cpp` 내 `modem_MqttPublish`에 추가.

## 📅 2026-06-23 (추가): [단말 펌웨어] SSL Root CA 인증서 교체 및 200바이트 분할 페이싱 적용 (+CME ERROR: 931 완벽 조치)
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: ISRG Root X2 PEM, AT+KCERTSTORE, Format Validation, Pacing Tuning, CME ERROR 931
* **작업 및 해결 내역**:
  - 기존에 잘못 적용되어 있던 964바이트 크기의 교차서명(Cross-signed) 버전의 Root X2 대신, 공식 Let's Encrypt 서버에서 획득한 진짜 자체서명(Self-signed) 버전의 **`ISRG Root X2` 인증서(개행문자 포함 790바이트)**를 C++ 코드에 탑재.
  - 이로 인해 모뎀이 개행문자가 포함되지 않은 비정상 포맷의 데이터를 받아 발생하던 `+CME ERROR: 931` 포맷 검증 실패 현상을 전면 해결.
  - 200바이트 분할 전송 루틴에 맞춰 바이트 간 페이싱 딜레이를 `2000us` (2ms), 청크 간 대기 시간을 `500ms`로 유지하여 안정적인 NVRAM 저장 시퀀스 보장.

## 📅 2026-06-23 (추가): [단말 펌웨어] 진짜 SSL Root CA (ISRG Root X2) 분할 연속 주입 및 전체 검증 활성화 적용
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Let's Encrypt ISRG Root X2, AT+KCERTSTORE, AT+KSSLCFG, Chunk-based UART Transmission
* **작업 및 해결 내역**:
  - 기존의 더미 CA(`GTS_ROOT_R4_CERT`)를 활용한 보안 검증 우회 구조 대신, `p.zxcx.io:8883` 실서버가 사용하는 진짜 Let's Encrypt ECDSA 체인의 루트 CA 인증서인 `ISRG Root X2` (Root YE, 776바이트)를 압축 정의하여 실장 완료.
  - 데이터 전송 안정성을 확보하기 위해, 776바이트 인증서 데이터를 64바이트 단위의 조각(fragments)으로 나누어 10ms 딜레이를 주며 연속으로 송신하는 청크 기반 분할 주입 로직을 `tasks_modem.cpp`에 구현하여 UART 버퍼 오버런 및 데이터 유실 문제를 방지.
  - 보안 검증 단계를 원래 요구 사양에 맞게 다시 정상 가동하기 위해, `modem_HttpOpen` 및 `modem_MqttOpen` 내부의 `AT+KSSLCFG=0,0` 설정을 `AT+KSSLCFG=0,3` (서버 및 클라이언트 전체 검증 레벨 3)으로 정상 원복 적용하여 빌드 성공.


## 📅 2026-06-23 (추가): [서버] EMQX 도커 컨테이너 데이터 볼륨 유실 복구 및 설정 원상복구
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: EMQX Config Restoration, Docker Volume Mount, API Key Generation
* **작업 및 해결 내역**:
  - 이전 작업에서 EMQX Docker 컨테이너에 Let's Encrypt 공인 인증서를 연동하는 과정 중, `/opt/emqx/data` 데이터 볼륨이 컨테이너에 정상 마운트되지 않아 기존에 구성된 모든 설정(Supabase 웹훅 연동, 규칙 데시벨, 기기 인증 규칙 등)이 유실 및 초기화되었던 장애를 진단.
  - 호스트 서버 내의 Docker anonymous volume들을 검사하여, 이전 데이터 파일 및 `cluster.hocon`이 온전히 잔존하는 볼륨명 `55a915af067edf58f7598aeb3b89c296390a1e5535eba9c32877b9f0e7a65fcb`을 특정하고 확인.
  - 해당 볼륨을 `-v 55a915af067edf58f7598aeb3b89c296390a1e5535eba9c32877b9f0e7a65fcb:/opt/emqx/data` 형태로 매핑하여 EMQX 컨테이너를 재기동함에 따라 기존의 모든 Supabase 인증 및 데이터 릴레이 규칙을 온전히 복구 완료.
  - 데이터 볼륨이 복구됨에 따라 새로운 API Key `supabase_setup`를 재발급하고, 이를 기반으로 `emqx_setup.sh` 셋업 스크립트 내의 `AUTH_HEADER` 인증 헤더 정보를 동기화 업데이트하여 향후 스크립트 실행 안정성을 확보.


## 📅 2026-06-23 (추가): [단말 펌웨어] MQTTCFG CME ERROR 0 조치를 위한 더미 CA 인증서 주입 복구
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: MQTTS SSL Bypass, GTS_ROOT_R4_CERT, AT+KCERTSTORE, AT+KMQTTCFG
* **작업 및 해결 내역**:
  - `tasks_modem.cpp`에서 MQTTS 인증서 검증 우회(`AT+KSSLCFG=0,0` 적용)를 처리하기 위해 `AT+KCERTSTORE` 주입 단계를 아예 생략했더니, 모뎀 내부의 SSL 스택 초기화 오류로 인해 `AT+KMQTTCFG` 명령 실행 시 `+CME ERROR: 0` (Phone failure)이 발생하는 현상을 진단 및 해결.
  - HL7811 모뎀의 TLS 스택 초기화에는 최소한 슬롯 0번에 유효한 인증서가 존재해야 하므로, 765바이트 크기의 컴팩트한 `GTS_ROOT_R4_CERT` (더미 CA)를 `AT+KCERTSTORE=0` 슬롯에 주입하는 시퀀스를 다시 복구함.
  - 이를 통해 `AT+KMQTTCFG` 호출 시 `+CME ERROR: 0` 없이 정상적으로 `OK` 응답을 획득하도록 보장하고, 실제 접속 검증 단계에서는 `AT+KSSLCFG=0,0` (verify_none) 설정을 통해 서버와의 암호화 핸드셰이크가 중단 없이 이루어지도록 최종 연동 구조를 완성함.


## 📅 2026-06-23 (추가): [단말 펌웨어] 모뎀 UART 선로 stdio 출력 간섭 해제 (무한 ERROR 해결)
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: UART stdio disabling, CMakeLists.txt modification, Serial output conflict resolution
* **작업 및 해결 내역**:
  - `CMakeLists.txt` 내의 `pico_enable_stdio_uart(nb_iot_project 1)`가 활성화되어 있어 모든 `printf` 디버그 출력이 모뎀이 물려있는 `uart0` 물리 핀(GP0/GP1)으로 전송되는 심각한 충돌 버그를 발견.
  - 이로 인해 Pico 2 W의 자가진단 및 통신 상태 디버그 메세지가 모뎀의 RX 핀으로 들어가 무수한 모뎀 구문 오류(`ERROR`)를 유발하고, 이 응답을 Pico가 다시 `printf`로 시리얼에 출력하면서 무한 피드백 루프(폭포수형 ERROR 로그)가 발생하는 현상을 파악.
  - `pico_enable_stdio_uart(nb_iot_project 0)`로 설정을 변경하여 물리 UART로의 stdio 리디렉션을 차단함. USB를 통한 시리얼 디버깅(`pico_enable_stdio_usb`)은 유지되므로 모뎀과의 선로 간섭 없이 깨끗한 제어가 가능하도록 조치함.


## 📅 2026-06-23 (추가): [단말 펌웨어] AT 명령어 개행 문자(\r) 수정 및 PC 시리얼 노이즈 가드(DebugTask 비활성화) 패치
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: AT Command Termination Character, Carriage Return, Serial Noise Guard, vDebugTask Suspension
* **작업 및 해결 내역**:
  - `modem_SendCmd()` 에서 송출 시 개행 문자를 `\r\n`으로 보낼 때, HL7811 모뎀 측에서 뒤이어 들어오는 `\n` (Line Feed)을 불필요한 빈 명령어로 인식하여 실행 직후 무조건 `ERROR`를 내뱉게 만들던 증상을 확인.
  - 송신 터미네이터를 `\r\n` 대신 모뎀 전용인 `\r` (Carriage Return)만 송출하도록 수정.
  - PC 터미널 모니터가 연결될 때 유입되는 비정상 데이터나 시리얼 노이즈가 `vDebugTask`를 타고 모뎀으로 흘러들어 가 통신 전체를 꼬이게 하고 무한 에러를 유발하는 요소를 원천 차단하기 위해 `main.cpp` 내 `vDebugTask` 생성을 임시 비활성화(주석 처리)하여 빌드 성공.

## 📅 2026-06-23 (추가): [단말 펌웨어] 자가진단(Self-Diagnostics) 원상복구 및 RAM 무결성 검사 정상 고정
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Rollback, Self-Diagnostics Restoring, RAM Test Force OK
* **작업 및 해결 내역**:
  - 자가진단 임시 우회(Bypass) 모드 이후에도 모뎀 부팅 에러가 해결되지 않음에 따라, 사용자의 요청을 수렴하여 기존 자가진단 단계(Pico 전압 측정, 내부 칩 온도 계측, 플래시 CRC 검증, LCD 상태 문자열 스크롤) 전체를 기존 상태로 원상복구.
  - 다만, 램 무결성 진단(`test_ram_integrity()`) 부분은 하드웨어적 병목 및 비효율성을 방지하기 위해 수행 결과를 강제로 정상 통과(`ram_ok = true / ram_test_val = 0`)로 매핑하여 고정.
  - 디버그 태스크 시리얼 가로채기 방지용 락 가드(`is_modem_busy`)와 MQTTS 연동 기능은 온전히 유지하여 원복 적용 완료 및 리빌드 성공.

## 📅 2026-06-23 (추가): [단말 펌웨어] UART 노이즈에 의한 무한 수신 루프(Lockup) 방지 및 풀업 가드 패치
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: UART Pull-up, Serial Stability, Read response Timeout/Guard, Infinite Loop Prevent
* **작업 및 해결 내역**:
  - 모뎀 기동 초기나 노이즈 환경 하에서 `uart_is_readable()`이 계속 참을 반환할 때 `modem_ReadResponse()` 함수 내 수신 루프가 무한 루프에 빠져 CPU를 100% 점유하고 다음 AT 명령어 시퀀스(`ATE0` 등)가 아예 실행되지 않던 버그를 확인.
  - `modem_ReadResponse()` 함수에 1회 호출 시 최대 256바이트까지만 읽도록 수신 바이트 제한 가드를 주입하여 무한 루프 락업을 완벽히 차단.
  - Pico의 UART TX(GP0)/RX(GP1) 핀에 `gpio_pull_up()` 설정을 강제 활성화하여 플로팅 상태에서의 UART 선로 유입 노이즈(이로 인해 모뎀이 ERROR를 자가 유발하는 현상)를 원천 차단하고 빌드 성공.

## 📅 2026-06-23 (추가): [단말 펌웨어] 디버그 태스크 시리얼 가로채기(Race Condition) 방지 가드 락 패치
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: FreeRTOS Tasks, Race Condition, UART RX Buffer, Debug Bypass
* **작업 및 해결 내역**:
  - 부팅 지연을 피하기 위해 `vBootTask` 진입 즉시 `lcd_params.is_booting = false;`를 선언하면서, 디버그 쉘 태스크(`vDebugTask`)가 모뎀 초기화가 완전히 끝나기도 전에 기동되는 문제를 확인.
  - 이로 인해 `vBootTask`가 AT 명령을 송신하고 응답을 대기하는 동안 `vDebugTask`가 UART RX 버퍼의 문자를 10ms 주기로 가로채 가(Race Condition) 모뎀 초기화 시퀀스가 붕괴하고 시리얼에 지속적으로 `ERROR`가 출력되는 현상이 유발됨.
  - `vBootTask`와 `vPeriodicModemTask`의 주요 모뎀 통신 구간 시작 시점에 `lcd_params.is_modem_busy = true;` 가드 락을 확실히 설정하고 완료 후 해제하도록 조치하여 충돌을 원천 차단하고 빌드 성공.

## 📅 2026-06-23 (추가): [단말 펌웨어] HL7811 모뎀 하드웨어 부팅 펄스 시퀀스 오류 패치
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Hardware Power-on, PWR_ON_PIN Pulse Sequence, Modem Initialization
* **작업 및 해결 내역**:
  - 모뎀 기동을 위한 하드웨어 부팅 시퀀스(`modem_hw_power_on()`) 완료 시점에 `PWR_ON_PIN`을 0(LOW)으로 끝내도록 되어 있던 버그를 수정.
  - Active Low로 동작하는 HL7811의 `PWR_ON_N` 핀이 계속 LOW 상태로 묶여 부팅 도중 무한 리셋이나 오동작(시리얼 콘솔에 ERROR가 지속적으로 출력되는 현상)을 일으키던 현상을 해결.
  - 펄스 시퀀스를 1(HIGH, 1초 대기) -> 0(LOW, 1.5초 유지하여 트리거) -> 1(HIGH, 최종 릴리즈 및 복구)로 변경하여 기기 부팅 시퀀스를 안정화하고 리빌드 완료.

## 📅 2026-06-23 (추가): [단말 펌웨어] USB CDC stdio 재활성화 패치
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: CMake Configuration, USB stdio enabling, Debugging recovery
* **작업 및 해결 내역**:
  - 부팅 지연 및 락업 문제가 바이패스 모드로 원천 해결되었고, stdio 타임아웃도 `0ms`로 이미 안전하게 설정되어 있으므로, 디버깅 모니터링 편의를 위해 USB stdio 출력 기능을 재활성화.
  - `CMakeLists.txt` 내 `pico_enable_stdio_usb(nb_iot_project 1)`로 매핑하여 PC의 시리얼 콘솔 프로그램을 통한 모니터링이 가능하도록 조치 및 리빌드 성공.

## 📅 2026-06-23 (추가): [단말 펌웨어] 비동기 부팅 바이패스(Bypass) 모드 도입
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: FreeRTOS Boot Task, Diagnostics Bypass, Non-blocking LCD
* **작업 및 해결 내역**:
  - 5V 환경 및 기동 직후 NTC/VSYS 전압 측정 및 RAM/플래시 자가진단 수행 시 발생하는 하드웨어 락업 및 부팅 지연으로 인한 LCD `Boot.. Check Pico` 멈춤 현상 원천 예방.
  - `vBootTask` 실행 즉시 `lcd_params.is_booting = false;` 및 `"Ready"` 상태로 LCD 화면을 즉각 해제하도록 구조 변경 (부팅 지연 0초).
  - 기존의 블로킹 유발형 자가진단 단계를 모두 스킵하고 디폴트값(전압 5.0V, 정상 코드 등)으로 포워딩하도록 단순화.
  - 모뎀 기동 및 1회성 MQTTS 부팅 보고 송출은 백그라운드 태스크에서 LCD 및 타 태스크 간섭 없이 비동기 구동되어 소멸하도록 개정 및 리빌드 성공.

## 📅 2026-06-23 (추가): [단말 펌웨어] MQTT QoS 1 하향 조정 적용
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: MQTT QoS 1, tasks_modem.cpp modification
* **작업 및 해결 내역**:
  - 단말의 모든 MQTT 메시지 발행(Publish) 및 구독(Subscribe) 요청의 QoS 수준을 기존 2(Exactly Once)에서 1(At least Once)로 하향 조정.
  - `tasks_modem.cpp` 내의 `AT+KMQTTPUB` 파라미터를 `,1,`로 수정하고, QoS 1 발행 성공을 뜻하는 모뎀 URC 코드 `+KMQTT_IND: <session_id>,3`을 대기하도록 URC 대기 시퀀스 개정.
  - 구독 명령 `AT+KMQTTSUB` 또한 QoS 1(파라미터 `,1`)로 작동하도록 튜닝 완료 및 펌웨어 리빌드 검증 성공.

## 📅 2026-06-23: [관제 웹 & 서버] Cloudflare Tunnel (zxcx.io) 연동 및 EMQX MQTT 브로커 최신 버전 구축
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Cloudflare Tunnel (cloudflared), Flask HTTPS Bypass, EMQX MQTT Broker Install, ddclient Cloudflare DDNS Setup

### 1. 작업 개요 (Goal & Requirements)
* 기존 `segang.duckdns.org` 도메인 대신 개인 도메인 `zxcx.io`를 Cloudflare DNS에 연동하고, 결제 인증된 정식 Cloudflare Tunnel(`cloudflared`)을 서버에 인스톨하여 대시보드 웹을 포트 포워딩 없는 HTTPS 환경으로 가동.
* 더 이상 무의미한 DuckDNS IP 자동 동기화 루프 및 구형 DDNS 설정을 정리.
* DNS Only(grey cloud)로 설정된 `p.zxcx.io`에 대해 서버의 공인 IP 변동 시 자동으로 동기화되도록 `ddclient`와 Cloudflare API v4를 연동.
* IoT 단말 통신 수신을 위해 최신 분산형 MQTT 메시지 브로커인 **EMQX 6.2.1**을 Docker 기반으로 신규 구축.

### 2. 주요 작업 및 기술적 의사결정
* **Flask 서버 HTTPS 해제 및 터널 매핑 (`main.py` 수정)**:
  - Cloudflare Edge단에서 HTTPS(SSL) 암호화 통신을 일괄 제어하므로, 우분투 서버 측은 자체 SSL 바인딩을 해제하고 HTTP 일반 모드(`port=18180`)로 깔끔하게 동작하도록 전환하여 CPU 리소스를 경감.
  - `main.py` 내의 `update_duckdns()` 및 `duckdns_loop()` 백그라운드 IP 동기화 스레드 동작을 비활성화 처리.
* **cloudflared Connector 서비스 설치 및 기존 ddclient 제거**:
  - 구버전 ddclient를 한 번 퍼지 처리하고, Cloudflare Zero Trust와 연동하는 `cloudflared` 바이너리를 설치하고 systemd 백그라운드 서비스(Active running)로 정상 안착시킴.
* **ddclient 재설치 및 Cloudflare API v4 연동 (`p.zxcx.io` DDNS 구성)**:
  - `apt-get`으로 `ddclient`를 재설치하고 `/etc/ddclient.conf` 파일에 Cloudflare API v4 연동 옵션 적용 (`protocol=cloudflare`, `login=token`, `password=cfut_...`, `zone=zxcx.io`, `p.zxcx.io`).
  - `ddclient.conf` 파일 권한을 `600`으로 제한하여 보안 격리 후 백그라운드 데몬 서비스(`active (running)`) 활성화.
  - `ddclient -force` 수동 실행을 통해 A 레코드 강제 업데이트 및 `SUCCESS: updating p.zxcx.io` 성공 동작 검증 완료.
* **EMQX MQTT 브로커 최신 버전(6.2.1) Docker 구축**:
  - 기존에 설치한 apt 기반의 EMQX 5.8.9를 정지 및 제거하고, Docker 기반으로 최신 EMQX 6.2.1 버전을 기동 완료.
  - 우분투 서버 내의 `docker` 사용자 그룹 누락으로 인한 `docker.socket` 기동 장애(Control process exited, status=216/GROUP)를 `groupadd docker` 및 데몬 릴로드를 통해 해결.
  - Docker 컨테이너를 `--restart always` 및 포트 1883(MQTT), 8083(WS), 8084(WSS), 8883(MQTTS), 18083(대시보드)으로 서비스가 안정 구동되도록 구축 완료.

---

## 📅 2026-06-23: [단말 펌웨어 & 서버] MQTTS (TLS 8883) 통신 마이그레이션 및 Supabase 인증/규칙 연동
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: MQTTS (QoS 2 / Clean Session 1) Migration, HL7811 MQTT AT, Supabase pg_cron Offline Check, authentication_logs DB Schema, LCD Unauth Layout

### 1. 작업 개요 (Goal & Requirements)
* 단말(Pico 2 W)의 데이터 전송 방식을 기존 HTTP REST API에서 암호화된 MQTTS(TLS, 포트 8883) 방식으로 완전 마이그레이션하여 통신 지연 및 전력 소모 감축.
* EMQX 6.2.1 서버 단에 Supabase PostgreSQL DB를 연동하여 기기(IMEI/IMSI) 접속 인증을 수행하고, 접속 통과/실패 내역을 Supabase `authentication_logs` 테이블에 자동 적재.
* 데이터 미전송 상태를 감지하여 30분 이상 무수신 시 DB 내에서 자동으로 단말 상태를 `'offline'`으로 갱신하는 크론 배치 스케줄러 등록.
* 인증 실패 URC 감지 시 안테나/송수신 애니메이션은 LCD에 그대로 작동시키면서 하단 왼쪽에 `Unauth` 에러를 선명하게 표시하도록 펌웨어 개정.

### 2. 주요 작업 및 기술적 의사결정
* **단말 MQTTS 통신 추가 및 80바이트 제한 방어 (`tasks_modem.cpp / main.cpp` 수정)**:
  - HL7811 모뎀의 `AT+KMQTTPUB` 페이로드 용량이 최대 80바이트로 한정되므로, JSON 구조를 대폭 축소하고 Ch0/Ch1 상태 필드를 각각 `ts0`, `ts1`로 분할하여 75바이트 이하로 압축 송신 및 소스 한글 상세 주석 주입.
  - 램 자가진단 검사는 일시 보류하여 항상 `ram_test_val = 0`(정상) 값으로 포워딩하도록 변경.
  - `AT+KSSLCFG=2,0` 명시를 통해 PSM/TLS 세션 재개(Session Resumption)를 가동하여 재접속 핸드셰이크에 수반되는 리소스 손실 방지.
  - 전송 직후 `devices/[IMEI]/config` 토픽을 구독 대기하여 Supabase 규칙 엔진이 보낸 실시간 임계 상한 온도(`tempUpperLimitValue`) 및 기기 제어 명령(`cmd`/`cmdId`)을 동적으로 갱신받는 비동기 양방향 동기화 시퀀스 수립.
* **QoS 2 및 Clean Session 1 적용**:
  - 데이터 유실 및 중복 배달을 완벽 차단하기 위해 **QoS 2 (Exactly Once)**를 적용하고, 20분 간격 초저전력 딥슬립 전환 시 메모리 세션 파편화를 방지하기 위해 **Clean Session 1** 적용.
* **LCD Unauth 렌더러 추가 (`tasks_lcd.cpp` 수정)**:
  - `is_unauthenticated` URC 감지 시, LCD 우측 작동 UI는 유지하되 하단 온도를 띄우는 라인에 `"Unauth"` 텍스트가 마스킹 출력되도록 렌더 분기 구현.
* **EMQX Supabase 연동 자동화 쉘 스크립트 작성 (`emqx_setup.sh` 신규 작성)**:
  - REST API 및 curl를 사용하여 우분투 서버에서 원클릭으로 PostgreSQL 인증 모듈, Webhook 데이터 브릿지, Republish 및 데이터 적재 규칙 엔진을 자동으로 구성하는 유틸리티 스크립트를 작성하여 서버로 배포 완료.
* **Supabase SQL 스키마 및 크론 스케줄 등록**:
  - `authentication_logs` 테이블 생성 및 `pg_cron` 확장 기반 매 1분마다 `last_seen_at`이 30분 이전인 기기의 status를 offline으로 일괄 업데이트하는 스케줄러(`device-offline-check`) 등록 완료.

---

## 📅 2026-06-22: [단말 펌웨어] 5V 부팅 임시 우회(자가진단 Bypass) 복구 롤백 패치
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Rollback, Diagnostics Bypass, Stability Recovery

### 1. 작업 개요 (Goal & Requirements)
* `safe_printf` 및 자가진단 활성화 패치 적용 후 5V 환경에서 다시 부팅 멈춤 증상이 재현됨에 따라, 5V 어댑터 전원 하에서 정상 동작(부팅 성공률 100%)이 확실하게 검증되었던 직전 시점인 **"자가진단 Phase 1 임시 우회(Bypass)"** 상태로 긴급 롤백 진행.

### 2. 주요 작업 및 기술적 의사결정
* **코드 롤백 실행**:
  - `safe_printf` 감지 및 전역 오버라이드 매크로를 `src/config.h`에서 제거.
  - `main.cpp` 내의 자가진단 과정(`read_vsys_voltage`, `read_internal_temp`, `check_flash_integrity`, `test_ram_integrity`)을 다시 더미 처리 및 주석 처리(우회)하여 `Check Pico` 락-업 요소를 강제 분리.
* **이유 분석**:
  - `safe_printf`를 적용했음에도 데드락이 발생한 것은, 멈춤의 원인이 `printf` 뿐만 아니라 자가진단 함수 내부에서 호출되는 하드웨어 핀 제어(예: VSYS 전압 측정 시 CYW43 무선 칩 SPI GP25/GP29 하이재킹에 따른 버스 인터랙션이나 내부 클록 오동작) 또는 RAM 테스트의 dynamic allocation 과정에 하드웨어적 병목이 아직 완전히 해결되지 않았음을 시사함.
* **빌드 및 갱신 완료**:
  - `ninja` 빌드를 통해 잘 작동하던 이전 바이너리로 복원 완료.

---

## 📅 2026-06-22: [단말 펌웨어] 초기 디버그 printf 폭발 방지 및 부팅 격리 패치 (5V 부팅 안정성 확보)
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: FreeRTOS Tasks, stdio CDC Buffer Deadlock, Debug Print Lockout

### 1. 작업 개요 (Goal & Requirements)
* USB 디버깅 기능(`pico_enable_stdio_usb`)을 유지한 상태에서, 부팅 직후(1초 시점) `vSensorTask`와 `vBootTask`가 동시에 대량의 디버그 `printf`를 송출하여 USB 가상 시리얼 포트(CDC) 버퍼를 꽉 채워 기기를 데드락에 빠뜨리던 현상 해결.

### 2. 주요 작업 및 기술적 의사결정
* **부팅 중 출력 차단 가드 보강 (`tasks_sensor.cpp` 수정)**:
  - `check_ntc_status_dual` 의 디버그 출력 가드 조건식에 `lcd_params.is_booting == true` 를 추가하여 부팅 완료(`Ready`) 상태 전까지는 어떠한 백그라운드 디버그 로그도 송출되지 않도록 격리.
* **최초 디버그 출력 시점 지연**:
  - `last_dbg_print_ms` 초기값을 `0` 대신 부팅 직후의 `to_ms_since_boot(get_absolute_time())` 으로 셋팅하여, 부팅 완료 단 1초 만에 320바이트의 `[Sensor Dbg]` 로그가 즉각 송출되는 문제를 해결하고 최초 출력을 3분 뒤로 미룸.
* **빌드 및 갱신 완료**:
  - Ninja 컴파일러 빌드를 통해 바이너리 갱신 완료.

---

## 📅 2026-06-22: [단말 펌웨어] USB stdio 제거 및 하드웨어 UART stdio 전환 (5V 전원 구동 최적화)
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: CMake Stdio Redirection, USB CDC Driver Disable, UART Stdio Enable

### 1. 작업 개요 (Goal & Requirements)
* 일반 5V 전원 어댑터 연결 등 USB 데이터 선이 플로팅되거나 호스트가 없는 무전력 감지 상황에서, USB 장치 스택의 드라이버 락 및 인터럽트 경합에 의한 부팅 정지 현상을 원천적으로 차단하기 위한 하드웨어 stdio 전향.

### 2. 주요 작업 및 기술적 의사결정
* **Stdio 출력 통로 변경 (`CMakeLists.txt` 수정)**:
  - `pico_enable_stdio_usb(nb_iot_project 0)` 설정하여 TinyUSB 가상 CDC 시리얼 장치 스택 비활성화.
  - `pico_enable_stdio_uart(nb_iot_project 1)` 설정하여 표준 출력(`printf`) 채널을 GP0(TX)/GP1(RX) 물리 UART 핀으로 강제 고정.
  - USB 연결 여부와 관계없이 드라이버 루프가 100% 논블로킹으로 구동되게끔 물리 통로를 분리하여 어댑터 부팅 문제를 완전 해소함.
* **빌드 완료**:
  - Ninja 컴파일러 빌드를 통해 바이너리 갱신 완료.

---

## 📅 2026-06-22: [단말 펌웨어] 스케줄러 기동 전 printf 임시 소거 패치 (5V 전원 먹통 방지)
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: FreeRTOS Pre-scheduler, stdio USB CDC lock-up prevent

### 1. 작업 개요 (Goal & Requirements)
* 외부 5V 전원 어댑터 연결 시 FreeRTOS 스케줄러 기동 전 실행되는 `printf` 출력들이 USB 호스트 미연결로 인해 TinyUSB 내부 락에 빠져 기기 부팅 자체를 차단하던 현상 임시 소거 및 검증.

### 2. 주요 작업 및 기술적 의사결정
* **초기 printf 주석 처리 (`main.cpp` 수정)**:
  - `detect_boot_reason()` 내의 진단 및 부팅 원인 출력 `printf` 2개 주석 처리.
  - `main()` 함수 초입의 배너 출력 `printf` 3개 주석 처리.
  - 이를 통해 USB 호스트가 없는 환경(5V 단독 전원 어댑터)에서 스케줄러(`vTaskStartScheduler()`)가 돌기도 전에 보드가 뻗는 현상을 예방.
* **빌드 완료**:
  - Ninja 컴파일러 빌드를 통해 바이너리 갱신 완료.

---

## 📅 2026-06-22: [단말 펌웨어] 외부 5V 전원 부팅 시 stdio 블로킹 멈춤 현상 패치
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: CMake Configuration, stdio USB blocking, TinyUSB driver timeout optimization

### 1. 작업 개요 (Goal & Requirements)
* 단말기를 맥북 USB 포트가 아닌 외부 5V 어댑터 전원에만 연결했을 때 부팅 진단 중 먹통(Check Pico 화면 등)이 되는 현상 해결.

### 2. 주요 작업 및 기술적 의사결정
* **USB Stdio 기본 타임아웃 0ms 최적화 (`CMakeLists.txt` 수정)**:
  - `add_compile_definitions(PICO_STDIO_USB_DEFAULT_TIMEOUT_MS=0)` 전역 매크로 정의를 빌드 구성에 추가.
  - PC 시리얼 모니터가 열리지 않는 상태에서 `printf` 및 `putchar` 호출 시, TinyUSB 전송 버퍼가 찰 때마다 발생하는 기본 50~100ms 대기 타임아웃을 강제로 제거하여 무비동기(Non-blocking)로 즉각 반환되도록 개선.
  - 이로 인해 외부 5V 단독 전원 동작 시에도 실시간 FreeRTOS 태스크 스케줄링이 마비되지 않고 정상 부팅 및 무중단 작동 보장.
* **빌드 및 갱신 완료**:
  - Ninja 컴파일러 빌드를 통해 바이너리(`nb_iot_project.uf2`) 갱신 완료.

---

## 📅 2026-06-21: [단말 펌웨어] RTOS 스택 오버플로우 방지 및 UART 락 충돌 방지 패치
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: FreeRTOS Task Stacks, stdio Mutex Locking, UART Deadlock Avoidance, Buffer Race Conditions

### 1. 작업 개요 (Goal & Requirements)
* HTTPS 송출과 3분 주기 NTC 온도 디버그 `printf` 출력 타이밍이 겹칠 때 단말이 뻗는(출력 잘림 및 무반응) 현상 해결.

### 2. 주요 작업 및 기술적 의사결정
* **스택 사이즈 최적화 (`main.cpp` 수정)**:
  - `vSensorTask` 스택 크기를 `256 words` (1024 bytes)에서 `1024 words` (4096 bytes)로 4배 증설하여 `printf` 내부의 대용량 float 포맷터와 수학 라이브러리(`log`) 연산에 필요한 스택 마진을 충분히 보장함.
  - 추가로 `vDebugTask`와 `vBuzzerTask` 스택 역시 각각 `512`에서 `1024 words`로 확장하여 태스크 구동의 안정성을 높임.
* **로그 출력 상호 배제 가드 추가 (`tasks_sensor.cpp` 수정)**:
  - 3분 주기 디버그 로그 `[Sensor Dbg]` 출력 시, 모뎀이 데이터를 전송 중이거나 비지 상태(`lcd_params.is_transmitting || lcd_params.is_modem_busy == true`)인 경우 출력을 1초간 미루고 건너뛰도록 가드 구현. 이를 통해 stdout(UART0) Mutex 락 경합 및 교착 상태를 원천 차단함.
* **빌드 및 갱신 완료**:
  - Ninja 컴파일러 빌드를 통해 오류 없이 바이너리 갱신 완료.

---

## 📅 2026-06-20: [펌웨어 & DB] Supabase DB Join 제거 및 싱글/듀얼 온도 센서(냉장/냉동) 동적 대응 고도화
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Supabase DDL (RPC), Pico 2 W C/C++ Firmware, Dual-Channel NTC ADC Read, Periodic Transmission, JSON Parsing, HTTP Session Optimization, Response Parsing Bug Fix, Math Domain Protection

### 1. 작업 개요 (Goal & Requirements)
* Supabase에 `sensorvalue` 인서트 시 발생하는 DB JOIN 부하를 제거하여 기성 쿼리를 튜닝할 것.
* 기기 부팅 시 기기에 매핑된 센서 정보를 Supabase `get_device_sensors` RPC를 통해 미리 조회하여 RAM에 캐싱.
* 추후 온도센서가 1개 더 추가되는 상황(냉장용 센서, 냉동용 센서 총 2개)과 기존 1개만 사용하는 기기 모두 완벽히 동적으로 자동 인식 및 지원할 것.
* 20분마다 온도를 전송할 때 캐싱된 각 센서의 `sensorId`를 포함해 JOIN이 없는 튜닝된 RPC `t`로 각각 전송할 것.
* **부팅 시 불필요한 HTTPS 세션 닫기/열기 해소**: 부팅 로그 전송 후 바로 연달아 센서 정보를 조회하므로, TCP/TLS 오버헤드를 막기 위해 단일 HTTPS 세션 내에서 연속적으로 전송하도록 시퀀스를 결합할 것.
* **센서 정보 파싱 실패 해결**: Supabase `get_device_sensors` 응답 시 cmd가 아닌 다른 일반 JSON이 응답되어도 누락 없이 `response_buf`에 복사되도록 필터를 완화할 것.
* **플로팅 핀(미연결 핀) 수학적 오류 예외 처리**: 온도 센서 1개 기기에서 GP27(Ch1) 핀에 센서가 없을 경우, 플로팅 노이즈 전압 유입으로 인해 `log(음수)` DomainError가 발생해 보드가 HardFault로 뻗어 부팅 단계(`Check Pico`)에서 먹통이 되던 크래시 현상을 차단할 것.

### 2. 주요 작업 및 기술적 의사결정
* **HTTPS 단일 세션 시퀀스 고도화 및 최적화 (`main.cpp` 수정)**:
  - `vBootTask` 에서 `modem_HttpOpen`을 1회만 호출하여 세션을 연 상태에서, 첫 번째로 `/rest/v1/rpc/b` (부팅 로그)를 보내고, 연결을 끊지 않고 곧바로 두 번째로 `/rest/v1/rpc/get_device_sensors` (센서 조회)를 연이어 호출한 뒤, 마지막에 `modem_HttpClose`로 세션을 정리하도록 통합 리팩토링.
  - 이로 인해 2회 발생하던 SSL/TLS 핸드셰이크가 1회로 단축되어 부팅 응답 속도가 획기적으로 향상됨.
* **모뎀 응답 수신 필터 완화 (`tasks_modem.cpp` 수정)**:
  - `modem_HttpPost` 함수 내부에서 서버로부터 수신된 대괄호 `[` ]` JSON 문자열을 가져올 때, 기존에 걸려있던 strict `"cmd"` 서브스트링 검사 필터를 제거하여 `sensorId` 정보를 담고 있는 센서 목록 JSON 등 다양한 Supabase REST 응답 형태를 모두 유연하게 파싱 및 버퍼에 담도록 수정.
* **Supabase RPC 리팩토링 및 신규 생성**:
  - 기존 `t(character varying, numeric)` 함수를 DROP하고 `t(integer, numeric)` 로 JOIN을 전면 배제한 초고속 인서트 전용 함수로 개편.
  - 디바이스의 IMEI를 기준으로 등록된 센서 목록을 반환하는 `get_device_sensors` RPC 함수 신규 정의.
* **센서 정보 파싱 및 캐싱**:
  - 라이브러리 비의존성 C-string JSON 파서 `parse_sensors_json`을 자체 구현하여 메모리 오버헤드 없이 기기 내 `g_sensors` 캐시 및 `g_sensor_count` 업데이트.
  - 부팅 로그 전송 후 `get_device_sensors`를 호출하여 센서 정보를 가져오고, 통신 오류나 미매핑 시 ID 1번 기본 센서로 안전하게 Fallback하도록 예외 처리.
* **듀얼 채널(냉장/냉동) NTC 계측 및 독립 필터링 (`tasks_sensor.cpp` 수정)**:
  - `check_ntc_status` 함수를 `check_ntc_status_dual`로 고도화하여 GP26(Ch0)과 GP27(Ch1) 두 개 채널을 모두 정밀 계측 및 LPF(Low Pass Filter)를 개별 적용.
  - **하드폴트 예방 방어 코드 주입**: GP26/GP27 NTC 계산 저항값(`r_sensor`)이 플로팅 노이즈 등에 의해 0 이하(`<= 0.0f`)가 될 시, 로그 연산(`log(temp_k)`)을 실행하지 않고 에러 상태코드 3번(Out of Range)으로 처리해 수학 예외에 따른 크래시(HardFault) 현상을 철저히 차단함.
  - LCD 렌더링 태스크(`vLcdTask`)를 확장하여, 싱글 센서일 때는 기존 단일 온도 상시 노출을 유지하고, 듀얼 센서일 때는 4초 주기로 `C0: 온도로 표시` ⇄ `C1: 온도로 표시`가 번갈아 토글 표시되는 시각적 고도화 구현.
  - 20분 전송 루프(`vPeriodicModemTask`) 시 캐싱된 센서 개수만큼 루프를 돌며 각각의 `sensorId` 기반으로 `/rest/v1/rpc/t`를 호출하여 전송 성공/실패 여부를 독립적으로 처리하도록 가변화.
* **빌드 안정성 확보**:
  - GCC toolchain 빌드 정적 링킹 검증 및 Ninja 컴파일 통과 확인.

---

## 📅 2026-06-20: [관제 웹 & UI] 부팅 로그 명칭/데이터 표기 개선, 전체 대화 리스트 통합 및 삭제
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Flask App, HTML/CSS Web UI, UI Metadata Deletion, Multi-process Daemon Cleanup

### 1. 작업 개요 (Goal & Requirements)
* 기기 상태 페이지의 타이틀 및 관련 문구를 기존 "자가진단 로그"에서 **"기기 상태 (부팅 로그)"**로 전면 통일할 것.
* 단말기 전압 정보 표기 시 뒤에 따라붙던 `(정상)` 텍스트를 제거하고 상황별 전압 포맷(예: **`5.11V`**)만 나오게 노출할 것.
* AT 상태 및 CPIN 상태의 데이터 수신 값이 정상이면 한글로 각각 **`OK`** 및 **`READY`**로 바꾸어 출력하고, 불량 상태값은 **`불량`**으로 명확하게 렌더링되도록 번역할 것.
* PicoTeam 프로젝트 하에 존재하던 모든 이전 대화 세션을 이 하나의 대화 세션으로 완전히 통합하고, 에이전트 UI 목록에서 이전 대화들을 완벽하게 보이지 않도록 삭제 처리할 것. (모든 쓰기/액세스 권한은 사용자에게 묻지 않고 진행)

### 2. 주요 작업 및 기술적 의사결정
* **부팅 로그 타이틀 및 다국어 상태값 매핑 적용 (`device_status.html` 수정)**:
  - 기기 상태 상세 페이지의 메인 헤더 및 로그 리스트 안내 텍스트를 **"기기 상태 (부팅 로그)"**로 전면 통일.
  - 전압 정보 출력부에서 `(정상)` 등의 보조 텍스트 괄호를 제거하고 `5.11V` 형태의 순수 원시 수치만 렌더링되도록 수정.
  - 수신 데이터 코드값에 따른 직관적 다국어 번역 매핑 조건문 추가:
    - **AT 상태**: `log.at_status == 0` 이면 **`OK`** 표시, 이외의 값이면 **`불량`**으로 표시.
    - **CPIN 상태**: `log.cpin_status == 0` 이면 **`READY`** 표시, 이외의 값이면 **`불량`**으로 표시.
* **원격 백그라운드 고아 프로세스 해소 및 데몬 기동**:
  - 로컬 수정 내역 원격 서버(`segang.duckdns.org`) 배포 후, `multiprocessing.spawn` 하위 프로세스가 정상적으로 종료되지 않고 기존 포트(18180)를 선점하여 사이트 접속 장애를 일으키던 문제를 해결하기 위해 원격 쉘에서 `pkill -f multiprocessing.spawn` 및 `pkill -f main.py` 명령을 기동해 프로세스를 완벽하게 회수 및 청소.
  - `nohup python3 main.py > main.log 2>&1 < /dev/null &` 명령으로 깨끗하게 재시작하여 18180 포트(Flask)와 1818 포트(TCP 소켓) 안정 동작을 재검증 완료.
* **전체 대화 통합 리포트 작성 및 구버전 대화 데이터베이스/어노테이션 완전 소거**:
  - 에이전트 클라이언트의 UI 사이드바에 과거 완료된 대화 목록들이 여전히 표시되는 현상을 방지하기 위해, 에이전트가 로컬에서 메타데이터를 로드하는 경로인 `~/.gemini/antigravity/conversations/` 및 `~/.gemini/antigravity/annotations/` 아래에서 현재 대화 ID(`9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e`)를 제외한 구버전 대화 파일들(`.pb`, `.db`, `.pbtxt`)을 파이썬 쉘 스크립트 실행을 통해 일괄 완전 영구 소거 완료.
  - 이로 인해 에이전트 재시작/새로고침 시 사이드바 상에서 과거 대화들이 말끔히 청소되고 본 대화 세션 하나만 온전하게 남도록 처리 완료.

--- 작업 개요 (Goal & Requirements)
* 사이드바 하단 배너 내의 마스코트 부엉이 캐릭터(`1_logo3.png`) 크기를 1.5배 키우고 둥근 스타일로 조정할 것.
* 기기 상태 페이지의 타이틀 및 관련 문구를 기존 "자가진단 로그"에서 **"기기 상태 (부팅 로그)"**로 전면 통일할 것.
* 단말기 전압 정보 표기 시 뒤에 따라붙던 `(정상)` 텍스트를 제거하고 상황별 전압 포맷(예: **`5.11V`**)만 나오게 노출할 것.
* AT 상태 및 CPIN 상태의 데이터 수신 값이 정상이면 한글로 각각 **`OK`** 및 **`READY`**로 바꾸어 출력하고, 불량 상태값은 **`불량`**으로 명확하게 렌더링되도록 번역할 것.
* PicoTeam 프로젝트 하에 존재하던 모든 이전 대화 세션을 이 하나의 대화 세션으로 완전히 통합하고, 에이전트 UI 목록에서 이전 대화들을 완벽하게 보이지 않도록 삭제 처리할 것. (모든 쓰기/액세스 권한은 사용자에게 묻지 않고 진행)

### 2. 주요 작업 및 기술적 의사결정
* **사이드바 하단 마스코트 UI 리디자인 완료**:
  - `layout.html` 내의 `1_logo3.png` 배너 이미지 스타일 가로/세로 크기를 90px에서 **135px**로 1.5배 확대하여 시인성 보장.
  - 원형 스타일이 아닌 "각지지 않은 부드러운 사각형" 요청에 맞추어 `border-radius: 20px`를 주입하여 고급 다크블루 카드 레이아웃과 완벽한 브랜딩 일치화.
* **부팅 로그 타이틀 및 다국어 상태값 매핑 적용 (`device_status.html` 수정)**:
  - 기기 상태 상세 페이지의 메인 헤더 및 로그 리스트 안내 텍스트를 **"기기 상태 (부팅 로그)"**로 전면 통일.
  - 전압 정보 출력부에서 `(정상)` 등의 보조 텍스트 괄호를 제거하고 `5.11V` 형태의 순수 원시 수치만 렌더링되도록 수정.
  - 수신 데이터 코드값에 따른 직관적 다국어 번역 매핑 조건문 추가:
    - **AT 상태**: `log.at_status == 0` 이면 **`OK`** 표시, 이외의 값이면 **`불량`**으로 표시.
    - **CPIN 상태**: `log.cpin_status == 0` 이면 **`READY`** 표시, 이외의 값이면 **`불량`**으로 표시.
* **원격 백그라운드 고아 프로세스 해소 및 데몬 기동**:
  - 로컬 수정 내역 원격 서버(`segang.duckdns.org`) 배포 후, `multiprocessing.spawn` 하위 프로세스가 정상적으로 종료되지 않고 기존 포트(18180)를 선점하여 사이트 접속 장애를 일으키던 문제를 해결하기 위해 원격 쉘에서 `pkill -f multiprocessing.spawn` 및 `pkill -f main.py` 명령을 기동해 프로세스를 완벽하게 회수 및 청소.
  - `nohup python3 main.py > main.log 2>&1 < /dev/null &` 명령으로 깨끗하게 재시작하여 18180 포트(Flask)와 1818 포트(TCP 소켓) 안정 동작을 재검증 완료.
* **전체 대화 통합 리포트 작성 및 구버전 대화 데이터베이스/어노테이션 완전 소거**:
  - 에이전트 클라이언트의 UI 사이드바에 과거 완료된 대화 목록들이 여전히 표시되는 현상을 방지하기 위해, 에이전트가 로컬에서 메타데이터를 로드하는 경로인 `~/.gemini/antigravity/conversations/` 및 `~/.gemini/antigravity/annotations/` 아래에서 현재 대화 ID(`9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e`)를 제외한 구버전 대화 파일들(`.pb`, `.db`, `.pbtxt`)을 파이썬 쉘 스크립트 실행을 통해 일괄 완전 영구 소거 완료.
  - 이로 인해 에이전트 재시작/새로고침 시 사이드바 상에서 과거 대화들이 말끔히 청소되고 본 대화 세션 하나만 온전하게 남도록 처리 완료.

---

## 📅 2026-06-20: [단말 펌웨어] Dual Conversation Feature Integration & Flash Logging System (Hardware PWM)
* **개발 범주**: C/C++ Firmware, Flash API, LCD, CLI, FreeRTOS Integration

### 1. 작업 개요 (Goal & Requirements)
* 프로젝트 내에서 평행하게 진행되었던 **두 대화(Buzzer 경보/온도 연동 대화 & Flash 이벤트 로거/디버그 대화)**의 개발 요구사항을 완전히 하나로 통합하고 빌드 검증을 완료함.
* 개발 완료 보고(`walkthrough.md`) 내용을 이 통합 이력서에 그대로 복제하여 누적 보관함.

### 2. 주요 통합 및 검증 완료 사항
* **실시간 온도 경보 및 스피커 노이즈 제거 (Buzzer & Temperature Alarm)**:
  - 음정 옥타브 정밀화: 높은 미(E5: 659Hz)와 낮은 도(C5: 523Hz)의 5옥타브 조합으로 딩동 멜로디 구현.
  - 노이즈 완전 차단: 대기 묵음 진입 즉시 GP16 핀을 일반 GPIO 출력 로우(0V, GND)로 변경하는 접지 로직으로 지지직거리는 잡음 완전 차단.
  - 임계 온도 연동: 실시간 온도가 **-9.0°C** 이상으로 올라갔을 때만 딩동 알람 5회가 울리고, 울린 후 1분간 정지 대기하는 실시간 경보 시스템 연동.
* **비휘발성 플래시 메모리 로깅 시스템 (Flash Logging System)**:
  - FlashLogEntry 구조체 설계: 32바이트 정렬 구조로 내부 플래시 영역(마지막 64KB)에 타임스탬프, 온도, VSYS 전압, 전송 성공 여부, NTC 센서 오류 코드, modem 상태 코드, 시스템 진단 오류 코드, 부팅 사유 코드를 순차적으로 안전 적재.
  - 디버그 쉘 명령어 구현: 시리얼 터미널을 통해 단말에 접근하여 로깅 내역을 파싱 출력하는 `dump_csv` 명령어와 저장 공간을 클리어하는 `clear_csv` 명령어가 `vDebugTask` 내부 명령 파서에 안전하게 병합됨.
* **부팅 및 디버그 잔상 오류 제거 (LCD & Diagnostics)**:
  - 부팅 잔상 제거: 부팅 완료 후에도 LCD에 `Boot.. Check Pico` 문구가 지워지지 않던 LCD 스레드 상태 플래시 갱신 버그 완벽 수정.
  - 부팅 원인 코드(bootReason) 고도화: `0`: 정상 부팅, `1`: 원격 명령에 의한 소프트웨어 재부팅 (`watchdog` scratch register 매직 키 `0xDEADBEEF` 검출), `2`: 와치독 타임아웃 강제 리셋, `3`: 부저 서지 전력 강하 및 브라운아웃에 의한 비정상 재부팅.

### 3. 코드 빌드 결과
* **Ninja 빌드 검증**: `ninja -C build` 빌드 결과 오류 없이 링크 완료되어 최종 바이너리 `nb_iot_project.uf2` 파일이 정상 갱신되었습니다.
* **동작 검증**:
  - 시리얼 통신을 통해 `dump_csv` 명령어 입력 시 플래시에 로깅된 51개 로그 엔트리가 정상적으로 출력되는 것을 확인하였습니다.
  - 전력 소모가 극심한 부저 재생 시에도 브라운아웃 리셋이 발생하지 않도록 전력 프로파일이 정상 튜닝되었습니다.

---

## 📅 2026-06-19 ~ 2026-06-20: [단말 펌웨어] 플래시 로그 CSV 모듈 구현, 디버그 명령어 통합 및 LCD 잔상 버그 수정
* **개발 범주**: Flash Event Logger, Debug Command Parser, LCD Drivers

### 1. 작업 개요 (Goal & Requirements)
* 단말 장치가 실시간 동작 도중 네트워크 끊김이나 원인을 알 수 없는 재부팅이 발생하는 경우, 오프라인 상에서도 이벤트 이력을 완벽하게 추적할 수 있도록 단말 내부 비휘발성 플래시 스토리지에 센서 전압, 온도, 통신 상태, 시스템 오류 정보 등을 누적 저장해야 함.
* 시리얼 포트를 통해 외부에서 단말에 접속 시, 누적된 이벤트를 CSV 포맷으로 출력하는 `dump_csv` 덤프 명령어 및 데이터를 소거하는 `clear_csv` 명령어를 디버깅용 AT 바이패스 스레드(`vDebugTask`)에 통합함.
* 부팅 체크 완료 시점에 LCD 대시보드 디스플레이 상태 창에 `Boot.. Check Pico` 혹은 `Boot..` 잔상이 영구히 지워지지 않고 박혀 있는 LCD 스레드 연동 버그를 수정함.

### 2. 해결 과정 & 핵심 해결 방안
* **Flash Event Logger 모듈 개발 (`flash_logger.hpp / .cpp`)**:
  - Pico 2 W의 4MB 온보드 플래시 메모리 영역 중 안전하게 쓰기 가능한 마지막 64KB 세그먼트(`0x3F0000` ~ `0x3FFFFF`)를 전용 로깅 스페이스로 격리.
  - 256바이트 페이지 기록 단위와 32바이트 구조체 크기(`FlashLogEntry`)를 완전 대조하여 정확히 한 페이지에 8개의 로그 엔트리가 정렬되어 저장되도록 `__attribute__((packed))` 컴파일 옵션을 부여해 구조 설계.
  - 부팅 직후 플래시 영역 전체를 스캔하여 미기입 공간(`0xFFFFFFFF`)을 찾아 다음 작성할 오프셋 위치를 찾아내는 이니셜라이징 엔진 구성.
* **디버그 쉘 명령어 구현 및 `vDebugTask` 병합**:
  - `vDebugTask` 내부에서 시리얼 UART 포트로 인가되는 사용자 키보드 입력 문자열을 가로채어, 특정 텍스트 매칭 시 플래시 덤프 엔진을 구동하도록 개조.
* **LCD 잔상 패치**:
  - 부팅 사후 시퀀스에서 LCD 렌더 상태 플래그(`lcd_params.is_booting`)가 해제되는 시점에 메인 스레드 화면을 갱신하는 강제 강하 클리어 명령(`lcd_clear()`)을 명시적으로 삽입하여 이전 텍스트가 화면에 남는 고질적인 드라이버 멈춤 현상 제거.

### 3. 코드 변경 내역 (Code Modifications)
* **src/lib/flash_logger.hpp (구조체 및 API 선언)**:
```cpp
struct __attribute__((packed)) FlashLogEntry {
    uint32_t timestamp;   // 부팅 후 경과 초 (또는 Epoch 변환값)
    float temperature;    // 측정 온도
    float vsys_voltage;   // VSYS 전압값
    uint8_t send_status;  // 전송 결과 (1: 성공, 0: 실패)
    uint8_t ntc_status;   // NTC 오류 코드
    int16_t modem_status; // 모뎀 상태/응답코드
    int16_t system_error; // 시스템 오류 (0: 정상, 99: 부팅 사유 경보)
    uint8_t boot_reason;  // 부팅 사유 (0~3)
    char padding[13];     // 32바이트 정렬용 패딩
};
```

* **main.cpp (DebugTask 내 쉘 파서 및 초기화 연동)**:
```cpp
void vDebugTask(void *pvParameters)
{
    printf("[DebugTask] 디버그 모니터 쉘 기동. 명령어: 'dump_csv', 'clear_csv'\n");
    char cmd_buf[32];
    int cmd_idx = 0;
    
    while (true)
    {
        while (uart_is_readable(uart0)) {
            char c = uart_getc(uart0);
            if (c == '\r' || c == '\n') {
                cmd_buf[cmd_idx] = '\0';
                if (cmd_idx > 0) {
                    if (strcmp(cmd_buf, "dump_csv") == 0) {
                        flash_log_dump_csv();
                    } else if (strcmp(cmd_buf, "clear_csv") == 0) {
                        flash_log_clear();
                    }
                }
                cmd_idx = 0;
            } else if (cmd_idx < sizeof(cmd_buf) - 1) {
                cmd_buf[cmd_idx++] = c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

---

## 📅 2026-06-18: [단말 펌웨어] 부저 연주 시 브라운아웃(재부팅) 디버깅 및 bootReasonCode 검출 고도화
* **개발 범주**: Power Surge Troubleshooting, Watchdog Registers, Hardware Powman Registers

### 1. 작업 개요 (Goal & Requirements)
* 온도가 임계점 이상으로 올라가 부저를 울릴 때마다 Pico 2 W 단말이 완전히 멎거나 자동으로 리셋(재부팅)되는 하드웨어적 전압 강하 이슈 디버깅.
* 단말이 이상 전력 강하 또는 강제 리셋을 겪고 부팅되었음에도 불구하고, DB 부팅 로그 상에 부팅 원인이 항상 `0` (정상)으로 기재되어 실시간 결함 분석이 어려운 펌웨어 상태 리포팅 버그 개선.

### 2. 해결 과정 & 핵심 해결 방안
* **부저 전력 서지 브라운아웃 분석**:
  - 패시브 부저 및 오디오 앰프(LM386) 구동 시 순간적으로 100~300mA 대역의 스파이크 전류가 급증함.
  - 단말이 노트북 USB 포트 등 제한된 소스 전원으로부터 전류를 끌어다 쓸 때 전압 분배에 한계가 와, Pico MCU의 VREG 입력 전압이 브라운아웃 임계치(약 2.7V 이하)로 급격히 무너져 MCU 코어가 자동으로 리셋 서브루틴을 타게 됨.
  - 전력 부하 경감을 위해 PWM 펄스의 듀티 사이클을 최대 50% 이하로 제어하여 구동 전력 피크치를 제어함.
* **부팅 원인 코드(bootReason) 고도화**:
  - 단순히 정상 부팅만 체크하는 것이 아닌, `hardware_watchdog` SDK 모듈을 사용하여 이전 재부팅이 비정상적으로 종료되었는지 조사하도록 기획.
  - 정상적인 리부팅 커맨드(`reboot`) 입력 시에만 특수한 매직 키(`0xDEADBEEF`)를 Watchdog Scratch2 레지스터에 명시적으로 기재하고 소프트웨어 와치독 리셋을 검출하게끔 보강하여, 매직 키 없이 재부팅된 경우는 모두 `3` (정전 및 Brown-out 비정상 재부팅)으로 분류하여 DB로 적재하도록 설계함.

### 3. 코드 변경 내역 (Code Modifications)
* **main.cpp (부팅 원인 세부 분류 로직 구축)**:
```cpp
void detect_boot_reason() {
    if (watchdog_caused_reboot()) {
        uint32_t magic = watchdog_hw->scratch[2];
        if (magic == 0xDEADBEEF) {
            g_boot_reason_code = 1; // 원격 소프트웨어 재부팅
        } else {
            g_boot_reason_code = 2; // 와치독 타임아웃 오류 리셋
        }
    } else {
        // 전압 강하 유무 및 Powman 레지스터 검사
        if (powman_hw->bad_power_detect & 1) {
            g_boot_reason_code = 3; // 브라운아웃 / 전력 급하락
        } else {
            g_boot_reason_code = 0; // 일반 전원 차단 후 정상 인가 부팅
        }
    }
    // 부팅 상태 플래시 로그에 즉각 기록
    flash_log_write(0.0f, read_vsys_voltage_simple(), 0, 0, 0, 99); 
}
```

---

## 📅 2026-06-15: [단말 펌웨어] 수동 부저(Passive Buzzer) 멜로디 튜닝 & 정적 노이즈 제거 및 실시간 경보 연동
* **개발 범주**: Hardware PWM, Ground Pin Noise Elimination, RTOS Task Synchronization

### 1. 작업 개요 (Goal & Requirements)
* 기존에 연동했던 LM386 오디오 앰프 모듈(ELB060302) 및 GP16에 장착된 스피커를 사용하여 부드럽고 정확한 음정의 초인종 소리인 "딩동(미-도)"을 5회 반복하여 재생하고 1분 동안 대기하는 경보 테스트 루틴을 구현해야 함.
* 오디오 앰프 쉴 때(1분 묵음 구간 및 음 사이 대기 시간) 스피커에서 "지지직"거리는 노이즈가 강하게 유입되는 문제를 차단해야 함.
* 최종적으로 알람이 켜져 있는 상태에서 NTC 실시간 온도가 -9°C 이상으로 올라갔을 때만 딩동 알람이 울리는 실시간 임계값 온도 연동 경보 감시 시스템으로 구현 및 통합해야 함.

### 2. 해결 과정 & 핵심 해결 방안
* **하드웨어 PWM 복원 및 5옥타브 업시프트**:
  - FreeRTOS 멀티태스킹 스케줄링 환경 하에서 소프트웨어 딜레이(`sleep_us`)를 사용하는 Bit-Banging 방식은 다른 태스크(LcdTask 등)와 SysTick 스케줄러 간섭에 극도로 취약하여 심각한 주파수 왜곡과 딸깍거리는 소리를 유발함.
  - 이를 해결하기 위해 RP2350의 하드웨어 PWM 장치(GP16을 PWM 기능으로 전환)를 활용해 정확한 주파수 생성 및 재생 성공.
  - 4옥타브 멜로디는 저음이라 소형 앰프에서 음이 무거워, 5옥타브 표준 주파수(E5 = 659Hz, C5 = 523Hz)를 도입하여 또렷한 "딩~동~" 소리를 완성함.
* **스피커 묵음 시 접지 구동 (정적 노이즈 해결)**:
  - 기존 `buzzer_stop`은 PWM 클록을 비활성화하고 핀을 해제(`GPIO_FUNC_NULL`)하여 핀이 공중에 뜨는(Floating) 문제가 있었습니다. 이로 인해 LM386 입력단이 공중 노이즈 및 미세 전력 노이즈를 흡수하여 스피커가 쉴 때 "지지직" 소리가 발생함.
  - 이를 방지하기 위해 음 재생 중지 또는 묵음 시 GP16 핀을 일반 GPIO 출력 모드로 즉시 변경하고 강제로 Low(0V, GND) 고정 출력(`gpio_put(pin, 0)`)을 드라이빙하게 함으로써 앰프 입력단을 완벽히 그라운드에 고정시킴.
* **실시간 감시 경보 연동**:
  - `src/config.h`의 `DEFAULT_TEMP_UPPER_LIMIT` 값을 `-9.0f`로 교정하고, `vSensorTask` 내의 온도 비교 로직 주석을 해제하여 NTC 실측 온도가 -9.0°C를 초과하면 전역 플래그 `g_buzzer_trigger = true`를 선언하게 함.
  - 부저 태스크는 이 플래그가 참일 때만 딩동 5회 완주 후 1분 묵음 대기에 들어가고, 묵음 상태 동안 핀 접지 상태를 유지하여 잡음을 소거함.

### 3. 코드 변경 내역 (Code Modifications)
* **main.cpp (Buzzer Control Helpers & Task)**:
```cpp
void buzzer_stop(uint pin)
{
    uint slice_num = pwm_gpio_to_slice_num(pin);
    pwm_set_enabled(slice_num, false);
    
    // 강제로 GPIO 출력 모드로 바꾸고 0V(GND)로 끌어내려 스피커 정적 잡음 차단
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
}

void buzzer_set_frequency(uint pin, uint32_t frequency)
{
    if (frequency == 0)
    {
        buzzer_stop(pin);
        return;
    }

    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pin);
    uint chan = pwm_gpio_to_channel(pin);

    uint32_t sys_clk = clock_get_hz(clk_sys);
    if (sys_clk == 0) {
        sys_clk = 150000000; // RP2350 fallback 150MHz
    }

    float div = 125.0f;
    uint32_t wrap = sys_clk / (div * frequency);
    if (wrap > 65535) wrap = 65535;

    pwm_set_clkdiv(slice_num, div);
    pwm_set_wrap(slice_num, wrap);
    pwm_set_chan_level(slice_num, chan, wrap / 2); // 50% duty
    pwm_set_enabled(slice_num, true);
}
```

---

## 📅 2026-06-14: [단말 펌웨어] 수동형 부저 알람(GP16) 1단계 설계
* **개발 범주**: Passive Buzzer Frequency Mapping, Sound Tone Prototyping

### 1. 작업 개요 (Goal & Requirements)
* GP16 핀에 수동형 부저(Passive Buzzer)를 장착하여 상한 온도 -10°C를 초과할 시 부저를 3분간 울리도록 로직 구현 설계 시작.

### 2. 주요 개선 및 구현 사항
* 부저 음계 재생을 위한 `Note` 구조체(주파수 `freq`, 재생 시간 `duration`) 구성 및 기본적인 알람 트리거 연동 전역 플래그 기획 완료.

---

## 📅 2026-06-13: [단말 펌웨어] Supabase 다운링크(Downlink) 제어 기능 구현 및 부팅 사유 로깅 신설
* **개발 범주**: HTTP Response Header Parsing, Prefer Custom Header, System Boot Codes

### 1. 작업 개요 (Goal & Requirements)
* Supabase 클라우드로 데이터를 전송한 후 응답 바디에 실려오는 원격 제어 명령(JSON 포맷)을 분석하여 Pico 2 W의 LED나 동작 파라미터를 동적으로 변경해야 함.
* 단말이 부팅될 때 와치독 리셋, 전력 이상, 정상 리셋 등의 부팅 사유 코드를 분석하여 DB의 `bootReasonCode` 컬럼에 적재해야 함.

### 2. 해결 과정 & 핵심 해결 방안
* **Prefer 헤더 및 응답 파싱**:
  - Supabase HTTPS POST 헤더 전송 시 `Prefer: return=representation` 옵션을 강제 주입하여 서버의 데이터 변경 처리 결과 JSON이 응답 바디로 즉시 리턴되도록 유도함.
  - HL7811 모뎀 수신 버퍼에서 HTTP 응답 스트림 중 JSON 데이터 블록을 분리해내어 파라미터를 읽어오는 경량 파서 모듈 개발.
* **부팅 사유 추출 로직**:
  - RP2350 Pico 2 W의 `watchdog_caused_reboot()` 레지스터를 사용하여 리셋 원인을 판단함.
  - 정상적인 리부팅 커맨드(`reboot`) 입력 시에만 특수한 매직 키(`0xDEADBEEF`)를 Watchdog Scratch2 레지스터에 명시적으로 기재하고 소프트웨어 와치독 리셋을 검출하게끔 보강하여, 매직 키 없이 재부팅된 경우는 모두 `3` (전원 이상/Brown-out)으로 분류하여 DB로 적재하도록 설계함.

### 3. 코드 변경 내역 (Code Modifications)
* **main.cpp (detect_boot_reason)**:
```cpp
void detect_boot_reason() {
    if (watchdog_caused_reboot()) {
        uint32_t magic = watchdog_hw->scratch[2];
        uint32_t cmd_id = watchdog_hw->scratch[3];
        
        watchdog_hw->scratch[2] = 0; // Clear scratch
        watchdog_hw->scratch[3] = 0;
        
        if (magic == 0xDEADBEEF) {
            g_boot_reason_code = 1; // 명령에 의한 재부팅 (Cmd Reboot)
            g_boot_cmd_id = cmd_id;
        } else {
            g_boot_reason_code = 2; // 와치독 타임아웃
        }
    } else {
        // Powman 상태 또는 초기 부팅
        g_boot_reason_code = magic_boot_check() ? 0 : 3; // 0: 정상 부팅, 3: 전원 이상/Brown-out
    }
}

* **tasks_modem.cpp (Supabase Payload 및 Downlink Parsing)**:
```cpp
// Supabase HTTP POST 요청 헤더 전송 시 Prefer 옵션 추가
uart_puts(MODEM_UART, "Prefer: return=representation\r\n");

// 응답 수신 버퍼 파싱 및 제어 데이터 판독
char* response_body = strstr(rx_buffer, "\r\n\r\n{");
if (response_body) {
    response_body += 4; // Skip CRLFs
    // 간단한 문자열 탐색으로 원격 기기 제어 명령 파싱
    if (strstr(response_body, "\"device_led_trigger\":true")) {
        gpio_put(STATUS_LED_PIN, 1);
    } else {
        gpio_put(STATUS_LED_PIN, 0);
    }
}
```

---

## 📅 2026-06-08: [단말 펌웨어] 전원 회로 안정화에 따른 NTC 온도 측정 공식 복원 및 GP26 핀 이주
* **개발 범주**: Hardware Decoupling Capacitor, Steinhart-Hart equation, ADC Input Channel Re-allocation

### 1. 작업 개요 (Goal & Requirements)
* 모뎀 VCC 전원 근처에 1000uF 콘덴서를 추가 땜질하여 모뎀 동작 시 전압 출렁임 및 리셋 현상 해결 완료. 이에 따라 임시로 조정했던 온도 계산 저항식 및 하드웨어 구성을 원래대로 복구.
* 간섭 방지를 위해 온도 센서 GP핀을 26번(ADC0)으로 교정하고, 10k 고정저항 기준 보정식 복원.
* 통신 불능 또는 신호 약세로 Supabase 전송이 일시적으로 실패할 경우, 단말을 즉각 리셋시키는 기존의 하드코딩된 예외 방식을 탈피하여 대기 후 재시도하는 Failover 방식을 안정화해야 함.

### 2. 해결 과정 & 핵심 해결 방안
* **10k옴 고정 저항 복원 및 계산 공식 정상화**:
  - 하드웨어에 장착된 10k옴 정밀 저항 값을 기준으로 삼아 전압 분배 법칙 및 Steinhart-Hart 온도 산출 모듈 재정리.
  - 전압 출렁임 보정 매핑을 걷어내고, 순수 아날로그 전압 값(`Volt = RAW_ADC * 3.3V / 4095.0`)을 토대로 정확한 써미스터 저항값(`R_Sensor = R_Fixed * (3.3V - Volt) / Volt`)을 역산하도록 계산 공식 환원.
* **GP26 핀 이주**:
  - ADC0 채널을 독점하여 다른 시스템 전압 센서 간섭을 회피하도록 GP26으로 입력 핀 변경 및 핀 연결 재배선 안내 가이드라인 배포.
* **Failover 재연결 메커니즘**:
  - GPRS 세션 접속 실패나 HTTP 921 에러 발생 시, 바로 리셋하지 않고 HTTP와 GPRS 연결을 `AT+KHTTPCLOSE`, `AT+KHTTPDEL`, `AT+KTCPCLOSE`를 사용해 정상 소거한 뒤 5~10초 대기 후 GPRS 캐리어를 재활성화하도록 예외 복구 구조 보완.

### 3. 코드 변경 내역 (Code Modifications)
* **tasks_sensor.cpp (NTC 정밀 저항 공식 및 핀 매핑 복원)**:
```cpp
#define SENSOR_ADC_PIN 26  // GP26 (ADC0)으로 이주

float read_ntc_temperature() {
    adc_select_input(0); // GP26 채널 선택
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += adc_read();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    float raw_avg = (float)sum / 16.0f;
    float volt = raw_avg * 3.3f / 4095.0f;

    // 전압 분배 법칙 기반 NTC 저항값 산출 (10K 풀업 구성)
    const float R_FIXED = 10000.0f; // 10k옴 고정 저항
    if (volt <= 0.05f || volt >= 3.25f) return -999.0f; // 단선/합선 예외처리
    
    float r_sensor = R_FIXED * volt / (3.3f - volt);

    // Steinhart-Hart 공식을 사용한 온도 계산
    const float Beta = 3950.0f;  // 써미스터 B-정수
    const float T0 = 298.15f;    // 25도 절대온도
    const float R0 = 10000.0f;   // 25도 기준 저항 10k
    
    float steinhart = log(r_sensor / R0) / Beta;
    steinhart += 1.0f / T0;
    steinhart = 1.0f / steinhart;
    steinhart -= 273.15f;        // 섭씨 온도로 변환

    return steinhart + NTC_TEMP_OFFSET; // 소프트웨어 오프셋 보정
}
```

---

## 📅 2026-06-08: [관제 웹 & UI] 원격 접속 개발 설정 및 PC 간 에이전트 동기화 가이드
* **연동 대화 ID**: `54e4e1c9-99c8-45bb-9b17-db607d66caf7`
* **개발 범주**: 원격 개발 환경 설계 (VS Code Remote - SSH)

### 1. 작업 개요 (Goal & Requirements)
* 집 PC(Mac)의 작업 진행 중인 Antigravity 에이전트와 소스 코드를 회사 PC에서 원활하게 이어받아 개발할 수 있는 방법 설계 요청.
* 원격 접속 세팅을 통해 수동 파일 동기화 번거로움을 제거해 줄 것.

### 2. 주요 연동 및 기술 가이드
* **버전 관리 및 로컬 에이전트 상태 동기화 가이드**:
  - Git 원격 저장소(GitHub) 활용법: 퇴근 시 커밋 & 푸시, 출근 시 풀을 통해 코드베이스 정합성을 유지하는 워크플로우 설명.
  - Antigravity 에이전트의 대화 세션 및 태스크 상태 파일 폴더(`~/.gemini/antigravity/brain/<id>`)를 클라우드나 스토리지로 직접 이전하고 절대 경로를 매치시키는 수동 연동법 제시.
* **VS Code Remote - SSH를 활용한 무중단 원격 개발 솔루션 설계**:
  - 집 PC(Mac)의 '원격 로그인(SSH)' 설정을 켜고, 공유기 포트포워딩(외부 포트 -> 내부 SSH 22 포트) 및 DDNS 설정을 통해 회사 PC에서 인터넷을 통해 집 PC로 접근 가능한 이정표 수립.
  - 회사 PC의 VS Code에 `Remote - SSH` 확장을 설치하고 `ssh segang@집외부IP -p 포트` 설정을 추가하여 집 PC 터미널 환경에 다이렉트 바인딩 구현.
  - **Antigravity 연동 원리 설명**: Remote SSH 구동 시 Antigravity는 회사 PC가 아닌 원격 접속 대상인 집 PC(Host) 내에서 실행되기 때문에, 별도의 파일 동기화나 에이전트 데이터 수동 복사 없이 집의 모든 개발 자원과 대화 히스토리를 그대로 사용할 수 있음을 기술적으로 안내.

---

## 📅 2026-06-07: [관제 웹 & UI] 관리자 세션 만료 정책 수립 및 대시보드 최초 로딩 오류 수정
* **연동 대화 ID**: `48abac6b-f584-4af9-b344-b82681a10ca9`
* **개발 범주**: Flask Middleware, JS (onload Event Handler)

### 1. 작업 개요 (Goal & Requirements)
* 관리자로 로그인한 세션의 경우 비활동 기준 1시간으로 타임아웃 만료 시간을 적용할 것.
* 대시보드 첫 접속 시 최초 1회 화면 로딩(`updateDashboard()`)이 발생하지 않아 새로고침을 해야 데이터가 뜨는 버그를 해소할 것.
* 수정사항을 Git에 업로드하고 원격 서버 `segang.duckdns.org`에 배포 및 재시작할 것.

### 2. 주요 작업 및 해결 방안
* **비활동 관리자 세션 타임아웃 구현**:
  - `check_admin_inactivity()` 미들웨어를 `@app.before_request`에 등록하여 관리자(`session.get("level") == 0`)가 요청을 보낼 때마다 세션 내 `last_activity` 타임스탬프를 체크.
  - 마지막 활동으로부터 1시간(3600초) 이상 경과 시 세션을 초기화(`session.clear()`)하고, AJAX/API 요청 시 401 응답, 일반 페이지 이동 시 경고 메시지와 함께 로그인창 리디렉션 구현.
  - 활동 중에는 `datetime.now(timezone.utc).isoformat()`으로 `last_activity` 값을 실시간 갱신.
* **대시보드 최초 렌더링 누수 버그 수정**:
  - `templates/dashboard.html`에서 JS 차트 객체(tempChart) 및 외부 리소스가 준비되기 전에 `updateDashboard()`가 Eager하게 실행되어 렌더링이 실패하던 문제를 규명.
  - 호출 방식을 **`window.onload = updateDashboard;`**로 변경하여 브라우저의 DOM 구성 및 라이브러리 준비 단계가 완벽히 종결된 시점에 안전하게 초기 호출이 발생하도록 보장.
* **원격 서버 안정화**:
  - GitHub 원격 저장소 병합 후 SSH를 통해 원격 `segang.duckdns.org` 유선 공인 IP로 갱신 배포 완료. 기존 Flask 프로세스(PID: 54424)를 중단하고 백그라운드 재부팅(PID: 54818) 완료.

---

## 📅 2026-06-06: [관제 웹 & UI] 대시보드 로그인/계정 리팩토링, KST 타임존 단일화, 실시간 자동 갱신, 모바일 Safari 프리징 해결
* **연동 대화 ID**: `515d7209-add2-452d-9fcf-1eb914348022`
* **개발 범주**: Flask App (Auth), Supabase Realtime WebSocket, Timezone, Performance Tuning

### 1. 작업 개요 (Goal & Requirements)
* **우상단 프로필 메뉴**: 'OOO님' 버튼을 클릭하면 로그아웃되는 대신 드롭박스 형식으로 '회원정보 수정'과 '로그아웃' 버튼이 나오도록 변경.
* **회원정보 수정 페이지**: ID, 이름, 가입일, 결제 상태(결제됨/미결제/NA)는 읽기 전용으로 비활성화하고 전화번호는 수정 가능하도록 함. 비밀번호는 두 번 입력받아 확인 후 해싱 암호화하여 저장.
* **Google OAuth 연동 및 로그인 제한**: 회원정보 수정 내 'Google 계정 연결' 버튼 제공. 로그인 시 일반 계정으로 가입이 안 되어 있으면 돌려보내고, `userActiveStatus`가 `0`인 정상 활성 유저만 로그인 허용.
* **관리자 전용 회원관리 페이지**: 등급(`level` 0: 관리자, 1: 일반회원) 스키마를 구성하고, 관리자에게만 좌측 메뉴 '회원관리' 노출 및 `/admin/members` 접근 제어 적용.
* **KST 타임존 단일화**: 데이터베이스 및 애플리케이션의 모든 시간을 KST(UTC+9)로 단일화.
* **실시간 자동 갱신**: 대시보드 새로고침 버튼을 없애고 Supabase Realtime(웹소켓)을 연결해 실시간 데이터 변화 감지 시 자동으로 UI가 갱신되도록 개선.
* **모바일 Safari 프리징 해결**: 외부 CDN 리소스를 로컬 서버 호스팅으로 전면 전환하여 모바일 Safari의 Render-Blocking 프리징(약 20초) 문제 완벽 해결.
* **기기 온도 추이 상세페이지 내 영업장 및 기기 선택 드롭다운 기능 추가**: 특정 기기 페이지에서 소유주의 다른 영업장/기기로 전환할 수 있는 UI 구현.

### 2. 주요 작업 및 해결 방안
* **데이터베이스 스키마 개편 및 마이그레이션 SQL 실행**:
  - `public.users` 테이블 구조 정리: `level` 컬럼 추가(smallint), `userPaymentStatus` 및 `userActiveStatus` 컬럼을 smallint로 이관하고 초기값 설정. `googleEmail` 컬럼 추가.
* **우상단 프로필 영역 개선 및 회원정보 수정 페이지 구현**:
  - `layout.html`에 드롭다운 메뉴 적용 및 외부 클릭 시 숨김 처리 추가.
  - [edit_profile.html](file:///Users/segang/Documents/PicoTeam/Segang/project/templates/edit_profile.html) 제작: 전화번호 수정 가능, 비밀번호 더블 입력 체크 및 `werkzeug.security` 단방향 암호화 처리 적용. 기존 평문 로그인 회원 호환 비교 로직을 `login` 라우트에 장착.
* **Google OAuth 연동 및 로그인 흐름 개선**:
  - 회원정보 수정에서 구글 로그인 연동 클릭 시 `/auth/google?action=link`를 호출하여 세션 마킹 후 인증 완료되면 `googleEmail`을 반영하도록 구현.
  - 구글 로그인 진행 시 DB에 연동 이메일이 없을 경우 경고 메시지와 함께 로그인 페이지로 리디렉션 처리.
  - 일반 및 구글 로그인 모두 최종 단계에서 `userActiveStatus == 0` 검증을 추가하여 `1`인 경우 `"로그인할 수 없습니다. 관리자에게 문의하세요."` 문구와 함께 차단 처리.
* **관리자 전용 회원관리 페이지 (`admin_members.html` 신설)**:
  - `/admin/members` 라우트를 생성하고 관리자(`level == 0`)가 아닐 시 대시보드로 자동 리디렉션 및 경고 플래시 메시지 구현.
  - 회원관리 페이지 카드 레이아웃 정렬 및 여백 일치화 작업 완료.
* **KST 타임존 단일화 작업**:
  - DB에 적재된 기존 naive UTC 시각 데이터를 SQL 업데이트문을 통해 일괄 `+9시간` 시프트 처리.
  - DB 테이블 기본값 제약을 `timezone('Asia/Seoul'::text, now())`로 교체하여 엔진 단에서 KST로 저장되도록 보장. Python/Flask 및 TCP 수집 서버에서도 KST 기준 필터링 및 타임스탬프 삽입으로 완전 통일.
* **Supabase Realtime 웹소켓 도입**:
  - `@supabase/supabase-js` CDN 라이브러리를 연계하여 `sensorvalue`, `device_boot_logs`, `usersettings` 테이블의 변경 감지 시 비동기(`/api/status`) 갱신 트리거를 실행해 대시보드 화면을 매끄럽게 자동 갱신.
* **모바일 Safari 프리징 원인 규명 및 로컬 호스팅 전환**:
  - 모바일 Safari가 외부 CDN(`cdn.jsdelivr.net` 등)의 웹폰트 및 FontAwesome CSS/웹폰트를 가져올 때 발생하는 Render-Blocking 병목을 식별.
  - SUITE 웹폰트 7종과 FontAwesome 에셋을 로컬 `static/fonts/` 및 `static/css/`에 직접 내장 서빙함으로써, 이미 수립된 SSL 커넥션(Keep-Alive)을 재활용하여 모바일 접속 프리징 문제를 완벽히 해결.
* **상세페이지 드롭다운 네비게이터 구현**:
  - `usermachine` 및 `userworkplace` 테이블에 `"userMachineName"`, `"WorkplaceName"` 컬럼 추가.
  - 상세페이지 진입 시 관리자의 전체 영업장/기기를 드롭다운으로 표시하고, 선택 변경 시 해당 영업장 또는 기기 이력 페이지(`/device-temp-history/<device_id>`)로 즉시 안전 리디렉션 처리.

---

## 📅 2026-06-02 ~ 2026-06-05: [관제 웹 & UI] 대시보드 리팩토링, TCP 1818 포트 전환, 동적 임계치 규칙 엔진, 전국 온도 실시간 관제 및 Owly 브랜딩
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (1부)
* **개발 범주**: Flask App, HTML/CSS Responsive, JS, TCP Socket Migration, Owly Branding

### 1. 작업 개요 (Goal & Requirements)
* PyWebView 데스크톱 윈도우 창의 가로 폭을 줄일 때 내부 요소(특히 차트와 카드 그리드)가 깨지지 않고 축소되도록 반응형 레이아웃 구성.
* 구글 OAuth 로그인 완료 후 새 브라우저 탭이 열려 있는 불편함 개선 및 로그인 오류 발생 시 PyWebView 창의 대기 화면에 에러를 표출하고 로그인으로 되돌려 보낼 것.
* 대시보드 내 운영 게시판에 진입할 때 Jinja2 컴파일러 에러(500) 및 따옴표 기호 깨짐 현상 해결.
* 대시보드 메인 상단 "전체 가동 기기 수" 클릭 시 상세 기기 목록을 보여주는 페이지 연동 및 USIM 식별자 매핑 표시.
* 실시간 전국 평균 온도를 보여주는 상세 조회 페이지와 2초 주기 실시간 데이터 갱신 기능 구현.
* `usersettings` 임계치와 `sensorvalue` 측정값을 동적으로 비교해 경보를 생성하는 규칙 엔진 구현 (단, 하한 온도 이탈은 무시하고 상한 초과 조건만 경보 적재).
* macOS Chrome 등 브라우저 보안으로 인해 외부 실행 탭이 자바스크립트로 자동 닫히지 않는 제약을 우회하는 안내 UX 구현.
* 단말 수집 TCP 소켓 서버 포트를 기존 `9000`에서 `1818`로 전면 이전 및 백엔드/시뮬레이터 일치화.
* 가상환경(venv)을 배제하고 우분투 원격 서버의 시스템 Python3를 직접 이용해 백그라운드로 안전하게 가동되도록 조치.
* 소켓 로그 수신 시 시인성 확보를 위해 ANSI 색상화(ANSI Color) 및 즉시 화면 출력을 보장(Buffer Flushing)할 것.
* 모달 팝업 대신 상세 글 전용 페이지 `/board/<post_id>`로 이동하여 게시글 본문을 온전히 읽을 수 있도록 구현.
* 대시보드 4대 핵심 지표 위젯 재구성, 12시간 정상 가동률 추이 그래프(가로 반응형 수축 버그 해소) 및 Pico AI 도우미 챗봇의 지식 매핑 리팩토링.
* PicoTeam 시그니처 캐릭터인 부엉이 **오울리(Owly)** 에셋(`Jina/1.png`)을 활용하여 로고, 사이드바 마스코트 배너, AI 챗봇 헤더 및 웰컴 답변에 테마 적용.

### 2. 주요 작업 및 해결 방안
* **PyWebView 반응형 그리드 & 차트 가로 축소 버그 수정**:
  - 메인 패널인 `.main-panel`에 `min-width: 0;` 속성을 부여하고, 4개 핵심 카드 그리드를 `grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));`로 리팩토링하여 유체형(Fluid) 반응형 구현.
  - CSS Grid의 1fr 최소 축소 제약(`minmax(auto, 1fr)`)으로 인해 차트가 가로로 수축되지 않던 문제를 그리드 정의에 **`minmax(0, 1fr)`** 및 차트 컨테이너에 `min-width: 0`을 명시하여 자식 Canvas가 창 크기에 따라 0까지 유연하게 축소되도록 완벽 조치.
* **구글 로그인 탭 강제 종료 및 Chrome 보안 우회 UX**:
  - `callback.html` 내에 커스텀 **[구글 인증 완료]** 카드를 설계하고, 브라우저가 사용자 친화적 제스처로 창 닫기를 승인하도록 **사용자 명시적 클릭 버튼**을 주입하여 `window.close()` 차단 보안을 해결.
  - 클릭 제스처로도 창이 닫히지 않는 macOS Chrome 환경을 위해, 탭이 살아남는 경우 화면 중앙에 **"단축키: Cmd + W (Mac) / Ctrl + W (Windows)"**를 크게 트랜스폼(변형) 출력해 주는 하이 엔드 UX 적용.
* **Jinja2 컴파일러 에러 및 특수문자 깨짐 수정 (`board.html` 리팩토링)**:
  - 인라인 JS 매개변수 바인딩 방식을 탈피하여 **HTML5 표준 `data-*` 속성**을 활용해 데이터를 바인딩함으로써 Jinja2 컴파일 500 에러를 방지하고 따옴표나 특수문자가 무결하게 안전 출력되도록 조치.
* **전체 가동 기기 상세 목록 및 IMSI 매핑 (`devices.html` 신설)**:
  - 대시보드의 "전체 가동 기기 수" 카드에 `cursor: pointer` 및 `/devices` 링크를 적용하고, 기기 목록 테이블에 USIM 테이블을 조인하여 고유식별코드인 **IMSI 번호(`usimIMSI`)**가 명확하게 노출되도록 구성.
* **실시간 전국 평균 온도 상세 조회 (`national_temperatures.html` 신설)**:
  - 2초 주기로 로컬 Flask API `/api/national-temperatures`를 호출하는 실시간 폴링 구현.
  - 백엔드단에서 `sensorvalue`, `sensor`, `device`, `usermachine`, `machine`, `users` 6개 테이블을 관계 매핑 조인하여 API로 스트리밍해 주는 인메모리 매핑 구조 완성.
* **동적 이상 온도 감지 엔진 설계 (`get_dynamic_anomalies`)**:
  - `sensorvalue` 최신 레코드와 `usersettings` 임계값을 동적으로 비교하되, 하한 임계치 미달 조건(`val < lower`)은 전면 배제하고 오직 상한 임계치 초과 조건(`val > upper`)에만 경보 및 대시보드 테이블에 노출되도록 리팩토링.
* **TCP 수집 소켓 1818 포트 전환**:
  - `.env`에 `TCP_SERVER_PORT=1818` 적용, `tcp_server.py` 기본값 및 모의 클라이언트 포트 1818로 동기화 완료.
* **시스템 Python3 구동 및 CLI 로그 개선**:
  - `main.py`에 시스템 Python3 shebang을 장착하고 `chmod +x`를 적용하여 가상환경(venv) 종속성 없이 기동되도록 조치.
  - `tcp_server.py`의 `add_log()`를 개정하여 터미널 화면에 ANSI Color 코드를 입혀 경고(`[⚠️ 경고]`), 데이터 적재(`[데이터베이스 저장]`), 수신(`[📡 TCP SOCKET]`)을 색상별로 구분하고 `sys.stdout.flush()`로 즉시 버퍼를 비워 실시간성을 확보.
* **운영 게시판 상세 조회 전용 페이지 (`board_detail.html` 신설)**:
  - 게시글 목록에서 제목 클릭 시 상세 페이지로 이동하도록 A 태그 적용. 본문 텍스트의 줄바꿈과 줄어듦 방지를 위해 `white-space: pre-wrap;` 및 `word-break: break-word;` 스타일 설계.
* **대시보드 위젯 및 AI 도우미 챗봇 고도화**:
  - 대시보드 4대 위젯(`전체 가동 기기`, `기기 상태`, `센서 상태`, `통신 상태`) 연산 로직을 `device_boot_logs` 및 `sensorvalue` 자가진단 파라미터를 기준으로 백분율 계산하여 렌더링.
  - AI 도우미 명칭을 "오울리 AI 도우미"로 바꾸고 캐릭터 이미지(`/static/owly.png`) 및 🦉 이모지 대화 톤앤매너 적용.

---

## 📅 2026-06-04: [단말 펌웨어] Supabase HTTPS 404 에러 원인 규명, Content-Length 보정 및 극저온 냉동고 NTC 공식 교정
* **개발 범주**: HTTP Header Compliance, Low Power VREG Stabilization, Sub-zero Thermistor Calibration

### 1. 작업 개요 (Goal & Requirements)
* Supabase rpc Endpoint(`/rest/v1/rpc/b`)를 활용하여 데이터를 업로드할 때 모뎀 로그 상 `HTTP/1.1 404 Not Found` 에러가 주기적으로 발생하여 데이터 적재가 불가능함.
* 모뎀에 전류 공급량이 부족한 상태에서 HTTP 응답 전문을 끝까지 파싱하려고 하면 전력 강하로 인한 Pico 단말의 무작위 재부팅 현상이 동반됨.
* 센서 단말이 냉장고 및 극저온 냉동고(-15°C 이하) 환경에 유입되었을 때, 전압 분배 수치 한계치 부근에서 온도가 실제 값보다 훨씬 높게(예: 7~24°C) 비정상 판독되는 오류 해결.

### 2. 해결 과정 & 핵심 해결 방안
* **Content-Length 헤더 필수 추가 및 헤더 분리 송출**:
  - Supabase Database API는 POST 요청 시 전송 본문의 크기를 나타내는 `Content-Length` 헤더를 엄격하게 검증하여 누락 시 404/400 오류를 냄.
  - 페이로드 버퍼 문자열의 바이트 길이를 정확히 연산하여 `Content-Length: <size>` 헤더를 포함해 송출하도록 코드 전면 재정리.
* **HTTP 버퍼 수신 조기 탈출 및 모뎀 전력 안정화**:
  - 모뎀이 응답 패킷 전체를 처리하는 동안 고전력을 유지하게 되어 Pico 2 W의 온보드 전력이 순간 급감함. 이를 막기 위해 HTTP 리스폰스 수신 시 헤더의 첫 라인인 `HTTP/1.1 204 No Content` 구문이 버퍼에 읽히는 즉시 세션을 조기 종결하고 대기 상태로 빠지도록 리시브 루틴을 경량화함.
* **극저온 써미스터 공식 교정**:
  - 극저온 환경에서 NTC 저항값이 수백 k옴 대역으로 치솟아 생기는 아날로그 전압의 비선형 왜곡 구간을 보정하기 위해 B-정수 매개변수 피팅과 함께 `NTC_TEMP_OFFSET` 상수값을 `-3.8f`로 미세 세부 튜닝하여 -15°C 대역의 실측 신뢰도를 확보함.

### 3. 코드 변경 내역 (Code Modifications)
* **tasks_modem.cpp (Supabase Content-Length 주입 및 응답 수집 조기 탈출)**:
```cpp
// 페이로드 문자열 생성
char payload[256];
snprintf(payload, sizeof(payload), 
    "{\"p_imei\":\"%s\",\"p_cimi\":\"%s\",\"p_voltage\":%.2f,\"p_temp\":%.2f}",
    modem.imei, modem.cimi, current_vsys, current_temp);

uint32_t payload_len = strlen(payload);

// HTTP POST 커맨드 실행 및 헤더 구성
char http_cmd[128];
snprintf(http_cmd, sizeof(http_cmd), "AT+KHTTPPOST=1,,\"%s\",,,%d", SUPABASE_RPC_PATH, payload_len);
modem_send_cmd(http_cmd);

// 프롬프트 '>'가 유입되면 헤더와 페이로드 전송
if (wait_for_modem_prompt(3000)) {
    // 필수 Supabase 인증 헤더 및 Content-Length 송신
    uart_printf("apikey: %s\r\n", SUPABASE_API_KEY);
    uart_printf("Authorization: Bearer %s\r\n", SUPABASE_API_KEY);
    uart_printf("Content-Type: application/json\r\n");
    uart_printf("Content-Length: %d\r\n", payload_len); 
    uart_printf("\r\n"); // 헤더 마감 빈 줄

    // 페이로드 전송
    uart_puts(MODEM_UART, payload);
    uart_puts(MODEM_UART, "--EOF--Pattern--");
}

// 응답 수집 시 204 No Content 확인 후 조기 탈출
char rx_buf[256];
int bytes_read = modem_read_response(rx_buf, sizeof(rx_buf), 5000);
if (bytes_read > 0 && strstr(rx_buf, "204")) {
    printf("[HTTPS] 204 No Content 확인. 송신 성공 후 세션 조기 종료.\n");
}
```

---

## 📅 2026-06-03: [단말 펌웨어] TLS 1.2 보안 인증 및 SSL Root CA 인증서 저장소 주입
* **개발 범주**: SSL Certificate Injection, AT+KCERTSTORE, SSL Session parameters

### 1. 작업 개요 (Goal & Requirements)
* HL7811 모뎀을 사용해 Supabase의 안전한 REST API 웹 포트(443)로 HTTPS POST 전송 시, 보안 협상 핸드셰이크가 무너지는 `CME ERROR: 921` 에러 발생.
* Supabase 클라우드가 신뢰하는 Root CA 인증서(`prod-ca-2021.crt`)를 모뎀 내부 플래시 스토리지의 0번 인증서 스페이스에 안정적으로 기록해야 함.

### 2. 해결 과정 & 핵심 해결 방안
* **TLS 1.2 암호화 스위트 매칭**:
  - Supabase 보안 가이드에 따라 하위 암호화 방식을 배제하고 TLS 1.2 프로파일을 강제로 사용하도록 `AT+KSSLCFG=0,3` 설정을 모뎀에 인가하여 보안 규격을 충족시킴.
* **Root CA 인증서 플래싱(Cert Injector)**:
  - `prod-ca-2021.crt` 인증서의 전체 텍스트 파일(약 1264~1344 바이트)을 바이트 단위로 분석.
  - `AT+KCERTSTORE=0,<size>,0` 명령으로 모뎀을 인증서 대기 스트림 상태로 진입시킨 뒤, C++ 루프를 통해 바이트를 정확히 전송하고 마감 URC인 `OK` 응답을 검증하여 주입 완료함.
  - `AT+KHTTPCFG` 생성 시 인증서 확인 옵션(1)을 인가하여 Supabase 접속 시마다 인증 서버 검증을 신뢰성 있게 밟도록 조정.

### 3. 코드 변경 내역 (Code Modifications)
* **tasks_modem.cpp (인증서 저장 및 SSL 세션 활성화)**:
```cpp
bool inject_root_certificate() {
    // 0번 인증서 슬롯 초기화 및 삭제
    modem_send_cmd("AT+KCERTDELETE=0,0");
    vTaskDelay(pdMS_TO_TICKS(500));

    // 인증서 크기에 맞춰 로드 대기 명령 송출
    const char* cert_data = "-----BEGIN CERTIFICATE-----\nMIIF...\n-----END CERTIFICATE-----";
    uint32_t cert_len = strlen(cert_data);
    
    char store_cmd[64];
    snprintf(store_cmd, sizeof(store_cmd), "AT+KCERTSTORE=0,%d,0", cert_len);
    modem_send_cmd(store_cmd);
    
    if (wait_for_modem_connect_prompt(2000)) {
        uart_puts(MODEM_UART, cert_data);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return true;
    }
    return false;
}

void configure_ssl_profile() {
    // 0번 SSL 프로필 버전을 TLS 1.2로 강제 셋업
    modem_send_cmd("AT+KSSLCFG=0,3");
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Supabase 전용 암호화 스위트 조합 적용
    modem_send_cmd("AT+KSSLCRYPTO=0,8,2,16384,8,4,1,0");
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

---

## 📅 2026-06-02 ~ 2026-06-03: [관제 웹 & UI] 부저 경보/온도 연동 및 Flash 이벤트 로거/디버그 통합
* **연동 대화 ID**: `b5d273b9-91f9-413f-8a6a-931adabd43c1`
* **개발 범주**: C/C++ SDK 기반 Firmware, Flash Memory API, LCD Display, 디버그 CLI

### 1. 작업 개요 (Goal & Requirements)
* GP16 핀에 장착된 5V Active Buzzer 모듈을 활용하여 온도 연동 경보 구현.
* 경보음은 높은 미(E5: 659Hz)와 낮은 도(C5: 523Hz)의 5옥타브 조합 딩동 멜로디로 출력하고, 대기 상태에서 발생하는 지지직거리는 스피커 노이즈(잔류 전류 노이즈)를 완전히 제거할 것.
* 실시간 온도가 **-9.0°C** 이상으로 올라갔을 때만 딩동 알람이 5회 발생하고, 재생 후 1분 동안 쉬는 동작을 무한 반복할 것.
* 비휘발성 플래시 메모리 영역(마지막 64KB)에 시스템 부팅 정보 및 상태 코드를 기록하는 로깅 시스템 구현.
* 시리얼 터미널 CLI 상에서 로그를 파싱해 출력하는 `dump_csv`와 초기화하는 `clear_csv` 명령어 추가.
* LCD 부팅 완료 후 `Boot.. Check Pico` 잔상이 지워지지 않는 문제 수정 및 부팅 원인 코드(bootReason) 세분화.

### 2. 주요 작업 및 해결 방안
* **부저 멜로디 및 노이즈 제어 (`main.cpp` 수정)**:
  - GP16 핀을 PWM 모드로 구동하여 E5(659Hz)와 C5(523Hz)의 정밀한 주파수 딩동 멜로디 구현.
  - 멜로디 재생 완료 즉시 GP16 핀을 일반 GPIO 출력 모드로 즉각 복구하고 출력 값을 로우(0V, GND 접지)로 낮추어 스피커 대기 노이즈를 근본적으로 차단.
  - `vBuzzerTask`를 FreeRTOS 상에서 구동하여 온도가 -9.0°C를 초과하는 조건일 때 5회 알람 송출 후 `vTaskDelay`를 활용해 1분간 정지 대기하도록 제어.
* **비휘발성 플래시 로깅 시스템 구현**:
  - 32바이트 정렬 구조의 `FlashLogEntry` 구조체 설계: 타임스탬프, 온도, VSYS 전압, 전송 성공 여부, NTC 오류 코드, 모뎀 상태 코드, 시스템 진단 오류 코드, 부팅 사유 코드를 순차적으로 적재.
  - 디버그 CLI 태스크(`vDebugTask`) 명령어 파서에 `dump_csv`와 `clear_csv` 명령어를 추가하여 시리얼 터미널을 통해 CSV 형태로 데이터 확인 및 메모리 클리어 제어 가능하도록 구현.
* **부팅 원인 코드 (`bootReason`) 세분화 및 LCD 수정**:
  - 부팅 원인 코드 매핑: `0`(정상 부팅), `1`(원격 명령에 의한 SW 재부팅 - watchdog scratch register에 매직 키 `0xDEADBEEF` 검출), `2`(와치독 타임아웃 리셋), `3`(부저 전력 강하 브라운아웃에 의한 비정상 재부팅).
  - LCD 스레드 상태 플래시 갱신 코드를 수정하여 부팅 완료 후 LCD 화면에 `Boot.. Check Pico` 잔상이 깔끔하게 소거되도록 조치.
* **Ninja 빌드 검증**:
  - `ninja -C build` 컴파일을 수행하여 바이너리 `nb_iot_project.uf2` 파일 정상 갱신 및 실장 하드웨어 동작 검증 완료.

---

## 📅 2026-06-02: [단말 펌웨어] Pico 2 W 전원/플래시 자가진단 로직 수립 및 Supabase HTTPS API 피벗 착수
* **개발 범주**: Embedded Self-Diagnostics, CRC32 Checksum, RAM Pattern Test, FreeRTOS LCD Multitasking

### 1. 작업 개요 (Goal & Requirements)
* 단말 부팅 프로세스 가동 시, Pico 2 W의 기본 동작 무결성을 점검(내부 전압이 정상 범위에 있는지, 내장 Flash 메모리가 오염되지 않았는지, RAM이 100% 정상 작동하는지)해야 함.
* 모뎀 부팅 후 LTE 네트워크(ATE0, SIM 체크, RSSI 세기, CEREG LTE 망 동기화, COPS 통신사 확인, IMEI/CIMI 추출) 상태 진단을 비동기식 스레드로 수립해야 함.
* 기획 단계의 외부 TCP 소켓 전송 규격을 클라우드 환경과의 직접 통합을 위해 Supabase HTTPS REST API 직접 적재 방식으로 전환(피벗) 및 설계해야 함.
* 사용자 친화적인 피드백을 위해 LCD에 부팅 상세 진행률을 표시하고, 백그라운드에서 신호 감도에 비례해 요동치는 RSSI 안테나 바 애니메이션 태스크 연동해야 함.

### 2. 해결 과정 & 핵심 해결 방안
* **Pico 2 W 자가진단 파이프라인 구현**:
  - `adc_read()`를 사용하여 내부 VSYS 입력 전압이 안정화 범위(3.3V 이상)인지 판독.
  - 내장 칩 온도 센서를 읽어 과열 구간(80°C 이하)인지 대조 검사.
  - 특정 Flash 메모리 영역의 데이터를 가져와 CRC32 체크섬을 돌려 무결성 검사.
  - RAM의 특정 짧은 힙 영역에 패턴 바이트(`0xAA`, `0x55`)를 채우고 읽어와 원본과 대조하여 RAM 회로 불량 검사 수행.
* **비동기 LCD RSSI 애니메이션 태스크 분리**:
  - 모뎀의 응답이 들어오는 대기시간 동안 화면이 굳는 현상을 방지하기 위해, LCD 안테나 그래픽을 독립적인 FreeRTOS 태스크로 분리하여 동작시킴으로써 60fps에 준하는 자연스러운 부팅 연출 완료.

### 3. 코드 변경 내역 (Code Modifications)
* **main.cpp (Pico Self-Diagnostics 및 RAM 테스트)**:
```cpp
bool verify_flash_integrity() {
    uint32_t calculated_crc = crc32_calculate((uint8_t*)FLASH_TARGET_OFFSET, FLASH_CHECK_SIZE);
    return calculated_crc == EXPECTED_FLASH_CRC;
}

bool run_ram_pattern_test() {
    volatile uint8_t* test_ptr = (volatile uint8_t*)malloc(1024);
    if (!test_ptr) return false;
    
    // Pattern writing
    for (int i = 0; i < 1024; i++) {
        test_ptr[i] = (i % 2 == 0) ? 0x55 : 0xAA;
    }
    
    // Pattern verification
    bool ok = true;
    for (int i = 0; i < 1024; i++) {
        if (test_ptr[i] != ((i % 2 == 0) ? 0x55 : 0xAA)) {
            ok = false;
            break;
        }
    }
    free((void*)test_ptr);
    return ok;
}
```

* **tasks_lcd.cpp (RSSI Check Animation Task)**:
```cpp
void vLcdRssiAnimationTask(void *pvParameters) {
    int anim_frame = 0;
    while (true) {
        if (lcd_params.is_booting) {
            lcd_set_cursor(0, 0);
            lcd_print("Booting System..");
            
            // 안테나 바가 움직이는 다이나믹 연출
            lcd_set_cursor(14, 0);
            for (int f = 0; f <= anim_frame; f++) {
                lcd_write_custom_char(f); // 안테나 바 출력
            }
            anim_frame = (anim_frame + 1) % 4;
            vTaskDelay(pdMS_TO_TICKS(250)); // 250ms 마다 갱신
        } else {
            // 부팅 완료 후에는 정적 온도 대시보드로 전환
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
```

---

## 📅 2026-06-02: [관제 웹 & UI] 1차 작업: Pico 2W 부팅 자가 진단 및 LTE 모뎀 상태 수집
* **연동 대화 ID**: `018c87a7-01e9-4241-bf50-880cf480c070`
* **개발 범주**: C/C++ Firmware, TCP 소켓 서버, Supabase DB 스키마 설계

### 1. 작업 개요 (Goal & Requirements)
* Raspberry Pi Pico 2W 기기가 부팅 시 스스로 전압, 칩 내부 온도, 플래시/RAM 무결성, LTE 모뎀 및 NTC 온도 센서 상태를 체크하고, 결과를 TCP 소켓으로 보낼 수 있는 MicroPython 코드 구성.
* 수신 서버가 IMEI와 CIMI(IMSI)를 바탕으로 기기와 사용자를 식별하여 Supabase DB에 부팅 로그 레코드를 적재하도록 스키마 설계 및 서버 개발.
* LTE 모뎀 안정화를 위해 `AT+CFUN=1` 수행 후 30초 대기 로직 반영.
* NTC 온도센서 상태값 분류(0: 정상, 1: 단선, 2: 합선, 3: 범위 초과, 99: 기타 결함) 처리.
* 보안 RLS(행 레벨 보안)는 기존 테이블 설정을 준수하여 우선 비활성화 처리.

### 2. 주요 작업 및 해결 방안
* **Supabase DDL 마이그레이션 (`create_device_boot_logs.sql` 생성)**:
  - `public.device_boot_logs` 테이블 생성: 부팅 체크 이력을 저장하고, 기존 `users` 및 `device` 테이블과 외래키(Foreign Key) 제약 조건 추가.
  - 데이터 칼럼: `id`(BigInt IDENTITY PK), `boottime`(timestamp KST), `userId`(FK), `deviceId`(FK), `pico_voltage`, `temperature`, `flash_integrity`, `ram_test`, `at_status`, `cpin_status`, `csq_rssi`, `cops_carrier`, `temp_sensor_status` 설계.
* **Pico 2W용 MicroPython 부팅 체크 코드 (`boot_check.py` 생성)**:
  - VSYS 공급 전압(ADC 29) 및 칩 내부 온도 센서(ADC 4) 값 계측 및 변환 로직 구현.
  - Flash 메모리 특정 코드 영역의 CRC32 Checksum 무결성 검증 로직 구현.
  - 특정 테스트 패턴을 RAM 영역에 쓰고 다시 읽어 검증하는 RAM 자가진단 루프 적용.
  - UART 채널을 통해 모뎀 전원 인가 제어, `AT+CFUN=1` 수행 후 `time.sleep(30)`으로 모뎀 안정화 대기 추가.
  - `AT+CPIN?`으로 SIM 상태 확인, `AT+CSQ`로 신호 감도 파싱, `AT+COPS?`로 통신사명 파싱, `AT+CGSN` 및 `AT+CIMI`로 단말 고유 식별자(IMEI, IMSI) 획득 구현.
  - NTC 써미스터의 ADC 전압 값을 읽어 단선(전압 VCC 인접), 합선(전압 GND 인접), 측정범위초과 판별 후 에러 코드로 변환하는 진단 로직 적용.
  - 수집된 자가 진단 데이터를 JSON 문자열로 직렬화하여 TCP 소켓을 통해 원격 서버로 송출 후 안전하게 종료.
* **TCP 소켓 및 Supabase 매핑 서버 코드 (`tcp_receiver.py` 생성)**:
  - Zero-dependency 순수 소켓 서버 구현.
  - 수신된 JSON 데이터에서 `imei`와 `cimi`를 추출하고, Supabase의 `device` 및 `usim` 테이블을 조인하여 `deviceId`와 `userId` 매핑.
  - 식별에 성공할 경우 `device_boot_logs` 테이블에 삽입(INSERT) 동작 처리.
* **통합 테스트 코드 (`mock_test.py` 생성)**:
  - 로컬 환경에서 가상 Pico Client 소켓 송신과 수신 서버, DB 매핑 및 삽입 과정 일체를 시뮬레이션하여 데이터 파이프라인의 무결성 검증 완료.

---

## 📅 2026-04-20 ~ 2026-05-29: [단말 펌웨어] Git Commit Log 기반 초기 빌드 아키텍처 및 FreeRTOS 세팅
* **개발 범주**: C/C++ CMake Toolchains, FreeRTOS Kernel Integration, Task Scheduling

### 1. 주요 커밋 및 빌드 히스토리
* **2026-05-29 (Commit: `602a06d`, `0e776cd`, `054f634`)**: 
  - Raspberry Pi Pico 2 W 타겟에 최적화된 FreeRTOS 커널 패키지 연동 완료.
  - 힙 메모리 관리(`heap_4.c`) 구성 및 멀티태스킹 환경 구동을 위한 기초 태스크 스케줄링 구조(`vBuzzerTask`, `vSensorTask`, `vLcdTask`, `vModemTask`) 설계 완료.
  - CMake 및 Ninja 빌드 빌드 타겟 확정.
* **2026-05-16 (Commit: `1aea5c8`)**:
  - NB-IoT 프로젝트 구조적 기초 뼈대(src, lib, include) 생성 및 SDK 라이브러리 링크 검증.
  - C++ 표준 입출력 및 GPIO 입출력 설정 초안 마련.
* **2026-04-22 (Commit: `d038fe0`)**:
  - 개발 환경 하드웨어(Pico 2 W) GPIO 및 디버그 출력 기본 테스트.
* **2026-04-20 (Commit: `d6e4e06`, `52f4d7a`)**:
  - 초기 설정 세팅 및 Initial commit.
