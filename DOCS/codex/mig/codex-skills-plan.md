# codex-skills-plan.md
# Antigravity Slash Command / Workflow → Codex Skill 이전 계획

---

## 개요

Antigravity에서 사용하던 반복 workflow와 slash command를 Codex skill로 옮기기 위한 목록입니다.
각 skill은 `name`, `description`, `trigger`, `steps`로 구성됩니다.

---

## Skill 목록

---

### Skill 1: `build-firmware`

| 항목 | 내용 |
|---|---|
| **name** | `build-firmware` |
| **description** | NB-IOT 펌웨어를 CMake로 빌드하고 결과를 보고한다 |
| **trigger** | "빌드", "펌웨어 빌드", "cmake build", "make 해줘" 등 NB-IOT 빌드 요청 |

**Steps:**
1. `/Users/segang/Documents/NB-IOT` 디렉토리 확인
2. `.env` 파일 존재 여부 확인 (없으면 `.env.example` 기반으로 생성 안내)
3. `mkdir -p build && cd build && cmake ..` 실행
4. `make -j$(sysctl -n hw.ncpu)` 실행
5. 빌드 결과 요약 출력 (성공/오류 로그)
6. 성공 시 `build/nb_iot_project.uf2` 경로 안내

---

### Skill 2: `run-server`

| 항목 | 내용 |
|---|---|
| **name** | `run-server` |
| **description** | 통합 Flask 관제 서버를 로컬에서 실행한다 |
| **trigger** | "서버 실행", "Flask 실행", "앱 켜줘", "run server" 등 |

**Steps:**
1. `/Users/segang/Documents/NB-IOT/Segang/project` 디렉토리 이동
2. `.env` 파일 존재 및 필수 키 확인 (`SUPABASE_URL`, `SUPABASE_KEY`, `GOOGLE_CLIENT_ID`)
3. `pip3 install -r requirements.txt` (필요 시)
4. `python3 app.py` 실행
5. 포트 번호와 접속 URL 출력

---

### Skill 3: `supabase-inspect`

| 항목 | 내용 |
|---|---|
| **name** | `supabase-inspect` |
| **description** | Supabase DB 상태를 점검한다 (테이블, 로그, 어드바이저 확인) |
| **trigger** | "DB 확인", "Supabase 체크", "테이블 조회", "로그 확인" 등 |

**Steps:**
1. MCP `supabase/list_projects`로 프로젝트 목록 확인
2. MCP `supabase/list_tables`로 현재 테이블 구조 확인
3. MCP `supabase/get_logs`로 최근 로그 확인
4. MCP `supabase/get_advisors`로 성능/보안 권고사항 확인
5. 요약 보고

---

### Skill 4: `db-migrate`

| 항목 | 내용 |
|---|---|
| **name** | `db-migrate` |
| **description** | Supabase DB 마이그레이션을 안전하게 적용한다 |
| **trigger** | "마이그레이션", "DB 변경", "테이블 수정", "RPC 수정" 등 |

**Steps:**
1. MCP `supabase/list_tables`로 현재 스키마 확인
2. 변경 SQL 초안 작성 및 사용자에게 검토 요청 (승인 필수)
3. MCP `supabase/apply_migration`으로 적용
4. MCP `supabase/execute_sql`로 결과 검증
5. 변경 내용 `project_history.md` 상단에 누적 기록

> ⚠️ `apply_migration`은 원격 DB에 직접 반영됨. 반드시 사용자 승인 후 실행.

---

### Skill 5: `commit-and-log`

| 항목 | 내용 |
|---|---|
| **name** | `commit-and-log` |
| **description** | 작업 완료 후 Git 커밋 + project_history.md + walkthrough.md를 자동으로 처리한다 |
| **trigger** | "커밋해줘", "작업 마무리", "저장해줘", "finish task" 등 |

**Steps:**
1. 변경된 파일 목록 확인 (`git status`)
2. `walkthrough.md`에 이번 작업 내용 단순/명료하게 작성
3. `project_history.md` **상단**에 최신 작업 항목 추가 (기존 내용 삭제 금지)
4. `git add .` → `git commit -m "feat: <한국어 간결 메시지>"`
5. 커밋 해시와 메시지 출력

---

### Skill 6: `modem-debug`

| 항목 | 내용 |
|---|---|
| **name** | `modem-debug` |
| **description** | HL7811 모뎀 AT 커맨드 문제를 체계적으로 디버깅한다 |
| **trigger** | "모뎀 오류", "AT 에러", "CME ERROR", "MQTT 안됨", "모뎀 디버그" 등 |

**Steps:**
1. 오류 코드 확인 (CME ERROR 번호로 원인 분류)
2. 주요 체크리스트 실행:
   - `PWR_ON_PIN` 펄스 시퀀스 확인 (HIGH→LOW→HIGH)
   - `pico_enable_stdio_uart` = 0 확인 (UART 간섭 방지)
   - AT 종결자 `\r`만 사용 여부 확인
   - `modem_ReadResponse()` 256바이트 가드 존재 여부
   - `is_modem_busy` 가드락 상태 확인
   - SSL 검증 레벨 `AT+KSSLCFG=0,3` 확인
3. 증상별 빠른 참조:
   - `CME ERROR: 0` → CA 인증서 슬롯 0 비어있음 (GTS_ROOT_R4 더미 주입 필요)
   - `CME ERROR: 907` → auth_device 인자명 불일치 (username/password 확인)
   - `CME ERROR: 931` → SSL 인증서 주입 실패 (청크 64바이트/10ms 분할 확인)
   - 무한 ERROR 루프 → stdio_uart 활성화 충돌 또는 DebugTask race condition
4. 수정 후 리빌드 및 USB 시리얼 모니터로 결과 확인

---

### Skill 7: `emqx-setup`

| 항목 | 내용 |
|---|---|
| **name** | `emqx-setup` |
| **description** | EMQX 브로커 설정을 Supabase와 연동하여 초기화한다 |
| **trigger** | "EMQX 설정", "브로커 초기화", "emqx_setup", "MQTT 브로커 연동" 등 |

**Steps:**
1. `.env` 파일에서 `SUPABASE_URL`, `SUPABASE_KEY` 확인
2. `bash emqx_setup.sh` 실행
3. 각 단계 결과 확인:
   - [1/6] HTTP 기기 인증 (Supabase auth_device RPC)
   - [2/6] telemetry_rule SQL 룰 등록
   - [3/6] boot_rule SQL 룰 등록
   - [4/6] config_fetch_rule 등록
   - [5/6] Supabase HTTP Action 연결
   - [6/6] 인증서 검증
4. EMQX API Key 유효성 확인 (만료 시 재발급 안내)
5. Docker volume 마운트 상태 확인 (`/opt/emqx/data` 경로)

> ⚠️ EMQX Docker volume 교체 시 기존 설정 유실 위험 — 반드시 volume ID 확인 후 진행

---

### Skill 8: `mock-test`

| 항목 | 내용 |
|---|---|
| **name** | `mock-test` |
| **description** | Pico 없이 서버 연동을 테스트하기 위해 mock 클라이언트를 실행한다 |
| **trigger** | "모의 테스트", "mock 클라이언트", "Pico 없이 테스트", "mock_pico" 등 |

**Steps:**
1. Flask 서버가 실행 중인지 확인
2. `python3 mock_pico_client.py` 실행
3. Supabase DB에 데이터 수신 확인 (`device_boot_logs`, `sensor_data` 테이블)
4. 결과 요약 출력

---

### Skill 9: `project-history-update`

| 항목 | 내용 |
|---|---|
| **name** | `project-history-update` |
| **description** | project_history.md에 새 작업 항목을 최신순으로 누적 추가한다 |
| **trigger** | "이력 업데이트", "history 추가", "개발 이력 기록" 등 |

**Steps:**
1. 현재 `project_history.md` 상단 섹션 확인
2. 신규 항목을 `## 📅 YYYY-MM-DD: [범주] 작업제목` 형식으로 작성
3. 항목 내용: 연동 대화 ID, 개발 범주, 작업 및 해결 내역
4. 기존 항목 **삭제 없이** 신규 항목을 최상단에 삽입
5. 저장 및 `git add project_history.md` 포함

---

## Antigravity Slash Command → Codex 매핑 참고

| Antigravity Slash Command | Codex 대응 방법 |
|---|---|
| `/goal` | Codex에 장기 목표 설정 후 반복 실행 지시 |
| `/schedule` | OS cron 또는 Codex 세션 예약으로 대체 (추정) |
| `/browser` | chrome_devtools MCP 또는 별도 브라우저 자동화 도구 |
| `/grill-me` | 사전 인터뷰 프롬프트 템플릿으로 대체 |
| `/learn` | AGENTS.md 또는 skill 파일에 직접 기록하여 영속화 |
