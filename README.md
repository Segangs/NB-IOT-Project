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
>   * **Modem**: HL7811 셀룰러 모뎀을 제어하여 LTE-M(NB-IoT) 망을 통한 TLS 1.2 보안 규격 통신 연동.
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

## 📅 2026-07-04

### [펌웨어/MQTT] KeepAlive 안정화 및 EMQX rule 분리
* MQTT config payload 방어용 `src/lib/mqtt_payload.hpp/cpp` 추가
* `undefined`, 빈 문자열, 공백, `null`, `[]`, 잘못된 JSON config payload 무시 처리
* config 배열/객체 payload의 필드 누락 및 null 값 수신 시 기존 설정 유지
* boot/periodic config 수신 처리를 `apply_mqtt_config_payload()`로 통합
* telemetry payload `[userSensorId, temperature]` 생성 전 sensor id 및 온도값 검증
* MQTT 상태 enum 및 `modem_MqttPoll()` 추가
* `vPeriodicModemTask`를 유지형 MQTT 세션, 60초 config request, reconnect backoff 구조로 조정
* boot task에서 `devices/<imei>/config/request` 별도 발행
* `emqx_setup.sh`의 telemetry/boot config republish 제거 및 `devices/+/config/request` rule 분리
* `emqx_setup.sh` 하드코딩 API 인증 헤더 제거 및 `.env`의 `EMQX_API_AUTH_HEADER` 사용
* `tests/mqtt_payload_test.cpp` 추가
* 호스트 단위 테스트, `emqx_setup.sh` 문법 검사, fresh CMake 펌웨어 빌드 및 UF2 생성 검증 완료

### [펌웨어] DS18B20 온도 보정 오프셋 추가
* `config.h`에 `TEMP1_CAL_OFFSET_C`, `TEMP2_CAL_OFFSET_C` 항목 추가
* TMP1/TMP2 기본 보정값 `0.0f` 설정
* DS18B20 정상 측정값에만 센서별 보정 오프셋 적용
* CMake 구성, 펌웨어 빌드, UF2 생성 검증 완료

## 📅 2026-07-03

### [펌웨어] DS18B20 디지털 온도센서 전환
* GP22 임시 DS18B20 실물 연결 기준 외부 온도센서 입력을 1-Wire 디지털 방식으로 전환
* 기존 아날로그 서미스터 ADC 측정, 저항 계산, B-parameter 변환, 보정 오프셋 코드 제거
* 온도센서 핀 정의를 `TEMP1_SENSOR_PIN=22`, `TEMP2_SENSOR_PIN=26`으로 정리
* TMP1은 GP22, TMP2는 GP26 DS18B20 데이터 라인 기준으로 부팅 체크와 주기 샘플링 수행
* DS18B20 reset, convert T, scratchpad read, CRC8 검증, 온도 범위 검증 루틴 추가
* VSYS 전압 및 내부 칩 온도 진단용 ADC 경로는 유지
* flash log CSV의 `NtcStatus` 명칭을 `TempStatus`로 정리
* DS18B20 읽기를 부팅 필수 경로에서 제거하고 센서 태스크 전용 읽기 구조로 조정
* 1-Wire 버스 접근 mutex 추가 및 부팅 초기 온도 상태 기본값을 미연결 상태로 초기화
* 부팅 먹통 원인 격리를 위해 `ENABLE_DS18B20_READ=0` 임시 스위치 추가 및 DS18B20 GPIO 미접근 빌드 생성
* DS18B20 GPIO 미접근 빌드의 정상 부팅 확인 결과를 기준으로 `ENABLE_DS18B20_READ=1`, `ENABLE_TEMP1_DS18B20=1`, `ENABLE_TEMP2_DS18B20=0` 구성 전환
* GP22 TMP1만 부팅 완료 30초 후부터 지연 읽기 수행하도록 조정
* 부팅 완료 직후 멈춤 증상 확인에 따라 DS18B20 1-Wire bit timing 루틴의 FreeRTOS critical section 제거
* GP22 presence 미검출 원인 확인용 `DS18B20_DIAG` 로그 추가
* reset 전 DATA idle 상태와 presence pulse 감지 여부를 5초 주기 진단 출력
* DS18B20 배선 오류 수정 후 GP22 TMP1 정상 온도 수신 확인
* LCD 하단 온도 표시를 `C1/C2` 순환 표시에서 TMP 연결 상태 기반 표시로 변경
* GP22 단독 연결 시 `-15.0°C`, GP26 단독 연결 시 `T2: -15.0°C`, 양쪽 연결 시 `-15.0 -15.0°C` 형식 적용
* 8002A 스피커 앰프 입력 핀을 GP6 PWM으로 변경
* 부팅 후 MQTT 발신 불안정 현상은 후속 정리 과제로 기록
* DS18B20 센서 샘플링 주기를 1초에서 10초로 변경
* CRC/일시 통신 실패 3회까지 마지막 정상 온도값 유지로 LCD `CRC` 표시 흔들림 완화
* `DIAG_CHECK`, `DS18B20_DIAG`, `DS18B20_READ_RESET_FAIL` 시리얼 로그 제거
* CMake 구성, 펌웨어 빌드, UF2 생성 검증 완료

### [DB/펌웨어/대시보드] 고정 USER_SENSOR 구조 전환
* Supabase `SENSOR_CTGY`, `USER_SENSOR` 테이블 생성 및 device 1~5 기준 센서 20행 초기 적재
* 기존 `sensor`, `usersettings` 테이블 제거 및 설정온도 저장 위치를 `USER_SENSOR`로 통합
* `sensorvalue`, boot log RPC, telemetry RPC, config RPC를 고정 `userSensorId` 구조에 맞춰 재구성
* EMQX telemetry/boot/config rule 및 HTTP action body를 IMEI + `userSensorId` 기반으로 갱신
* 펌웨어 센서 ID를 TMP1=1, TMP2=2, MIC1=3, MIC2=4 고정 매핑으로 변경
* 부팅 로그의 센서 회로 상태를 TMP1, TMP2, MIC1, MIC2로 분리 전송
* MQTT config 수신 시 TMP1/TMP2 설정 상한값을 Pico 내부에 채널별 저장
* 대시보드 `/device-status`, 온도 상태, 온도 이력, `/api/status`를 `USER_SENSOR` 기준으로 전환
* 운영 서버 `/home/segang/project`에 대시보드 변경 반영 및 Flask 프로세스 재시작
* Supabase RPC 검증, EMQX 운영 설정 검증, Python 문법 검사, CMake 펌웨어 빌드 및 UF2 생성 검증 완료

### [펌웨어] HL7811 AT 명령 목차 확인 및 MQTT 코드 분리
* `DOCS/RM78-1 데이터시트/HL78xx - AT Commands Interface Guide - Rev16.0.pdf` 17-26p 목차 확인
* HL7811 관련 명령 범위 확인: V25ter/General, ME Control/Status, Packet Domain, Protocol Common/SSL, MQTT AT Commands
* HTTP Client(`KHTTP*`) 및 raw TCP socket(`KTCP*`) 기반 펌웨어 코드 제거
* 부팅 중 잔존 HTTP 세션 정리 루프 제거
* `tasks_modem.cpp`를 모뎀 전원, UART, AT 응답, SIM/망 상태, 인증서, 시간 조회 중심으로 정리
* MQTT 세션/발신/구독/종료 구현을 `src/tasks/tasks_mqtt.cpp`로 분리
* MQTT 연결 상태 플래그를 `is_socket_open`에서 `is_mqtt_connected`로 정리
* `CMakeLists.txt`에 `src/tasks/tasks_mqtt.cpp` 빌드 대상 추가
* CMake 구성, 펌웨어 빌드, UF2 생성 검증 완료

### [펌웨어] MQTT CONNECT FAIL 세션 초기화 보강
* 부팅 로그 기준 `AT+KMQTTCFG` 직후 `+CME ERROR: 0` 및 `MQTT_CFG_FAIL` 발생 원인 추적
* 잦은 재부팅 후 모뎀 내부 MQTT 세션 잔존 가능성 기준 `KMQTTCFG` 전 세션 초기화 절차 추가
* `modem_MqttOpen()` 시작 시 `AT+KMQTTCLOSE=1..6`, `AT+KMQTTDEL=1..6` 순차 수행
* 세션 초기화 후 `mqtt_session_id=0`, `is_mqtt_connected=false` 상태 재설정
* `KMQTTCFG` 1차 실패 시 `MQTT_CFG_RETRY` 로그 후 세션 초기화 재수행 및 1회 재시도
* `tasks_boot.cpp`의 중복 `MQTT_CONNECT`, `MQTT_CONNECT_OK` 로그 제거
* CMake 구성, 펌웨어 빌드, UF2 생성 검증 완료

### [펌웨어] RTOS 로그 출력 큐 및 단일 LogTask 전환
* 펌웨어 프로젝트 소스의 직접 `printf` 호출을 `LOG()` 매크로 기반 호출로 전환
* `src/lib/log.cpp/hpp` 추가 및 FreeRTOS 정적 큐 기반 로그 버퍼 구성
* `LOG()` 호출부는 문자열 포맷 후 큐 적재만 수행하고 실제 USB stdio 출력은 `vLogTask`에서만 수행
* `main.cpp`에 `vLogTask` 등록 및 우선순위 0 최저 우선순위 적용
* LTE/모뎀 통신 태스크의 로그도 직접 출력 대신 큐 적재 방식으로 통일
* SSL Root CA 인증서 주입 구간에서 로그 mute 처리 후 성공/실패 결과만 사후 출력
* `CMakeLists.txt`에 `src/lib/log.cpp` 빌드 대상 추가
* FreeRTOS heap `192KB`, minimal stack `384`, timer task stack `1536`으로 확장
* LCD, Boot, Sensor, LED, Debug, PeriodicModem, Buzzer, Log task 스택 여유 확대
* 부팅, 모뎀, 인증서, MQTT, 센서, 경보 로그를 `BOOT`, `MODEM_AT_OK`, `CERT_WRITE_OK`, `MQTT_CONNECT_OK` 등 상태 토큰 중심으로 축약
* `dump_csv` 명령의 CSV 데이터 출력은 사용자 요청 데이터 출력으로 유지

### [서버/DB] EMQX MQTT 인증 실패 추적 필드 확장
* EMQX 로그 기준 `2026-07-03 05:03:30~05:03:31` 인증 실패의 외부 IP `44.220.185.63` 및 랜덤 clientid 확인
* `authentication_logs`에 `clientid`, `peerhost`, `listener`, `username_raw`, `password_empty` 컬럼 추가
* `auth_device()` RPC에 EMQX 접속 문맥 인자 확장 및 기존 2인자 호출 호환 유지
* 빈 MQTT username 요청은 `MQTT username empty` 사유로 분리 기록
* EMQX HTTP Authentication body에 `clientid`, `peerhost`, `listener`, `username_raw` 전달 설정 반영
* EMQX HTTP Authentication 요청 헤더에 `x-emqx-auth-secret` 전용 식별 헤더 추가
* 운영 서버 `/home/segang/project/.env`에 `EMQX_AUTH_SECRET` 생성 및 운영 EMQX 인증 리소스 반영
* 운영 EMQX 인증 리소스의 헤더 키 및 body 템플릿 정상 유지 확인
* MQTT 1883/8883 정상 인증 허용 및 빈 username 인증 거부 검증
* Supabase `authentication_logs`에 검증 로그 ID 788~791 기록 확인
* 운영 서버와 로컬 `emqx_setup.sh`의 인증 body를 Supabase RPC 인자 구조에 맞춰 갱신
* 검증용 빈 username 접속 거부 및 정상 IMEI/IMSI 접속 허용 확인

### [펌웨어] USB 디버그 명령 확장 및 전원 제어 준비
* `vDebugTask` 재등록으로 USB 시리얼 로컬 명령 수신 경로 복구
* `reboot` 명령 입력 시 `safe_reboot()`와 hardware watchdog 기반 Pico 재부팅 수행
* `power off`, `poweroff`, `power_off` 명령 입력 시 GP15 LTC2954 KILL# 기반 전원 차단 요청 시퀀스 수행
* LTC2954 미연결 테스트 환경을 고려한 KILL# 2초 LOW 요청 후 생존 시 HIGH 복귀 처리
* PCB 문서 기준 RM78-1 PWRON 핀 GP4, LTC2954 INT GP14, KILL GP15 정의 분리
* `dump_csv`, `clear_csv`, `reboot`, `power off` 로컬 명령은 모뎀 busy 상태와 무관하게 우선 처리

### [펌웨어] GP28 TXON LED 엣지 기반 표시 보정
* GP28 TXON 표시 LED가 모뎀 송수신 중 깜박이지 않는 실물 증상 확인
* 50ms 폴링 방식이 짧은 TXON pulse를 놓칠 수 있는 구조 확인
* GP5 TXON 입력을 내부 pull-up 및 양방향 엣지 인터럽트 기반으로 변경
* RM78-1 매뉴얼 기준 TX_ON indicator feature 활성화를 위해 `AT+KHWIOCFG?` 조회 및 필요 시 `AT+KHWIOCFG=5,1` 설정 로직 추가
* `AT+KHWIOCFG=5,1` 변경은 모뎀 재부팅 후 적용 가능하다는 매뉴얼 주의사항 로그 반영
* GP28 LED를 평상시 ON, 펌웨어 송신 상태(`is_transmitting`) 동안 100ms 간격 fallback blink 방식으로 변경
* GP5 TXON 입력 직접 미러링 제거 및 TXON edge/monitor 시리얼 진단 로그 제거
* 부팅 중 GP8 빨강 상태 LED 점멸 간격을 500ms로 변경
* `dump_csv`, `clear_csv` 로컬 디버그 명령은 모뎀 busy 상태와 무관하게 USB stdio 입력에서 처리되도록 수정
* 일반 AT 바이패스 명령은 모뎀 busy 상태에서 전달 보류 및 안내 로그 출력

### [운영 지침] 펌웨어 검증 범위 축소
* 펌웨어 작업 후 Codex 검증 범위를 CMake 구성, 빌드, UF2 생성 확인까지로 제한
* Pico 드라이브 복사, Pico 재부팅, 시리얼 로그 읽기 작업은 사용자 별도 요청 시에만 수행
* UF2 반영 및 시리얼/실물 하드웨어 확인은 사용자 직접 수행 기준 반영

### [운영 지침] 시리얼 모니터 공유 검증 원칙 추가
* 펌웨어 실기기 테스트 중 시리얼 로그 확인 시 우측/가시 터미널 창에 시리얼 모니터를 출력하는 원칙 추가
* 사용자도 동일한 시리얼 로그 흐름을 보며 부팅, 모뎀 AT, MQTTS 연결 상태를 함께 확인하는 검증 방식 반영
* LCD, LED, 부저 등 시리얼 외 하드웨어 표시 항목은 기존처럼 사용자 실물 확인 요청 유지

## 📅 2026-07-02

### [펌웨어] MQTT 브로커 환경값 누락 및 GP28 TXON LED 극성 보정
* `.env`에 `MQTT_BROKER_HOST`, `MQTT_BROKER_PORT` 누락으로 `config.h` fallback placeholder가 펌웨어에 컴파일된 원인 확인
* 로컬 `.env`에 `MQTT_BROKER_HOST="p.zxcx.io"`, `MQTT_BROKER_PORT="8883"` 추가 및 신규 UF2 문자열 기준 `p.zxcx.io` 반영 확인
* GP28 TXON LED가 모뎀 부팅 후 꺼진 채 유지되는 현상 기준 GP5 TXON 입력을 반전 표시에서 직접 미러링으로 변경
* GP5 HIGH idle 상태에서는 GP28 LED ON, TXON LOW pulse 시 LED OFF blink 방향으로 보정

### [펌웨어] GP7 BAT MODE 표시 임시 비활성화
* GP7 전원 어댑터 감지 분압 회로 미연결 상태에서 내부 풀다운에 의해 `BAT MODE`가 표시되는 원인 확인
* 실제 PCB 회로 연결 전까지 `tasks_led.cpp`의 배터리 모드 LCD 표시 플래그를 `false`로 고정
* 회로 연결 후 `lcd_params.is_battery_mode = !adapter_present` 로직 재활성화 위치 주석 기록
* 기존 `build/` 산출물 대신 수정 후 생성된 임시 검증 UF2 기준 재플래시 수행
* LCD/LED 등 시리얼 로그로 확인되지 않는 하드웨어 표시 항목은 플래시 후 사용자 실물 확인 요청 원칙 추가

### [펌웨어] RTOS 태스크 파일 분리 및 main.cpp 정리
* `main.cpp`에 몰려 있던 RTOS 태스크를 `src/tasks/` 아래 전용 `cpp/hpp` 파일로 분리
* `tasks_led`, `tasks_periodic_modem`, `tasks_buzzer`, `tasks_sensor_reader`, `tasks_debug`, `tasks_boot` 신규 구성
* 공용 전역 상태와 JSON/리부트 헬퍼를 `app_context.cpp/hpp`로 이동하여 태스크 파일 간 의존성 정리
* `main.cpp`를 부팅 이유 판정, 초기화, FreeRTOS 태스크 등록, RTOS 훅 중심으로 축소
* `CMakeLists.txt`에 신규 태스크 소스 파일 연결 및 깨끗한 임시 빌드 검증

### [대시보드] 웹폰트 로딩 안정화 변경 서버 반영
* `/home/segang/project` 원격 서버에 `app.py`, `templates/layout.html`, `templates/dashboard.html` 반영
* 반영 전 원격 백업 경로 생성: `/home/segang/project/deploy_backup_20260702_081055`
* Flask 대시보드 프로세스 재시작 및 `zxcx.io/dashboard` 302 응답 확인
* `/static/fonts/SUITE-Regular.woff2` 응답의 `Cache-Control: public, max-age=31536000, immutable` 헤더 확인

### [펌웨어/대시보드] RJ45 LED, 배터리 모드 표시, 웹폰트 로딩 안정화
* GP10/GP12 RJ45 온도센서 LED를 정상 샘플 수신 시 점등, 센서 샘플 수신 시 짧은 blink 처리
* GP11/GP13 마이크 LED는 I2S/Edge AI 수집 루틴 적용 전까지 미사용 OFF 유지
* GP7 전원 어댑터 감지 입력 추가 및 어댑터 분리 시 LCD 하단 `BAT MODE` 우선 표시
* 대시보드 공통 레이아웃의 SUITE 웹폰트 preload, `font-display: block`, 폰트 정적 캐시 헤더 적용
* `MQTT_BROKER_HOST` 기본값 placeholder 전환 및 `.env.example` MQTT 브로커 키 추가
* 신규 빌드 디렉터리에서도 FreeRTOS import 경로가 재현되도록 `FREERTOS_KERNEL_PATH` CMake 변수 명시

### [펌웨어] PCB 기준 상태 LED 및 TXON 표시 LED 제어 추가
* `DOCS/PCB/pico2w_rm78_sensor_pcb_design_portfolio.md` 기준 GP8 빨강 상태 LED, GP9 초록 상태 LED, GP28 TXON 표시 LED 반영
* 부팅 중 GP8 빨강 LED 1초 간격 점멸, 부팅 완료 후 GP9 초록 LED 상시 ON 구성
* RM78-1 TXON 입력 GP5를 50ms 주기로 샘플링하고 GP28 LED를 평상시 ON, TXON 감지 시 LOW로 반전 표시
* 별도 FreeRTOS `StatusLedTask` 구성 및 공유 락 미사용, 짧은 GPIO 갱신 후 `vTaskDelay` 양보 방식으로 교착상태 회피

### [운영 지침] 작업 기록 및 Git 보류 원칙 반영
* 작업 완료 후 `project_history.md` 및 `README.md` 기록 유지
* 작업 완료 시점 자동 Git commit/push 보류, 사용자 명시 요청 시에만 커밋·동기화 진행
* `README.md` 날짜별 섹션 1개 안에 여러 작업 내용 순차 누적 방식 적용

### [PCB 설계자료] SPH0645LM4H Edge AI 음향 센서 의도 반영
* SPH0645LM4H를 단순 기계음 녹음용이 아닌 Edge AI/TinyML 입력용 장비 음향 데이터 수집 채널로 재정의한 내용 반영
* 정상 운전음, 비정상 운전음, 이상 동작 예측 신호 구분을 위한 장기 음향 패턴 수집 목적 숙지
* Pico 측 원시 PCM 또는 FFT, RMS, 주파수 대역별 에너지, MFCC 유사 특징량 전처리 가능성 기록
* 3m UTP 케이블 조건을 고려한 8kHz~16kHz 기본 운용 및 필요 시 24kHz 수준 샘플링 방향 기록

### [PCB 설계자료] EasyEDA 회로도·PCB 산출물 반입 및 작업 지침 반영
* `DOCS/PCB/` 내 회로도, PCB 이미지, EasyEDA JSON, 포트폴리오 문서 전체 확인
* Pico 2 W, RM78-1 LTE-M, DS1129-04 듀얼 RJ45, DS18B20, SPH0645LM4H, LCD1602 I2C, 8002A, IP5306, MP1584EN, LTC2954CTS8-1 통합 설계 숙지
* `+5V_IP5306` 상시 전원과 `+5V_SYS` 시스템 전원 분리 구조 및 GP0~GP28 주요 GPIO 배정 기록
* RJ45 UTP Pair 기준 BCLK-GND Pair 배치, DS18B20 5.1kΩ Pull-up, I2S 47Ω 직렬 댐핑 구성 기록
* 후속 확인 항목: R6 100kΩ → 1kΩ 변경, GP7 감지 전압 확인, C4 22µF 종료 지연 의도 확인

### [Git 동기화] 미커밋 변경 정리 및 원격 동기화
* `flash_logger.cpp`의 `flash_safe_execute` 기반 듀얼코어 플래시 I/O 보호 로직 반영
* Pico stdio/flash 관련 빌드 의존성 보강, `www.zxcx.io` → `zxcx.io` 301 리다이렉트 추가
* Cloudflare Tunnel 전제 HTTP 모드 고정, DuckDNS 자동 동기화 비활성화, 생성물 ignore 규칙 정리

### [Codex 마이그레이션] Antigravity 설정/워크플로우를 Codex 프로젝트 표면으로 이전
* `DOCS/codex/mig/` 산출물 4개 검토 및 Codex 0.142.5 공식 매뉴얼 기준 적용
* 루트 `AGENTS.md`, `.codex/config.toml`, `.agents/skills/` repo-scoped skill 구성
* PicoTeam 별도 경로 미사용 및 통합 서버 경로 `/Users/segang/Documents/NB-IOT/Segang/project` 고정
* Supabase MCP OAuth 접근 검증 및 `NB_IOT` 프로젝트(`yzorfvgpmkwnjpdfyqsk`) 조회 정상 동작 확인
* Supabase RLS/SECURITY DEFINER 관련 후속 보안 작업 기록

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

## 📅 2026-06-20: [단말 펌웨어] 네트워크/서버 환경 설정 변수 .env 이전 및 CMake 동적 매크로 전역 도입
* **개발 범주**: Security Hardening, CMake Build, Environment Variables (.env)

### 1. 작업 개요 (Goal & Requirements)
* `src/config.h`에 민감한 API 인증 키(`SUPABASE_ANON_KEY`)뿐만 아니라, `APN_NAME`, `SUPABASE_HOST`, `SUPABASE_PORT` 등 모든 가변 환경 설정 변수가 하드코딩되어 깃허브 공개 저장소에 직접 노출되는 구조 개선.
* 모든 서버 연동 및 네트워크 설정 파라미터를 외부 `.env` 파일로 통합 이전하여 기밀성과 빌드 유연성을 극대화할 것.

### 2. 해결 과정 & 핵심 해결 방안
* **CMake 기반 `.env` 동적 파서 확장 (`CMakeLists.txt`)**:
  - 프로젝트 루트의 `.env` 파일에 기록된 모든 환경 변수(`SUPABASE_ANON_KEY`, `APN_NAME`, `SUPABASE_HOST`, `SUPABASE_PORT`)를 CMake 빌드 구성 단계에서 파싱하여 컴파일러 전처리 매크로(`add_compile_definitions`)로 전역 주입하도록 기능을 일원화함.
* **`config.h` 하드코딩 파라미터 완전 소거 및 매크로 가드 도입**:
  - `src/config.h` 내부의 하드코딩된 서버/APN 연결 설정을 모두 삭제하고, `#ifndef` 지시문을 결합하여 컴파일 타임에 환경 변수 값이 주입되지 않을 경우에만 대체 기본값/플레이스홀더가 삽입되도록 안전장치 수립.
* **환경 구성 템플릿 갱신 (`.env.example` 및 `.gitignore`)**:
  - `.env.example` 파일에 모든 네트워크 파라미터 구조 템플릿을 명시하였고, 실제 연결 설정값들이 담긴 `.env` 파일은 깃 커밋 대상에서 완벽히 격리시켰습니다.

---

## 📅 2026-06-20: [관제 웹 & UI] 부엉이(Owly) 캐릭터 고도화, 부팅 로그 명칭/데이터 표기 개선, 전체 대화 리스트 통합 및 삭제
* **연동 대화 ID**: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e` (2부 / 현재 대화)
* **개발 범주**: Flask App, HTML/CSS Web UI, UI Metadata Deletion, Multi-process Daemon Cleanup

### 1. 작업 개요 (Goal & Requirements)
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
