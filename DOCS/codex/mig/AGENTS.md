# AGENTS.md — NB-IoT 통합 개발 지침
# Codex 프로젝트 루트(또는 각 워크스페이스 루트)에 배치

---

## 1. 프로젝트 개요

본 프로젝트는 **NB-IoT (HL7811) 셀룰러 모듈 + Raspberry Pi Pico 2 W** 단말을 기반으로 한
**초저전력 지능형 이상온도 감지 및 실시간 원격 관제 시스템**입니다.

- **워크스페이스 A (임베디드 펌웨어)**: `/Users/segang/Documents/NB-IOT`
  - MCU: RP2350 (Pico 2 W), C/C++ SDK 17, CMake 빌드
  - RTOS: FreeRTOS (멀티태스킹: 센서, LCD, 모뎀, 부저)
  - 모뎀: HL7811 (LTE-M/NB-IoT), TLS 1.2 (MQTTS port 8883)
- **워크스페이스 B (관제 서버)**: `/Users/segang/Documents/NB-IOT/Segang/project`
  - Backend: Python Flask
  - DB/BaaS: Supabase PostgreSQL + Realtime WebSocket
  - Desktop: PyWebView 패키징

---

## 2. Git 커밋 규칙

- **커밋 메시지는 반드시 한국어(Korean)로 작성**
- 핵심 키워드 위주로 최대한 짧고 간결하게 작성
- 형식 예시:
  ```
  feat: FreeRTOS, 센서, 플래시, LCD 통합
  fix: UART stdio 간섭 해제
  refactor: MQTTS 페이로드 JSON Array 전환
  ```

---

## 3. Build / Test / Run 명령

### NB-IOT 펌웨어 (C/C++, CMake)

```bash
# 빌드 디렉토리 초기화 (최초 1회)
cd /Users/segang/Documents/NB-IOT
mkdir -p build && cd build
cmake ..

# 빌드 (병렬)
make -j$(sysctl -n hw.ncpu)
# 또는
ninja

# 플래시 전송 (Pico 2 W UF2)
# build/ 디렉토리에 생성된 nb_iot_project.uf2를 Pico에 복사
```

**환경 변수 (.env 파일 필수):**
```bash
# /Users/segang/Documents/NB-IOT/.env (gitignore됨)
SUPABASE_ANON_KEY="<SUPABASE_ANON_KEY>"
APN_NAME="<APN_NAME>"
SUPABASE_HOST="<SUPABASE_HOST>"
SUPABASE_PORT="443"
MQTT_BROKER_HOST="p.zxcx.io"
MQTT_BROKER_PORT="8883"
```
> CMakeLists.txt가 `.env`를 읽어 컴파일러 매크로로 주입함

### 통합 관제 서버 (Python Flask)

```bash
cd /Users/segang/Documents/NB-IOT/Segang/project

# 의존성 설치
pip3 install -r requirements.txt

# 개발 서버 실행
python3 app.py

# 데스크톱 앱 실행
python3 desktop_app.py

# Pico 모의 클라이언트 (테스트용)
python3 mock_pico_client.py

# EMQX 설정 자동화 (최초 배포 시)
bash emqx_setup.sh
```

**환경 변수 (.env 파일 필수):**
```
SUPABASE_URL=<SUPABASE_URL>
SUPABASE_KEY=<SUPABASE_ANON_KEY>
FLASK_SECRET_KEY=<FLASK_SECRET_KEY>
GOOGLE_CLIENT_ID=<GOOGLE_CLIENT_ID>
GOOGLE_CLIENT_SECRET=<GOOGLE_CLIENT_SECRET>
```

### TCP 디버그 서버
```bash
python3 tcp_server.py
```

---

## 4. 코딩 스타일 및 컨벤션

### C/C++ 펌웨어 (NB-IOT)
- C Standard: C11 / C++ Standard: C++17
- 보드: `pico2_w` / 플랫폼: `rp2350`
- **UART0 (GP0/GP1)**: 모뎀 전용 — stdio 절대 공유 금지
  - `pico_enable_stdio_uart` = 0 (비활성화 유지)
  - `pico_enable_stdio_usb` = 1 (디버깅은 USB만)
- AT 커맨드 종결자: `\r` 단독 (절대 `\r\n` 금지 — HL7811이 `\n`을 빈 명령으로 처리)
- UART 수신 루프에 **최대 256바이트 가드** 유지 (무한 루프 방지)
- MQTTS 페이로드 최대 80바이트 제한 엄수 (JSON Array 형식 사용)
  - 텔레메트리: `[sensorId, temperature]` (예: `[1, -8.5]`)
  - 부팅 로그: `[vsys_v, chip_temp, ram_ok, 0, carrier, rssi, ...]`
- Flash 로그 구조체 32바이트 정렬 유지
- 모뎀 통신 중 `is_modem_busy` 가드락 필수 사용
- FreeRTOS 태스크 이름: `vBootTask`, `vSensorTask`, `vLCDTask`, `vPeriodicModemTask`, `vBuzzerTask`

### Python 서버 (통합 서버)
- Python 3, Flask >= 3.0.0
- Supabase SDK >= 2.3.0
- `.env` 파일로 모든 비밀값 관리 (`python-dotenv` 사용)
- Supabase 스키마 변경 시 `list_tables`로 기존 구조 확인 먼저
- DB 마이그레이션 전 반드시 `get_logs`, `get_advisors` 확인

---

## 5. 아키텍처 핵심 결정사항

### 펌웨어 (NB-IOT)
- SSL 검증 레벨: `AT+KSSLCFG=0,3` (전체 검증 활성화 — 이전에 `0,0`으로 우회했다가 `0,3`으로 원복됨)
- ISRG Root X2 CA 인증서를 **64바이트 청크 분할, 10ms 딜레이** 주입 (UART 버퍼 오버런 방지)
- MQTT QoS 1 성공 URC: `+KMQTT_IND: <session_id>,3` 또는 `,4` 둘 다 성공으로 처리
- HL7811 PWR_ON_N 핀 펄스 시퀀스: `HIGH(1s) → LOW(1.5s) → HIGH(release)`
- RAM 무결성 테스트는 강제 정상 통과 처리 (`ram_ok = true` 고정)

### 서버/DB (통합 서버)
- Supabase `auth_device` RPC 인자명: `username text, password text` (EMQX HTTP Authenticator가 보내는 바디 키와 일치해야 함)
- `get_device_sensors` RPC: `usermachine + usersettings` LEFT JOIN 포함 (`tempUpperLimitValue`, `tempLowerLimitValue` 반환)
- EMQX 룰 엔진 SQL: `json_decode(payload)` + `nth` 사용, `cast(col, type)` 콤마 문법 사용 (괄호형 안됨)
- Supabase Realtime WebSocket으로 DOM 깜빡임 없는 실시간 갱신
- Google OAuth 2.0 + PyWebView 루프백 로그인 (Safari/Chrome 패스키 우회)

---

## 6. 코드 리뷰 기준

1. **버그/리그레션 우선** — 심각도 순으로 파일+라인 참조 명시
2. **안전성**: 커밋/배포/DB 마이그레이션 전 반드시 확인 요청
3. **외부 부작용 작업**: push, merge, deploy, DB 변경, 이메일 전송 시 사전 승인 필수
4. 검증 없이 "완료"라고 선언하지 말 것
5. 타겟 테스트 → 타입체크/lint → 빌드 확인 순으로 검증

---

## 7. Agent 행동 규칙

### 이력 관리
- `walkthrough.md`: 해당 작업 내용만 단순/명료하게 작성
- `project_history.md`: 이전 히스토리 삭제 금지, **최신 내용을 상단에 누적 추가**

### 사전 승인된 명령어 (확인 없이 실행 가능)
`python3`, `pip3`, `curl`, `nano`, `echo`, `touch`, `rm`, `mv`, `chmod`, `uname`,
`ps`, `kill`, `sleep`, `zip`, `unzip`, `tar`, `brew`, `node`, `npm`, `npx`,
`ping`, `nslookup`, `dig`, `ssh-keygen`,
`cat`, `ls`, `find`, `grep`, `head`, `tail`, `wc`, `which`, `cp`, `mkdir`,
`cmake`, `make`, `ninja`, `openssl`, `mosquitto_pub`,
`git add`, `git commit`, `git status`, `git log`, `git diff`, `git branch`,
`git checkout`, `git clone`, `git subtree`, `git filter-branch`

### 사전 승인된 URL 읽기
기술 문서, GitHub, Supabase docs, EMQX docs, Sierra Wireless 포럼은 별도 승인 없이 읽기 가능

### 계획(plan.md)
- 비사소한 멀티스텝 작업에만 `plan.md` 유지
- 단순 Q&A, 짧은 명령, 한줄 수정에는 plan.md 불필요

---

## 8. 프로젝트 주요 파일 위치

### NB-IOT
| 파일/경로 | 역할 |
|---|---|
| `main.cpp` | 메인 진입점, FreeRTOS 태스크 생성 |
| `src/config.h` | 핀 설정, 타이밍, 임계값 상수 |
| `src/tasks/tasks_sensor.cpp` | NTC 온도 측정 태스크 |
| `src/tasks/tasks_lcd.cpp` | LCD I2C 렌더링 태스크 |
| `src/tasks/tasks_modem.cpp` | HL7811 AT 명령, MQTTS 통신 |
| `src/lib/flash_logger.cpp` | 비휘발성 Flash 로깅 |
| `lib/LCD_I2C.cpp` | I2C LCD 드라이버 |
| `CMakeLists.txt` | CMake 빌드 설정 |
| `.env` | 비밀 자격증명 (gitignore) |
| `.env.example` | 환경변수 템플릿 |
| `project_history.md` | 누적 개발 이력 |

### 통합 서버
| 파일/경로 | 역할 |
|---|---|
| `Segang/project/app.py` | Flask 관제 서버 메인 |
| `Segang/project/desktop_app.py` | PyWebView 데스크톱 패키지 |
| `Segang/project/tcp_server.py` | TCP 디버그 서버 |
| `Segang/project/mock_pico_client.py` | Pico 모의 테스트 클라이언트 |
| `Segang/project/emqx_setup.sh` | EMQX 설정 자동화 스크립트 |
| `Segang/project/requirements.txt` | Python 의존성 |
| `Segang/project/.env` | 비밀 자격증명 (gitignore) |
| `Segang/project/templates/` | HTML 템플릿 (dashboard, board 등) |
| `project_history.md` | 누적 개발 이력 |
| `DOCS/antigravity/agent.md` | Antigravity 에이전트 행동 지침 백업 |
