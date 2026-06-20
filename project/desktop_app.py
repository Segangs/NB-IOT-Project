#!/usr/bin/env python3
import os
import sys
import time
import multiprocessing
import webview
from app import app

def run_flask_backend():
    """Target function to run the Flask Web/API server in a child process."""
    # Ensure stdout line buffering for clear logs
    sys.stdout.reconfigure(line_buffering=True)
    # Disable debug reloader to prevent spawning multiple processes in child
    app.run(host="127.0.0.1", port=8080, debug=False, use_reloader=False)

if __name__ == "__main__":
    # Use 'spawn' start method for safety and cross-platform compatibility
    multiprocessing.set_start_method('spawn', force=True)

    print("=" * 70)
    print("❄️ PicoTeam 독립형 관제 데스크톱 앱 (PyWebView) - 멀티프로세스 구동")
    print("=" * 70)
    
    # 1. Start Flask Web Server in a separate child process
    flask_process = multiprocessing.Process(
        target=run_flask_backend,
        name="DesktopFlaskBackendProcess"
    )
    flask_process.daemon = True
    flask_process.start()
    print("▶️ [Process 1] Flask 웹 API 서버 백엔드가 가동되었습니다. (포트: 8080)")
    
    # Wait a moment for Flask to fully start and bind the port
    time.sleep(1.5)
    
    # 2. Main process runs PyWebView on the main thread
    print("▶️ [Process 2 / Main] PyWebView GUI 렌더러 창을 활성화합니다...")
    try:
        webview.create_window(
            title="PicoTeam 냉동공조기계 지능형 이상감지 관제 시스템",
            url="http://127.0.0.1:8080/",
            width=1280,
            height=850,
            resizable=True
        )
        # Start PyWebView loop (blocks main thread until window is closed)
        webview.start(user_agent="PicoTeamDesktop/1.0")
    except Exception as e:
        print(f"🚨 GUI 가동 오류 발생: {e}")
    finally:
        # 3. Ensure the child Flask process is cleaned up when GUI exits
        if flask_process.is_alive():
            print("👋 GUI가 종료되었습니다. Flask 백엔드 프로세스를 정지합니다...")
            flask_process.terminate()
            flask_process.join()
        print("모든 백그라운드 프로세스가 완벽히 정지되었습니다. 데스크톱 앱을 종료합니다.")
