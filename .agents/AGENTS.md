# Project Rules

- **누적식 프로젝트 작업 이력(project_history) 유지**:
  - 개별 작업 완료 보고서인 `walkthrough.md`는 해당 작업의 내용만 단순/명료하게 작성하십시오.
  - 대신, 프로젝트 전체 개발 이력을 추적하는 `project_history.md`를 작성하거나 수정할 때, 이전 작업 히스토리를 지우지 말고 최신 작업 내용이 상단에 위치하도록 지속적으로 누적하여 기록하십시오.

- **깃 커밋 메시지 한글 및 간결 작성**:
  - 앞으로 Git에 커밋을 진행할 때 커밋 메시지는 반드시 **한글(Korean)**로 작성하십시오. (영어 메시지 지양)
  - 메시지는 길게 늘여 쓰지 말고, 핵심 키워드 위주로 **최대한 짧고 간결하게** 작성하십시오.
  - 형식 예시: `feat: FreeRTOS, 센서, 플래시, LCD 통합`

- **README / project_history 작성 형식**:
  - `README.md` 및 `project_history.md` 신규 항목 작성 시 `했습니다`, `합니다`, `됩니다` 등 풀이형 종결 지양.
  - 항목 제목, bullet, 요약 문장은 `정리`, `반영`, `검증`, `동기화`, `기록` 등 명사형 종결 사용.

- **PCB / 회로도 설계 자료 참조**:
  - GPIO, 전원 Net, RJ45 센서 케이블링, I2S 마이크, LTC2954 전원관리, EasyEDA/PCB 관련 작업 전 `DOCS/PCB/pico2w_rm78_sensor_pcb_design_portfolio.md` 우선 확인.
  - SPH0645LM4H 마이크는 단순 녹음용이 아니라 Edge AI/TinyML 기반 정상·이상·이상 예측 상태 판단용 장비 음향 데이터 수집 채널로 취급.
  - 향후 펌웨어/서버 설계 시 원시 PCM 또는 FFT, RMS, 주파수 대역 에너지, MFCC 유사 특징량 등 음향 특징 데이터 흐름 고려.
  - 현재 PCB 설계 기준 핵심 후속 확인: R6 100kΩ → 1kΩ 변경, GP7 감지 전압 3.3V 이하 확인, C4 22µF 안전 종료 지연 의도 확인.

- **사전 승인된 명령어 및 URL (반복 Allow 팝업 방지)**:
  - `python3`, `pip3`, `curl`, `nano`, `echo`, `touch`, `rm`, `mv`, `chmod`, `uname`, `ps`, `kill`, `sleep`, `zip`, `unzip`, `tar`, `brew`, `node`, `npm`, `npx`, `ping`, `nslookup`, `dig`, `ssh-keygen` 명령어는 별도의 허가 요청 없이 실행 가능합니다.
  - 웹 검색 및 기술 문서 URL 읽기(`read_url`)는 별도의 허가 요청 없이 수행 가능합니다.
