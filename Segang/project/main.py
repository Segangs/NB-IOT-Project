#!/usr/bin/env python3
import os
import sys
import time
import threading
import urllib.request
from app import app

def get_interface_ip(interface_name="enp3s0"):
    try:
        import subprocess
        out = subprocess.check_output(["ip", "-4", "addr", "show", interface_name]).decode("utf-8")
        ips = []
        for line in out.splitlines():
            if "inet " in line:
                ip = line.split()[1].split("/")[0]
                # Check if the IP is in private ranges (10.x, 172.16-31.x, 192.168.x, 127.x)
                parts = [int(p) for p in ip.split('.')]
                is_private = (
                    parts[0] == 10 or
                    (parts[0] == 172 and 16 <= parts[1] <= 31) or
                    (parts[0] == 192 and parts[1] == 168) or
                    parts[0] == 127
                )
                if not is_private:
                    return ip
                ips.append(ip)
        if ips:
            return ips[0]
    except Exception as e:
        print(f"❌ [Interface IP] Error getting IP for {interface_name}: {e}")
    return None

def update_duckdns():
    """Update DuckDNS with the server's current public IP address."""
    try:
        ip = get_interface_ip("enp3s0")
        token = "32f6ebb7-1538-4548-8c8d-4fc1b0caed12"
        url = f"https://www.duckdns.org/update?domains=segang&token={token}"
        if ip:
            url += f"&ip={ip}"
            print(f"🔄 [DuckDNS] Updating with explicit IP: {ip}")
        else:
            print("⚠️ [DuckDNS] Could not detect interface IP, updating with default source IP.")
        req = urllib.request.Request(
            url,
            headers={'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'}
        )
        with urllib.request.urlopen(req, timeout=10) as response:
            result = response.read().decode('utf-8')
            if result.strip() == "OK":
                print("🔄 [DuckDNS] IP address updated successfully.")
            else:
                print(f"⚠️ [DuckDNS] Update failed with response: {result}")
    except Exception as e:
        print(f"❌ [DuckDNS] Update error: {e}")

def duckdns_loop():
    """Periodically update DuckDNS every 10 minutes."""
    while True:
        update_duckdns()
        time.sleep(600)

def run_flask_server():
    """Wrapper to run the Flask Web/API server."""
    sys.stdout.reconfigure(line_buffering=True)
    
    # Start DuckDNS automatic sync thread - DISABLED for Cloudflare Tunnel testing
    # print("📡 [DuckDNS] Starting automatic IP synchronization thread...")
    # t = threading.Thread(target=duckdns_loop, daemon=True)
    # t.start()
    
    base_dir = os.path.dirname(os.path.abspath(__file__))
    cert_path = os.path.join(base_dir, "certbot/config/live/segang.duckdns.org/fullchain.pem")
    key_path = os.path.join(base_dir, "certbot/config/live/segang.duckdns.org/privkey.pem")
    
    # Force HTTP mode (port 18180) to support Cloudflare Quick Tunnel forwarding correctly
    print("⚠️ [Flask] Starting server in HTTP mode on port 18180 for Cloudflare Tunnel forwarding...")
    app.run(host="0.0.0.0", port=18180, debug=False, use_reloader=False)

if __name__ == "__main__":
    print("=" * 70)
    print("❄️ PicoTeam Flask 관제 웹 서버 (단일 프로세스 구동)")
    print(" - 구동 시작 시각:", time.strftime("%Y-%m-%d %H:%M:%S"))
    print("=" * 70)
    
    try:
        run_flask_server()
    except KeyboardInterrupt:
        print("\n👋 관제 웹 서버가 종료되었습니다.")
