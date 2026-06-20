#!/usr/bin/env python3
import socket
import time
import random
import sys

def main():
    # Allow passing target host and port as arguments
    host = sys.argv[1] if len(sys.argv) > 1 else 'localhost'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 1818
    
    print("=" * 60)
    print(f"❄️ Raspberry Pi Pico W 온도 센서 시뮬레이터 구동")
    print(f"연결 서버: {host}:{port}")
    print("=" * 60)
    
    sensor_id = 1
    device_id = 1
    
    # Establish connection
    try:
        client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client_socket.connect((host, port))
        print("✅ 소켓 서버 연결 성공!")
        print("3초 간격으로 온도 데이터를 전송합니다. (Ctrl+C로 종료)")
        print("-" * 60)
        
        step = 0
        while True:
            # Generate normal/varying temperature values
            # Normal range is around -22°C to 5°C
            # Periodically spike to trigger alarm limits (e.g., 35°C or -30°C)
            if step > 0 and step % 8 == 0:
                # Alarm Spike!
                temp = round(random.choice([32.4, 35.8, -32.5]), 1)
                print(f"⚠️ [임계치 일탈 발생 모의] 온도 경보 테스트용 스파이크 생성!")
            else:
                # Normal variation
                temp = round(random.choice([random.uniform(-23.0, -19.0), random.uniform(1.5, 4.5)]), 1)
            
            # Formulate packet
            # Support both formats:
            # 1. Plain float (for backwards compatibility): str(temp)
            # 2. Structured string: f"DEVICE:{device_id},SENSOR:{sensor_id},TEMP:{temp}"
            packet_format = random.choice([
                f"DEVICE:{device_id},SENSOR:{sensor_id},TEMP:{temp}",
                str(temp)
            ])
            
            print(f"📡 송신 패킷 데이터: '{packet_format}'")
            client_socket.send(packet_format.encode('utf-8'))
            
            # Wait for control command response (e.g. "motor on" / "motor off")
            response = client_socket.recv(1024).decode('utf-8')
            print(f"📥 수신 제어 명령: '{response}'")
            print("-" * 60)
            
            step += 1
            time.sleep(3)
            
    except ConnectionRefusedError:
        print("❌ 연결 실패: 서버가 꺼져 있거나 포트 번호가 올바르지 않습니다.")
        print("python3 tcp_server.py가 먼저 구동 중인지 확인해 주세요.")
    except KeyboardInterrupt:
        print("\n👋 시뮬레이터 전송을 종료합니다.")
    except Exception as e:
        print(f"❌ 예외 발생: {e}")
    finally:
        try:
            client_socket.close()
        except:
            pass

if __name__ == "__main__":
    main()
