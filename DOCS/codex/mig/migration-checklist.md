# migration-checklist.md
# Antigravity → OpenAI Codex 수동 마이그레이션 체크리스트

---

## 체크리스트 사용법
- `[ ]` 미완료 / `[x]` 완료 / `[~]` 부분 완료 / `[!]` 주의 필요

---

## Phase 1: Codex 환경 초기 설정

- [x] Codex CLI 설치 및 버전 확인
  ```bash
  npm install -g @openai/codex
  codex --version
  ```
- [x] `.codex/config.toml` 또는 `~/.codex/config.toml` 배치
  - 실제 파일: `/Users/segang/Documents/NB-IOT/.codex/config.toml`
  - Codex 0.142.5 매뉴얼 기준으로 `approval_policy`, `sandbox_mode`, `[sandbox_workspace_write]`, `[mcp_servers.supabase]` 구조 적용
- [x] `AGENTS.md`를 프로젝트 루트에 배치
  - `/Users/segang/Documents/NB-IOT/AGENTS.md`
  - PicoTeam 경로는 `NB-IOT/Segang/project`로 통합되어 별도 배치하지 않음
- [~] Codex 모델 선택 확인
  - 프로젝트 config에서는 모델을 고정하지 않음. 모델/프로바이더 기본값은 사용자 설정 또는 세션 설정을 따르도록 유지.
- [x] approval_policy 설정 확인
  - Antigravity의 `suggest` 매핑은 Codex 실제 키가 아니므로 사용하지 않음.
  - 현재 프로젝트 config: `approval_policy = "on-request"`

---

## Phase 2: MCP 서버 재연결 및 재인증

### Supabase MCP
- [x] Supabase Access Token 유효성 확인
  - 발급 위치: https://supabase.com/dashboard → Account → Access Tokens
  - Antigravity 원본 토큰: `/Users/segang/.gemini/antigravity/mcp_oauth_tokens.json` (직접 복사 금지, 재발급 권장)
- [x] Codex에서 Supabase MCP 연결 테스트
  - `list_projects` 도구로 프로젝트 목록 조회 정상 여부 확인
  - `list_tables` 도구로 테이블 구조 조회 정상 여부 확인
- [x] Supabase 프로젝트 ID 확인 및 기록
  - 통합 서버 프로젝트 URL/KEY 재확인
  - 프로젝트: `NB_IOT`
  - Project ref: `yzorfvgpmkwnjpdfyqsk`
  - API URL: `https://yzorfvgpmkwnjpdfyqsk.supabase.co`
  - 상태: `ACTIVE_HEALTHY`
  - 주의: public 16개 테이블 RLS 비활성 및 SECURITY DEFINER 함수 권고 확인. 별도 보안 설계/승인 후 처리 필요.

### Chrome DevTools MCP (추정)
- [x] Codex가 chrome_devtools MCP를 지원하는지 공식 문서에서 확인
  - Antigravity에서 `mcp(chrome_devtools/evaluate_script)` 사용 이력 있음
  - Codex 미지원 시 대체 방법 검토 (Playwright MCP 등)
  - Codex 매뉴얼상 MCP 서버와 Chrome DevTools MCP 예시는 지원됨.
  - 현재 프로젝트에는 별도 chrome_devtools MCP 서버를 자동 등록하지 않음. Codex 앱의 in-app browser/Chrome plugin 또는 필요 시 Playwright/Chrome DevTools MCP로 대체.
- [~] 브라우저 자동화 테스트 워크플로우 재검증
  - 이번 마이그레이션에서는 실제 브라우저 워크플로우 실행 없음.

---

## Phase 3: 환경 변수 및 비밀키 재확인

### NB-IOT 펌웨어 (.env)
- [ ] `/Users/segang/Documents/NB-IOT/.env` 파일 존재 확인
- [ ] 아래 키 값이 올바르게 설정되어 있는지 확인:
  - `SUPABASE_ANON_KEY`
  - `APN_NAME`
  - `SUPABASE_HOST`
  - `SUPABASE_PORT`
  - `MQTT_BROKER_HOST` (기본값: `p.zxcx.io`)
  - `MQTT_BROKER_PORT` (기본값: `8883`)
- [ ] CMake 빌드 시 `.env` 값이 컴파일러 매크로로 정상 주입되는지 확인

### 통합 서버 (.env)
- [~] `/Users/segang/Documents/NB-IOT/Segang/project/.env` 파일 존재 확인
- [ ] 아래 키 값이 올바르게 설정되어 있는지 확인:
  - `SUPABASE_URL`
  - `SUPABASE_KEY`
  - `FLASK_SECRET_KEY`
  - `GOOGLE_CLIENT_ID`
  - `GOOGLE_CLIENT_SECRET`
- [ ] Google OAuth 리디렉션 URI가 현재 환경에 등록되어 있는지 확인

### EMQX 브로커 인증
- [ ] EMQX API Key (`supabase_setup`) 유효성 확인
  - `emqx_setup.sh` 내 `AUTH_HEADER` 값이 현재 유효한 키인지 확인
  - 만료 시 EMQX 대시보드에서 재발급 후 `emqx_setup.sh` 업데이트
- [ ] EMQX Docker volume 마운트 상태 확인
  - 기존 volume ID: `55a915af067edf58f7598aeb3b89c296390a1e5535eba9c32877b9f0e7a65fcb`
  - 컨테이너 재시작 시 동일 volume 유지 여부 확인

---

## Phase 4: Build / Test 명령 검증

### NB-IOT 펌웨어 빌드
- [ ] Pico SDK 경로 확인: `/Users/segang/Documents/NB-IOT/pico-sdk`
- [ ] FreeRTOS Kernel 경로 확인: `/Users/segang/Documents/NB-IOT/FreeRTOS-Kernel`
- [ ] Pico SDK 버전 확인 (현재 CMakeLists.txt: `sdkVersion 2.2.0`)
- [ ] 툴체인 버전 확인 (현재: `toolchainVersion 14_2_Rel1`)
- [ ] 빌드 성공 여부 확인:
  ```bash
  cd /Users/segang/Documents/NB-IOT/build
  cmake .. && make -j$(sysctl -n hw.ncpu)
  ```
- [ ] `nb_iot_project.uf2` 생성 확인

### 통합 서버 실행 테스트
- [ ] Python 의존성 설치 확인:
  ```bash
  pip3 install -r /Users/segang/Documents/NB-IOT/Segang/project/requirements.txt
  ```
- [ ] Flask 서버 기동 확인: `python3 app.py`
- [ ] Mock 클라이언트 테스트: `python3 mock_pico_client.py`
- [ ] Supabase Realtime WebSocket 연결 확인
- [ ] PyWebView 데스크톱 앱 실행 확인: `python3 desktop_app.py`

---

## Phase 5: Git 설정 검증

- [x] 각 워크스페이스의 `.gitignore` 확인
  - `.env` 파일이 gitignore에 포함되어 있는지 확인
  - `/Users/segang/Documents/NB-IOT/.gitignore`
  - PicoTeam은 `NB-IOT/Segang/project`로 통합됨
- [x] Git 커밋 메시지 언어 설정 확인 (한국어)
- [x] `project_history.md` 파일이 git에 추적되는지 확인
- [~] `walkthrough.md` 파일 처리 방식 결정 (추적 여부)
  - 현재 파일 없음. 작업별 임시 보고가 필요할 때만 생성.

---

## Phase 6: Hooks / 자동화 검증

- [x] Antigravity의 사전 승인된 명령어 목록을 Codex approval 설정으로 이전 완료 여부 확인
  - 원본 위치: `/Users/segang/.gemini/config/config.json` → `globalPermissionGrants.allow`
  - Codex에는 동일한 `approval.allow_commands` 구조가 없으므로 그대로 이식하지 않음.
  - 프로젝트 config는 `approval_policy = "on-request"`로 두고, 반복 명령은 Codex 승인/샌드박스 정책을 따름.
- [~] Codex에서 `command(cmake ..)`, `command(make ...)` 등 빌드 명령 자동 승인 여부 확인
  - 명령 자동 승인 이전 대신 `build-firmware` skill로 표준 빌드 절차를 보존.
- [~] read_url 허용 도메인 목록 Codex에 설정:
  - `github.com`, `docs.emqx.com`, `supabase.com`, `forum.sierrawireless.com`
  - Codex 웹 검색/브라우징 정책은 런타임 도구 정책을 따름. 프로젝트 config에는 도메인 allowlist를 강제하지 않음.
- [x] Codex 세션 중단/재개 시 `plan.md` 또는 `task.md` 상태 지속 여부 확인
  - 장기 작업은 Codex plan/checklist 및 필요 시 `project_history.md` 누적 기록으로 관리.

---

## Phase 7: 프로젝트 컨텍스트 및 이력 이전

- [x] `project_history.md` 내용을 Codex 프로젝트 지침에 요약 포함
  - NB-IOT: `/Users/segang/Documents/NB-IOT/project_history.md` (약 93KB, 1057줄)
  - 통합 서버: `/Users/segang/Documents/NB-IOT/Segang/project`
- [x] `agent.md` 내용을 AGENTS.md에 통합 또는 참조 추가
  - 원본: `/Users/segang/Documents/NB-IOT/DOCS/antigravity/agent.md`
  - Codex 프로젝트 지침에는 핵심 운영 원칙만 반영.
- [x] 최근 대화 세션의 주요 결정사항 확인 (대화 ID: `9dc91f96-ffb3-4b09-99d9-8e51ecea9d9e`)
  - `project_history.md` 상단 최신 작업 이력 확인.

---

## Phase 8: 누락 가능성 있는 항목

### 추정 항목 (확인 필요)
- [ ] **Antigravity "knowledge" 디렉토리**: `/Users/segang/.gemini/antigravity/knowledge/`
  - 내용 확인 후 Codex에 동등한 방식으로 이전 여부 결정
- [ ] **Antigravity Conversations**: `/Users/segang/.gemini/antigravity/conversations/`
  - 과거 대화 이력 중 Codex에서도 참조가 필요한 것이 있는지 확인
- [ ] **Sidecars 설정**: `/Users/segang/.gemini/config/sidecars/`
  - Antigravity 전용 기능으로 Codex에 동등 기능 없을 수 있음
- [x] **Antigravity agent.md의 Superpowers / karpathy-guidelines 지침**
  - Codex에서 동등한 skill/plugin 시스템이 없는 경우 AGENTS.md에 핵심 원칙만 발췌 통합
- [ ] **EMQX 룰 엔진 SQL 현재 상태 백업**
  - EMQX 대시보드에서 현재 배포된 telemetry_rule, boot_rule, config_fetch_rule SQL 내보내기

### Codex repo skills
- [x] `build-firmware`
- [x] `run-server`
- [x] `supabase-inspect`
- [x] `db-migrate`
- [x] `commit-and-log`
- [x] `modem-debug`
- [x] `emqx-setup`
- [x] `mock-test`
- [x] `project-history-update`
  - 위치: `/Users/segang/Documents/NB-IOT/.agents/skills/*/SKILL.md`

### 알려진 미이전 항목
- [ ] Antigravity 브라우저 녹화 (`browser_recordings/`) — Codex에서 재현 불필요
- [ ] Antigravity 빌트인 skill (`antigravity_guide`) — Codex 전환 후 불필요
- [ ] Antigravity Installation UUID — Codex에서 불필요

---

## Phase 9: 최종 검증

- [x] Codex 세션에서 AGENTS.md가 정상 로드되는지 확인
  - `codex debug prompt-input`에서 루트 `AGENTS.md` 및 repo skills 목록 로드 확인.
- [x] Codex 세션에서 `list_tables` MCP 도구가 정상 작동하는지 확인
- [ ] NB-IOT 펌웨어 빌드 → Pico 플래시 → USB 시리얼 모니터 연결 전 과정 검증
- [ ] 통합 서버 실행 → 대시보드 접속 → Supabase Realtime 데이터 수신 검증
- [ ] Git 커밋이 한국어 메시지로 정상 생성되는지 확인
- [x] `project_history.md` 상단 누적 방식이 정상 동작하는지 확인

---

## 참고: 주요 파일 위치 요약

| 파일 | 경로 |
|---|---|
| Antigravity 전역 설정 | `/Users/segang/.gemini/config/config.json` |
| Antigravity MCP 설정 | `/Users/segang/.gemini/config/mcp_config.json` |
| NB-IOT AGENTS.md (원본) | `/Users/segang/Documents/NB-IOT/.agents/AGENTS.md` |
| 통합 서버 경로 | `/Users/segang/Documents/NB-IOT/Segang/project` |
| Antigravity agent.md 백업 | `/Users/segang/Documents/NB-IOT/DOCS/antigravity/agent.md` |
| NB-IOT 프로젝트 이력 | `/Users/segang/Documents/NB-IOT/project_history.md` |
| 과거 PicoTeam 프로젝트 이력 | 통합 전 경로 `/Users/segang/Documents/PicoTeam/project_history.md` (현재 작업 경로 아님) |
| Supabase MCP 지침 | `/Users/segang/.gemini/antigravity/mcp/supabase/instructions.md` |
| Antigravity MCP OAuth 토큰 | `/Users/segang/.gemini/antigravity/mcp_oauth_tokens.json` (⚠️ 재발급 권장) |
| Codex AGENTS.md (신규) | `/Users/segang/Documents/NB-IOT/AGENTS.md` |
| Codex 설정 (신규) | `/Users/segang/Documents/NB-IOT/.codex/config.toml` |
| Codex Skills 계획 (신규) | 이 패키지의 `codex-skills-plan.md` |
