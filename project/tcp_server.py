#!/usr/bin/env python3
import os
import socket
import threading
import json
from datetime import datetime, timezone, timedelta
from dotenv import load_dotenv
from supabase import create_client, Client

# Load environment variables
load_dotenv()

SUPABASE_URL = os.environ.get("SUPABASE_URL")
SUPABASE_KEY = os.environ.get("SUPABASE_KEY")
TCP_PORT = int(os.environ.get("TCP_SERVER_PORT", 1818))

# Initialize Supabase client
supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)

# Server state logs list for UI dashboard (stored in-memory and written to file)
LOG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tcp_server.log")

def add_log(message):
    timestamp = datetime.now(timezone(timedelta(hours=9))).strftime("%Y-%m-%d %H:%M:%S")
    log_line = f"[{timestamp}] {message}"
    
    # Write to local file for Flask to read (clean text without ANSI colors)
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(log_line + "\n")
            
        # Rotate log file if it gets too large (> 1MB)
        if os.path.getsize(LOG_FILE) > 1024 * 1024:
            with open(LOG_FILE, "w", encoding="utf-8") as f:
                f.write(log_line + "\n")
    except Exception:
        pass

    # Distinct styled logging for terminal console monitoring
    color_prefix = "\033[1;36m[📡 TCP SOCKET]\033[0m"
    if "⚠️" in message or "🚨" in message or "경고" in message or "에러" in message or "오류" in message:
        color_msg = f"\033[1;33m{message}\033[0m"  # Bold Yellow for alerts/warnings
    elif "수신 데이터" in message:
        color_msg = f"\033[1;32m{message}\033[0m"  # Bold Green for incoming packet data
    elif "데이터베이스 저장" in message:
        color_msg = f"\033[1;34m{message}\033[0m"  # Bold Blue for DB operations
    else:
        color_msg = message
        
    print(f"[{timestamp}] {color_prefix} {color_msg}", flush=True)

def handle_client_connection(client_socket, client_address):
    add_log(f"클라이언트 연결됨: {client_address}")
    
    try:
        while True:
            data = client_socket.recv(1024)
            if not data:
                break
            
            decoded_data = data.decode("utf-8").strip()
            add_log(f"수신 데이터 [{client_address}]: {decoded_data}")
            
            # Parse data
            # Support plain float temperature value (legacy/simple test client support)
            # Support structured string: "DEVICE:<device_id>,SENSOR:<sensor_id>,TEMP:<temp>"
            # Support JSON format
            
            sensor_id = 1
            device_id = 1
            temp_value = None
            
            if decoded_data.startswith("{"):
                try:
                    payload = json.loads(decoded_data)
                    packet_type = payload.get("type")
                    
                    # [Case A] Boot self-check diagnostic telemetry
                    if packet_type == "boot":
                        add_log(f"부팅 자가 진단 패킷 감지! 수신 데이터: {payload}")
                        imei = payload.get("imei")
                        cimi = payload.get("cimi")
                        
                        userId = 1
                        deviceId = 1
                        
                        # 1단계: IMEI로 기기 정보 조회 (deviceId, userId, usimId 매핑)
                        dev_res = supabase.table("device").select("deviceId, userId, usimId").eq("deviceIMEI", imei).execute()
                        if dev_res.data and len(dev_res.data) > 0:
                            dev_info = dev_res.data[0]
                            d_id = dev_info.get("deviceId")
                            u_id = dev_info.get("userId")
                            usim_id = dev_info.get("usimId")
                            
                            # 2단계: usimId로 usimIMSI(cimi) 상호 검증
                            usim_res = supabase.table("usim").select("usimIMSI").eq("usimId", usim_id).execute()
                            if usim_res.data and len(usim_res.data) > 0:
                                db_imsi = usim_res.data[0].get("usimIMSI")
                                if db_imsi == cimi:
                                    add_log(f"기기 인증 및 매핑 성공! IMEI: {imei}, CIMI: {cimi} -> Device ID: {d_id}, User ID: {u_id}")
                                    userId = u_id
                                    deviceId = d_id
                                else:
                                    add_log(f"⚠️ 경고: USIM IMSI 불일치! DB: {db_imsi}, 수신: {cimi}")
                            else:
                                add_log(f"⚠️ 경고: USIM 매핑 레코드 없음! usimId: {usim_id}")
                        else:
                            add_log(f"⚠️ 경고: 등록되지 않은 IMEI 단말! {imei}")
                            
                        # 3단계: device_boot_logs 테이블에 자가진단 내역 적재
                        boot_log = {
                            "userId": userId,
                            "deviceId": deviceId,
                            "pico_voltage": float(payload.get("pico_voltage", 0.0)),
                            "temperature": float(payload.get("temperature", 0.0)),
                            "flash_integrity": int(payload.get("flash_integrity", 0)),
                            "ram_test": int(payload.get("ram_test", 0)),
                            "at_status": int(payload.get("at_status", 0)),
                            "cpin_status": int(payload.get("cpin_status", 0)),
                            "csq_rssi": int(payload.get("csq_rssi", 99)),
                            "cops_carrier": payload.get("cops_carrier", "Unknown"),
                            "temp_sensor_status": int(payload.get("temp_sensor_status", 0))
                        }
                        
                        res = supabase.table("device_boot_logs").insert(boot_log).execute()
                        if res.data:
                            add_log(f"부팅 자가 진단 로그 Supabase DB 저장 성공! Log ID: {res.data[0].get('id')}")
                        else:
                            add_log(f"부팅 자가 진단 로그 Supabase DB 저장 완료 (Identity Latch)")
                        
                        client_socket.send(b"boot_logged")
                        continue
                    
                    # [Case B] Emergency operational alert (unstable voltage or broken circuit)
                    elif packet_type == "error_alert":
                        add_log(f"🚨 긴급 장비 이상 알림 수신! 데이터: {payload}")
                        imei = payload.get("deviceIMEI")
                        
                        # Fetch associated user and machine ID to log alarm alert
                        dev_res = supabase.table("device").select("deviceId, userId").eq("deviceIMEI", imei).execute()
                        if dev_res.data and len(dev_res.data) > 0:
                            d_id = dev_res.data[0].get("deviceId")
                            u_id = dev_res.data[0].get("userId")
                            
                            mach_res = supabase.table("usermachine").select("userMachineId").eq("deviceId", d_id).execute()
                            if mach_res.data and len(mach_res.data) > 0:
                                u_mach_id = mach_res.data[0].get("userMachineId")
                                
                                alert_data = {
                                    "userId": u_id,
                                    "userMachineId": u_mach_id,
                                    "sensorValueId": None,
                                    "alertSendDate": datetime.now(timezone(timedelta(hours=9))).replace(tzinfo=None).isoformat()
                                }
                                supabase.table("alertsend").insert(alert_data).execute()
                                add_log(f"긴급 이상 상태에 대한 alertsend 알람 기록 완료 (Device: {d_id})")
                        
                        client_socket.send(b"alert_logged")
                        continue
                    
                    # [Case C] Periodic Temperature JSON reporting
                    else:
                        imei = payload.get("deviceIMEI")
                        sensor_val = payload.get("sensorValue")
                        
                        if imei is not None and sensor_val is not None:
                            temp_value = float(sensor_val)
                            # Lookup sensorId associated with this IMEI
                            dev_res = supabase.table("device").select("deviceId").eq("deviceIMEI", imei).execute()
                            if dev_res.data and len(dev_res.data) > 0:
                                d_id = dev_res.data[0].get("deviceId")
                                sens_res = supabase.table("sensor").select("sensorId").eq("deviceId", d_id).execute()
                                if sens_res.data and len(sens_res.data) > 0:
                                    sensor_id = sens_res.data[0].get("sensorId")
                                    device_id = d_id
                                else:
                                    sensor_id = 1
                            else:
                                sensor_id = 1
                        else:
                            sensor_id = int(payload.get("sensorId", 1))
                            device_id = int(payload.get("deviceId", 1))
                            temp_value = float(payload.get("temp", 0.0))
                            
                except Exception as ex:
                    add_log(f"JSON 파싱 에러: {ex}")
                    temp_value = None
            elif "TEMP:" in decoded_data:
                try:
                    parts = decoded_data.split(",")
                    for part in parts:
                        if part.startswith("DEVICE:"):
                            device_id = int(part.split(":")[1])
                        elif part.startswith("SENSOR:"):
                            sensor_id = int(part.split(":")[1])
                        elif part.startswith("TEMP:"):
                            temp_value = float(part.split(":")[1])
                except Exception as ex:
                    add_log(f"패킷 문자열 파싱 에러: {ex}")
            else:
                try:
                    temp_value = float(decoded_data)
                except ValueError:
                    add_log(f"알 수 없는 데이터 형식: {decoded_data}")
            
            if temp_value is not None:
                # 1. Write temperature to public.sensorvalue
                try:
                    time_str = datetime.now(timezone(timedelta(hours=9))).replace(tzinfo=None).isoformat()
                    # Check if sensor exists in database, otherwise insert a mockup sensor or fallback
                    sensor_data = {
                        "sensorId": sensor_id,
                        "sensorValue": temp_value,
                        "sensorvaluetime": time_str
                    }
                    res = supabase.table("sensorvalue").insert(sensor_data).execute()
                    
                    inserted_value_id = None
                    if res.data and len(res.data) > 0:
                        inserted_value_id = res.data[0].get("sensorValueId")
                        add_log(f"데이터베이스 저장 완료: 센서 ID {sensor_id}, 온도 {temp_value}°C")
                    else:
                        # Fallback for older supabase-py versions that do not return inserted rows automatically
                        # Search for the latest inserted value
                        latest_res = supabase.table("sensorvalue").select("sensorValueId").order("sensorValueId", desc=True).limit(1).execute()
                        if latest_res.data:
                            inserted_value_id = latest_res.data[0].get("sensorValueId")
                    
                    # 2. Fetch threshold settings from public.usersettings for this device/sensor
                    # Map sensorId -> deviceId -> userMachineId -> usersettings
                    sensor_db = supabase.table("sensor").select("deviceId").eq("sensorId", sensor_id).execute()
                    
                    if sensor_db.data and len(sensor_db.data) > 0:
                        device_id = sensor_db.data[0].get("deviceId")
                    
                    machine_db = supabase.table("usermachine").select("userMachineId").eq("deviceId", device_id).execute()
                    
                    if machine_db.data and len(machine_db.data) > 0:
                        user_machine_id = machine_db.data[0].get("userMachineId")
                        
                        settings_db = supabase.table("usersettings").select("tempUpperLimitValue, tempLowerLimitValue").eq("userMachineId", user_machine_id).execute()
                        
                        # Get user associated with device to log alerts
                        device_info = supabase.table("device").select("userId").eq("deviceId", device_id).execute()
                        user_id = device_info.data[0].get("userId") if (device_info.data and len(device_info.data) > 0) else 1
                        
                        if settings_db.data and len(settings_db.data) > 0:
                            settings = settings_db.data[0]
                            upper_limit = float(settings.get("tempUpperLimitValue") or 30.0)
                            lower_limit = float(settings.get("tempLowerLimitValue") or -20.0)
                            
                            motor_command = "motor off"
                            
                            if temp_value > upper_limit:
                                add_log(f"⚠️ 경고: 상한 임계치 초과 ({temp_value}°C > {upper_limit}°C)")
                                motor_command = "motor on"
                                alert_data = {
                                    "userId": user_id,
                                    "userMachineId": user_machine_id,
                                    "sensorValueId": inserted_value_id,
                                    "alertSendDate": datetime.now(timezone(timedelta(hours=9))).replace(tzinfo=None).isoformat()
                                }
                                supabase.table("alertsend").insert(alert_data).execute()
                                
                            elif temp_value < lower_limit:
                                add_log(f"⚠️ 경고: 하한 임계치 미만 ({temp_value}°C < {lower_limit}°C)")
                                # Insert alarm alert
                                alert_data = {
                                    "userId": user_id,
                                    "userMachineId": user_machine_id,
                                    "sensorValueId": inserted_value_id,
                                    "alertSendDate": datetime.now(timezone(timedelta(hours=9))).replace(tzinfo=None).isoformat()
                                }
                                supabase.table("alertsend").insert(alert_data).execute()
                            
                            # Send control command back to Pico W (backwards compatible with picosim_server.py)
                            client_socket.send(motor_command.encode("utf-8"))
                            add_log(f"제어 명령 전송: {motor_command}")
                        else:
                            # Default response
                            client_socket.send(b"motor off")
                    else:
                        client_socket.send(b"motor off")
                        
                except Exception as db_err:
                    add_log(f"Supabase DB 처리 오류: {db_err}")
                    client_socket.send(b"error")
                    
    except Exception as e:
        add_log(f"클라이언트 통신 에러: {e}")
    finally:
        client_socket.close()
        add_log(f"클라이언트 연결 종료: {client_address}")

def start_tcp_server():
    import sys
    sys.stdout.reconfigure(line_buffering=True)
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind(("0.0.0.0", TCP_PORT))
        server_socket.listen(5)
        add_log(f"❄️ Pico W 온도 데이터 수집 TCP 서버 가동 시작 (포트: {TCP_PORT})")
        
        while True:
            client_socket, client_address = server_socket.accept()
            # Spawns a new concurrent Thread for each connected Pico W / Simulator client
            client_thread = threading.Thread(
                target=handle_client_connection,
                args=(client_socket, client_address),
                daemon=True
            )
            client_thread.start()
            
    except Exception as err:
        add_log(f"TCP 서버 오류: {err}")
    finally:
        server_socket.close()
        add_log("TCP 서버가 종료되었습니다.")

if __name__ == "__main__":
    start_tcp_server()
