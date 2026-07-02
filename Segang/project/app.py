import os
import secrets
from datetime import datetime, timedelta, timezone
from flask import Flask, render_template, request, redirect, url_for, session, jsonify, flash
from dotenv import load_dotenv
from supabase import create_client, Client

# Load environment variables
load_dotenv()

SUPABASE_URL = os.environ.get("SUPABASE_URL")
SUPABASE_KEY = os.environ.get("SUPABASE_KEY")
FLASK_SECRET_KEY = os.environ.get("FLASK_SECRET_KEY", secrets.token_hex(24))

# Initialize Supabase client
supabase: Client = create_client(SUPABASE_URL, SUPABASE_KEY)

app = Flask(__name__)
app.secret_key = FLASK_SECRET_KEY
app.config["SEND_FILE_MAX_AGE_DEFAULT"] = timedelta(days=30)

# Global storage for pending desktop logins
pending_logins = {}

# Log file path from tcp_server
LOG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tcp_server.log")

def parse_to_kst(t_str):
    if not t_str:
        return None
    t_clean = t_str.replace("Z", "").replace("T", " ")
    if "." in t_clean:
        t_clean = t_clean.split(".")[0]
    dt = None
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"):
        try:
            dt = datetime.strptime(t_clean, fmt)
            break
        except ValueError:
            continue
    if not dt:
        return None
    return dt

def get_recent_logs(n=10):
    """Fetches recent database events from Supabase in the last 24 hours and formats them as simplified real-time logs."""
    try:
        # Get current time in KST and calculate 24h cutoff
        now_kst = datetime.now(timezone.utc).astimezone(timezone(timedelta(hours=9))).replace(tzinfo=None)
        cutoff_kst = now_kst - timedelta(hours=24)

        # 1. Fetch latest boot logs in the last 24 hours
        boot_res = supabase.table("device_boot_logs")\
            .select("*")\
            .gte("boottime", cutoff_kst.strftime("%Y-%m-%d %H:%M:%S"))\
            .order("id", desc=True)\
            .limit(n)\
            .execute()
        boot_logs = boot_res.data or []
        
        # 2. Fetch latest sensor values in the last 24 hours
        sens_res = supabase.table("sensorvalue")\
            .select("*")\
            .gte("sensorvaluetime", cutoff_kst.strftime("%Y-%m-%dT%H:%M:%S"))\
            .order("sensorValueId", desc=True)\
            .limit(n)\
            .execute()
        sens_logs = sens_res.data or []
        
        # Load sensors to map sensorId -> deviceId
        sensor_res = supabase.table("sensor").select("sensorId, deviceId").execute()
        sensor_map = {s["sensorId"]: s["deviceId"] for s in (sensor_res.data or [])}

        # Load device to map deviceId -> (deviceIMEI, userId)
        dev_res = supabase.table("device").select("deviceId, deviceIMEI, userId").execute()
        dev_map = {d["deviceId"]: (d["deviceIMEI"], d["userId"]) for d in (dev_res.data or [])}

        # Load userMachine to map deviceId -> userMachineId
        um_res = supabase.table("usermachine").select("deviceId, userMachineId").execute()
        um_map = {um["deviceId"]: um["userMachineId"] for um in (um_res.data or [])}

        # Load userSettings to map userMachineId -> tempUpperLimitValue
        us_res = supabase.table("usersettings").select("userMachineId, tempUpperLimitValue").execute()
        us_map = {us["userMachineId"]: float(us["tempUpperLimitValue"] or 30.0) for us in (us_res.data or [])}

        # Load user map
        users_res = supabase.table("users").select("userId, userName").execute()
        user_map = {u["userId"]: u["userName"] for u in (users_res.data or [])}
        
        merged_logs = []
        
        # 3. Format Boot Logs
        for blog in boot_logs:
            btime = blog.get("boottime", "")
            d_id = blog.get("deviceId", 1)
            
            imei = "Unknown IMEI"
            u_id = None
            if d_id in dev_map:
                imei, u_id = dev_map[d_id]
                
            user_name = user_map.get(u_id, "외부인(OAuth)")
            
            is_healthy = (blog.get("flash_integrity") == 0 and blog.get("ram_test") == 0)
            status_str = "정상" if is_healthy else "이상"
            
            dt = parse_to_kst(btime)
            if dt:
                formatted_btime = dt.strftime("%y/%m/%d %H:%M")
                log_line = f"[{formatted_btime}] {user_name} : 서버 접속됨 ({status_str}, IMEI {imei})"
                merged_logs.append((dt, log_line))
            
        # 4. Format Sensor Logs
        for slog in sens_logs:
            stime = slog.get("sensorvaluetime", "")
            sens_id = slog.get("sensorId", 1)
            val = float(slog.get("sensorValue", 0.0))
            
            d_id = sensor_map.get(sens_id)
            imei = "Unknown IMEI"
            u_id = None
            if d_id in dev_map:
                imei, u_id = dev_map[d_id]
                
            user_name = user_map.get(u_id, "외부인(OAuth)")
            
            # Check upper limit
            upper_limit = 30.0 # Default fallback
            if d_id in um_map:
                um_id = um_map[d_id]
                if um_id in us_map:
                    upper_limit = us_map[um_id]
                    
            status_str = "정상" if val <= upper_limit else "이상"
            
            dt = parse_to_kst(stime)
            if dt:
                formatted_stime = dt.strftime("%y/%m/%d %H:%M")
                log_line = f"[{formatted_stime}] {user_name} : 서버 접속됨 ({status_str}, IMEI {imei})"
                merged_logs.append((dt, log_line))
            
        # 5. Sort merged logs by datetime descending
        merged_logs.sort(key=lambda x: x[0], reverse=True)
        
        # Return top n logs
        return [item[1] for item in merged_logs[:n]]
    except Exception as ex:
        return [f"[로그 백엔드 오류]: {ex}"]

# ----------------- Middlewares / Helpers -----------------
def is_logged_in():
    return "user_id" in session or "supabase_token" in session

@app.before_request
def redirect_www():
    host = request.headers.get("Host", "")
    if host.startswith("www.zxcx.io"):
        new_url = request.url.replace("www.zxcx.io", "zxcx.io", 1)
        return redirect(new_url, code=301)

@app.after_request
def add_static_font_cache_headers(response):
    if request.path.startswith("/static/fonts/"):
        response.headers["Cache-Control"] = "public, max-age=31536000, immutable"
    return response

@app.before_request
def check_admin_inactivity():
    if request.endpoint in ('static', 'logout'):
        return

    if session.get("level") == 0:
        last_activity_str = session.get("last_activity")
        if last_activity_str:
            try:
                last_activity = datetime.fromisoformat(last_activity_str)
                if last_activity.tzinfo is None:
                    last_activity = last_activity.replace(tzinfo=timezone.utc)
                
                now = datetime.now(timezone.utc)
                if now - last_activity > timedelta(hours=1):
                    session.clear()
                    is_api_request = request.path.startswith('/api/') or request.headers.get('X-Requested-With') == 'XMLHttpRequest'
                    if is_api_request:
                        return jsonify({"success": False, "error": "관리자 세션이 비활동 상태로 인해 만료되었습니다."}), 401
                    
                    flash("1시간 동안 활동이 없어 세션이 만료되었습니다. 다시 로그인해주세요.", "warning")
                    return redirect(url_for("login"))
            except Exception:
                pass
        
        is_api_request = request.path.startswith('/api/') or request.headers.get('X-Requested-With') == 'XMLHttpRequest'
        if not is_api_request:
            session["last_activity"] = datetime.now(timezone.utc).isoformat()

def get_dynamic_anomalies():
    """
    Computes anomalies in the last 24 hours by matching sensorvalue against usersettings limits.
    Returns a list of calculated anomalies.
    """
    try:
        # Get KST and UTC cutoffs for last 24 hours
        now_kst = datetime.now(timezone.utc).astimezone(timezone(timedelta(hours=9))).replace(tzinfo=None)
        cutoff_kst = now_kst - timedelta(hours=24)

        # Fetch required tables from Supabase, filtering sensorvalue by last 24h (max 500 rows for performance)
        sv_res = supabase.table("sensorvalue")\
            .select("*")\
            .gte("sensorvaluetime", cutoff_kst.strftime("%Y-%m-%dT%H:%M:%S"))\
            .order("sensorValueId", desc=True)\
            .limit(500)\
            .execute()
        s_res = supabase.table("sensor").select("sensorId, deviceId").execute()
        um_res = supabase.table("usermachine").select("deviceId, userMachineId, machineId").execute()
        us_res = supabase.table("usersettings").select("userMachineId, tempUpperLimitValue, tempLowerLimitValue").execute()
        m_res = supabase.table("machine").select("machineId, modelName").execute()
        
        # Load device owners mapping
        d_res = supabase.table("device").select("deviceId, userId").execute()
        u_res = supabase.table("users").select("userId, userName").execute()
        
        sensorvalues = sv_res.data or []
        sensors = s_res.data or []
        usermachines = um_res.data or []
        usersettings = us_res.data or []
        machines = m_res.data or []
        devices = d_res.data or []
        users = u_res.data or []
        
        # 1. Create sensor -> device mapping
        sensor_device = {item["sensorId"]: item["deviceId"] for item in sensors}
        
        # 2. Create device -> userMachineId & machineId mapping
        device_machine = {}
        for item in usermachines:
            device_machine[item["deviceId"]] = {
                "userMachineId": item["userMachineId"],
                "machineId": item["machineId"]
            }
            
        # 3. Create machineId -> modelName mapping
        machine_model = {item["machineId"]: item["modelName"] for item in machines}
        
        # 4. Create userMachineId -> settings limits mapping
        settings_map = {}
        for item in usersettings:
            settings_map[item["userMachineId"]] = {
                "upper": float(item["tempUpperLimitValue"] or 0.0),
                "lower": float(item["tempLowerLimitValue"] or 0.0)
            }
            
        # 5. Create device -> user name mapping
        device_user = {d["deviceId"]: d["userId"] for d in devices}
        user_name_map = {u["userId"]: u["userName"] for u in users}
            
        anomalies = []
        for sv in sensorvalues:
            sensor_id = sv.get("sensorId")
            device_id = sensor_device.get(sensor_id)
            if not device_id:
                continue
                
            dm = device_machine.get(device_id)
            if not dm:
                continue
                
            user_machine_id = dm["userMachineId"]
            machine_id = dm["machineId"]
            
            limits = settings_map.get(user_machine_id)
            if not limits:
                continue
                
            val = float(sv.get("sensorValue") or 0.0)
            upper = limits["upper"]
            lower = limits["lower"]
            
            is_anomaly = False
            msg = ""
            if val > upper:
                is_anomaly = True
                msg = f"상한 임계 온도({upper}°C) 초과! 현재 {val}°C"
                
            if is_anomaly:
                model_name = machine_model.get(machine_id, "알 수 없는 기기")
                t_str = sv.get("sensorvaluetime")
                
                # Fetch device owner name
                u_id = device_user.get(device_id)
                u_name = user_name_map.get(u_id, "외부인(OAuth)")
                
                try:
                    # Parse using parse_to_kst to get the KST datetime
                    dt = parse_to_kst(t_str)
                    if dt:
                        formatted_time = dt.strftime("%H:%M:%S")
                        full_date = dt.strftime("%Y-%m-%d %H:%M:%S")
                    else:
                        formatted_time = t_str
                        full_date = t_str
                except Exception:
                    formatted_time = t_str
                    full_date = t_str
                    
                anomalies.append({
                    "id": sv.get("sensorValueId"),
                    "time": formatted_time,
                    "date": full_date,
                    "user_name": u_name,
                    "device": model_name,
                    "value": val,
                    "message": msg,
                    "upper": upper,
                    "lower": lower,
                    "raw_time": t_str
                })
        return anomalies
    except Exception as err:
        print(f"Error calculating dynamic anomalies: {err}")
        return []

# ----------------- Web Views -----------------
@app.route("/")
def index():
    if is_logged_in():
        return redirect(url_for("dashboard"))
    return render_template("index.html")

@app.route("/dashboard")
def dashboard():
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
    
    user_name = session.get("user_name", "사용자")
    return render_template(
        "dashboard.html", 
        user_name=user_name, 
        supabase_url=SUPABASE_URL, 
        supabase_key=SUPABASE_KEY
    )

# ----------------- Authentication -----------------
@app.route("/auth/login", methods=["GET", "POST"])
def login():
    if is_logged_in():
        return redirect(url_for("dashboard"))
        
    if request.method == "POST":
        account_id = request.form.get("accountId")
        password = request.form.get("password")
        
        try:
            # Query the public.users table
            res = supabase.table("users").select("*").eq("userAccountId", account_id).execute()
            
            if res.data and len(res.data) > 0:
                user = res.data[0]
                
                # Check userActiveStatus
                if user.get("userActiveStatus") != 0:
                    flash("로그인할 수 없습니다. 관리자에게 문의하세요.", "danger")
                    return render_template("login.html")
                
                # Check password
                from werkzeug.security import check_password_hash
                stored_pwd = user.get("userPassword") or ""
                pwd_ok = False
                
                if stored_pwd.startswith("pbkdf2:") or stored_pwd.startswith("scrypt:") or stored_pwd.startswith("argon2:"):
                    pwd_ok = check_password_hash(stored_pwd, password)
                else:
                    pwd_ok = (stored_pwd == password)
                
                if pwd_ok:
                    session["user_id"] = user.get("userId")
                    session["user_name"] = user.get("userName")
                    session["account_id"] = user.get("userAccountId")
                    session["level"] = user.get("level")
                    if user.get("level") == 0:
                        session["last_activity"] = datetime.now(timezone.utc).isoformat()
                    flash(f"{user.get('userName')}님, 환영합니다!", "success")
                    return redirect(url_for("dashboard"))
                else:
                    flash("비밀번호가 올바르지 않습니다.", "danger")
            else:
                flash("존재하지 않는 사용자 아이디입니다.", "danger")
        except Exception as err:
            flash(f"로그인 처리 중 에러 발생: {err}", "danger")
            
    return render_template("login.html")

@app.route("/auth/register", methods=["GET", "POST"])
def register():
    if is_logged_in():
        return redirect(url_for("dashboard"))
        
    if request.method == "POST":
        account_id = request.form.get("accountId")
        password = request.form.get("password")
        name = request.form.get("name")
        phone = request.form.get("phone")
        
        try:
            # Check if username/account already exists
            dup_res = supabase.table("users").select("userId").eq("userAccountId", account_id).execute()
            if dup_res.data and len(dup_res.data) > 0:
                flash("이미 사용 중인 아이디입니다.", "danger")
                return render_template("register.html")
                
            # Get next userId
            max_res = supabase.table("users").select("userId").order("userId", desc=True).limit(1).execute()
            next_id = 1
            if max_res.data:
                next_id = int(max_res.data[0].get("userId") or 0) + 1
                
            from werkzeug.security import generate_password_hash
            hashed_password = generate_password_hash(password)
            
            new_user = {
                "userId": next_id,
                "userAccountId": account_id,
                "userPassword": hashed_password,
                "userName": name,
                "userPhoneNumber": phone,
                "userPaymentStatus": 9,  # Default N/A
                "userCreateDate": datetime.now(timezone(timedelta(hours=9))).replace(tzinfo=None).isoformat(),
                "userActiveStatus": 0,    # Default active (0)
                "level": 1                # Default general member (1)
            }
            
            supabase.table("users").insert(new_user).execute()
            flash("회원가입이 완료되었습니다. 로그인해 주세요!", "success")
            return redirect(url_for("login"))
        except Exception as err:
            flash(f"회원가입 처리 중 에러 발생: {err}", "danger")
            
    return render_template("register.html")

# ----------------- Google OAuth -----------------
@app.route("/auth/google")
def google_login():
    import secrets
    import webbrowser
    
    action = request.args.get("action")
    if action == "link" and is_logged_in():
        session["oauth_action"] = "link"
    else:
        session["oauth_action"] = "login"
        
    # Check if requested from our desktop app (using User-Agent or platform parameter)
    is_desktop = "PicoTeamDesktop" in request.headers.get("User-Agent", "")
    if request.args.get("platform") == "desktop" or session.get("platform") == "desktop":
        is_desktop = True
        
    try:
        if is_desktop:
            # Generate a unique login session ID for this desktop handshake
            login_id = secrets.token_hex(16)
            
            # The callback URL we register on local Flask server
            # We append login_id so Supabase passes it back in the redirect back to localhost
            redirect_url = url_for("auth_callback", login_id=login_id, _external=True)
            if "localhost" not in redirect_url and "127.0.0.1" not in redirect_url:
                redirect_url = redirect_url.replace("http://", "https://")
            
            res = supabase.auth.sign_in_with_oauth({
                "provider": "google",
                "options": {
                    "redirect_to": redirect_url
                }
            })
            
            # Open the Google OAuth URL in the user's default system browser (outside PyWebView)
            # This enables standard OAuth redirects and native OS Passkey validation (WebAuthn)
            webbrowser.open(res.url)
            
            # Render the polling wait screen in the PyWebView window
            return render_template("waiting.html", login_id=login_id)
        else:
            # Standard browser login (direct redirect)
            redirect_url = url_for("auth_callback", _external=True)
            if "localhost" not in redirect_url and "127.0.0.1" not in redirect_url:
                redirect_url = redirect_url.replace("http://", "https://")
                
            res = supabase.auth.sign_in_with_oauth({
                "provider": "google",
                "options": {
                    "redirect_to": redirect_url
                }
            })
            return redirect(res.url)
    except Exception as err:
        flash(f"구글 로그인 호출 실패: {err}", "danger")
        return redirect(url_for("login"))

@app.route("/auth/callback")
def auth_callback():
    # Renders callback.html which will capture URL hash fragments/queries
    return render_template("callback.html")

@app.route("/auth/session", methods=["POST"])
def auth_session():
    import re
    data = request.get_json() or {}
    token_val = data.get("access_token") or data.get("code")
    login_id = data.get("login_id")
    
    if not token_val:
        if login_id:
            pending_logins[login_id] = {"success": False, "error": "No token or code provided"}
            return jsonify({"success": True, "is_desktop": True})
        return jsonify({"success": False, "error": "No token or code provided"}), 400

    user_res = None
    refresh_token = None
    access_token = None

    try:
        # JWT format: three segments separated by '.'
        if re.fullmatch(r"[^\.]+\.[^\.]+\.[^\.]+", token_val):
            access_token = token_val
            user_res = supabase.auth.get_user(access_token)
        else:
            # Exchange authorization code for session
            exchange_res = supabase.auth.exchange_code_for_session({"auth_code": token_val})
            if not exchange_res or not getattr(exchange_res, "session", None):
                raise Exception("Failed to exchange auth code")
            access_token = exchange_res.session.access_token
            refresh_token = exchange_res.session.refresh_token
            user_res = supabase.auth.get_user(access_token)
            
        if not user_res:
            raise Exception("User profile not found")
            
        google_email = user_res.user.email
        
        # Safely parse user_metadata
        metadata = getattr(user_res.user, "user_metadata", {}) or {}
        if isinstance(metadata, str):
            import json
            try:
                metadata = json.loads(metadata)
            except Exception:
                metadata = {}
                
        user_name = metadata.get("full_name") or metadata.get("name") or "OAuth 사용자"
        
        oauth_action = session.get("oauth_action")
        
        if oauth_action == "link":
            # Link Google account to currently logged-in user
            logged_in_uid = session.get("user_id")
            if not logged_in_uid:
                raise Exception("로그인 세션이 만료되었습니다. 다시 로그인해 주세요.")
                
            # Check if this Google email is already linked to another user
            dup = supabase.table("users").select("userId, userName").eq("googleEmail", google_email).execute()
            if dup.data and len(dup.data) > 0 and dup.data[0].get("userId") != logged_in_uid:
                raise Exception(f"해당 구글 계정({google_email})은 이미 다른 회원({dup.data[0].get('userName')})에 연동되어 있습니다.")
                
            # Update public.users
            supabase.table("users").update({"googleEmail": google_email}).eq("userId", logged_in_uid).execute()
            session.pop("oauth_action", None)
            flash("Google 계정이 성공적으로 연동되었습니다.", "success")
            
            if login_id:
                pending_logins[login_id] = {
                    "success": True,
                    "user_id": logged_in_uid,
                    "user_name": session.get("user_name"),
                    "supabase_token": access_token,
                    "refresh_token": refresh_token,
                    "action": "link"
                }
                return jsonify({"success": True, "is_desktop": True, "action": "link"})
            else:
                return jsonify({"success": True, "action": "link"})
                
        else:
            # Login action
            # Query users by googleEmail first, then fall back to userAccountId
            res = supabase.table("users").select("*").eq("googleEmail", google_email).execute()
            if not res.data:
                res = supabase.table("users").select("*").eq("userAccountId", google_email).execute()
                
            if res.data and len(res.data) > 0:
                user = res.data[0]
                
                # Check active status
                if user.get("userActiveStatus") != 0:
                    raise Exception("로그인할 수 없습니다. 관리자에게 문의하세요.")
                    
                # Log them in!
                session["user_id"] = user.get("userId")
                session["user_name"] = user.get("userName")
                session["account_id"] = user.get("userAccountId")
                session["level"] = user.get("level")
                session["supabase_token"] = access_token
                if user.get("level") == 0:
                    session["last_activity"] = datetime.now(timezone.utc).isoformat()
                
                flash(f"{user.get('userName')}님, 환영합니다! (Google 로그인)", "success")
                
                if login_id:
                    pending_logins[login_id] = {
                        "success": True,
                        "user_id": user.get("userId"),
                        "user_name": user.get("userName"),
                        "level": user.get("level"),
                        "account_id": user.get("userAccountId"),
                        "supabase_token": access_token,
                        "refresh_token": refresh_token,
                        "action": "login"
                    }
                    return jsonify({"success": True, "is_desktop": True, "action": "login"})
                else:
                    return jsonify({"success": True, "action": "login"})
            else:
                raise Exception("일반 계정으로 가입되어 있지 않습니다. 먼저 일반 계정으로 가입하고 로그인하여 Google 계정을 연결해 주세요.")
                
    except Exception as err:
        error_msg = str(err)
        flash(error_msg, "danger")
        if login_id:
            pending_logins[login_id] = {
                "success": False,
                "error": error_msg
            }
            return jsonify({"success": True, "is_desktop": True})
        return jsonify({"success": False, "error": error_msg}), 400

@app.route("/auth/poll")
def auth_poll():
    login_id = request.args.get("login_id")
    if not login_id:
        return jsonify({"success": False, "error": "Missing login_id"}), 400
        
    if login_id in pending_logins:
        # Pop the completed login record
        login_data = pending_logins.pop(login_id)
        
        if login_data.get("success") is False:
            return jsonify({"success": False, "error": login_data.get("error")})
            
        # Populate session for this polling window (which runs inside PyWebView)
        session["user_id"] = login_data["user_id"]
        session["user_name"] = login_data["user_name"]
        session["account_id"] = login_data.get("account_id")
        session["level"] = login_data.get("level")
        session["supabase_token"] = login_data["supabase_token"]
        if login_data.get("refresh_token"):
            session["supabase_refresh_token"] = login_data["refresh_token"]
        if login_data.get("level") == 0:
            session["last_activity"] = datetime.now(timezone.utc).isoformat()
            
        return jsonify({"success": True})
        
    return jsonify({"success": False, "status": "pending"})

@app.route("/auth/logout")
def logout():
    session.clear()
    flash("로그아웃되었습니다.", "info")
    return redirect(url_for("index"))

# ----------------- Bulletin Board (게시판 CRUD) -----------------
@app.route("/board")
def board_list():
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
        
    try:
        # Query board postings, order by created_at desc
        res = supabase.table("board").select("*").order("created_at", desc=True).execute()
        posts = res.data or []
        
        # Load user display names
        users_res = supabase.table("users").select("userId, userName").execute()
        user_map = {u["userId"]: u["userName"] for u in (users_res.data or [])}
        
        # Map author names to posts
        for post in posts:
            author_id = post.get("userId")
            post["author_name"] = user_map.get(author_id, "외부인(OAuth)")
            
            # Format datetime
            try:
                dt = datetime.fromisoformat(post.get("created_at").replace("Z", "+00:00"))
                post["formatted_date"] = dt.strftime("%Y-%m-%d %H:%M")
            except:
                post["formatted_date"] = post.get("created_at")
                
    except Exception as err:
        posts = []
        flash(f"게시글 목록을 가져오는 동안 에러 발생: {err}", "danger")
        
    return render_template("board.html", posts=posts, current_user_id=session.get("user_id"))

@app.route("/board/<int:post_id>")
def board_detail(post_id):
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
        
    try:
        # Fetch the specific post
        res = supabase.table("board").select("*").eq("boardId", post_id).limit(1).execute()
        if not res.data:
            flash("게시글을 찾을 수 없습니다.", "warning")
            return redirect(url_for("board_list"))
            
        post = res.data[0]
        
        # Fetch author name
        author_id = post.get("userId")
        user_res = supabase.table("users").select("userName").eq("userId", author_id).limit(1).execute()
        if user_res.data:
            post["author_name"] = user_res.data[0].get("userName", "외부인(OAuth)")
        else:
            post["author_name"] = "외부인(OAuth)"
            
        # Format datetime
        try:
            dt = datetime.fromisoformat(post.get("created_at").replace("Z", "+00:00"))
            post["formatted_date"] = dt.strftime("%Y-%m-%d %H:%M")
        except:
            post["formatted_date"] = post.get("created_at")
            
        return render_template("board_detail.html", post=post, current_user_id=session.get("user_id"))
        
    except Exception as err:
        flash(f"게시글 상세 조회 중 에러 발생: {err}", "danger")
        return redirect(url_for("board_list"))

@app.route("/board/create", methods=["POST"])
def board_create():
    if not is_logged_in():
        return jsonify({"success": False, "error": "Unauthorized"}), 401
        
    title = request.form.get("title")
    content = request.form.get("content")
    
    if not title or not content:
        flash("제목과 내용을 입력해 주세요.", "warning")
        return redirect(url_for("board_list"))
        
    try:
        # Author user ID
        user_id = session.get("user_id")
        if isinstance(user_id, str): # OAuth UUID
            # For OAuth users, we can default author userId to a mockup system user ID (e.g. 1)
            # or dynamically insert a user. Let's use 1 as a fallback.
            user_id = 1
            
        post_data = {
            "title": title,
            "content": content,
            "userId": user_id,
            "created_at": datetime.now(timezone(timedelta(hours=9))).replace(tzinfo=None).isoformat()
        }
        
        supabase.table("board").insert(post_data).execute()
        flash("게시글이 성공적으로 등록되었습니다.", "success")
    except Exception as err:
        flash(f"게시글 등록 중 에러 발생: {err}", "danger")
        
    return redirect(url_for("board_list"))

@app.route("/board/update/<int:post_id>", methods=["POST"])
def board_update(post_id):
    if not is_logged_in():
        return jsonify({"success": False, "error": "Unauthorized"}), 401
        
    title = request.form.get("title")
    content = request.form.get("content")
    
    try:
        update_data = {
            "title": title,
            "content": content
        }
        supabase.table("board").update(update_data).eq("boardId", post_id).execute()
        flash("게시글이 수정되었습니다.", "success")
    except Exception as err:
        flash(f"게시글 수정 중 에러 발생: {err}", "danger")
        
    return redirect(url_for("board_list"))

@app.route("/board/delete/<int:post_id>", methods=["POST"])
def board_delete(post_id):
    if not is_logged_in():
        return jsonify({"success": False, "error": "Unauthorized"}), 401
        
    try:
        supabase.table("board").delete().eq("boardId", post_id).execute()
        flash("게시글이 삭제되었습니다.", "success")
    except Exception as err:
        flash(f"게시글 삭제 중 에러 발생: {err}", "danger")
        
    return redirect(url_for("board_list"))

# ----------------- Devices List -----------------
@app.route("/devices")
def device_list():
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
        
    try:
        res = supabase.table("device").select("*").order("deviceId", desc=False).execute()
        devices = res.data or []
        
        # Load USIM mapping to show usimIMSI
        usim_res = supabase.table("usim").select("usimId, usimIMSI").execute()
        usim_map = {u["usimId"]: u["usimIMSI"] for u in (usim_res.data or [])}
        
        for dev in devices:
            u_id = dev.get("usimId")
            dev["usim_imsi"] = usim_map.get(u_id, "Unknown IMSI")
    except Exception as err:
        devices = []
        flash(f"기기 목록을 가져오는 동안 에러 발생: {err}", "danger")
        
    return render_template("devices.html", devices=devices)

# ----------------- Device Status (기기 상태 상세 로그) -----------------
@app.route("/device-status")
def device_status():
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
        
    try:
        # Fetch latest boot logs
        boot_res = supabase.table("device_boot_logs").select("*").order("id", desc=True).limit(100).execute()
        boot_logs = boot_res.data or []
        
        # Fetch device and user maps
        dev_res = supabase.table("device").select("deviceId, deviceIMEI").execute()
        dev_map = {d["deviceId"]: d["deviceIMEI"] for d in (dev_res.data or [])}
        
        users_res = supabase.table("users").select("userId, userName").execute()
        user_map = {u["userId"]: u["userName"] for u in (users_res.data or [])}
        
        # Collect cmdIds for batch query
        cmd_ids = []
        for log in boot_logs:
            reason = log.get("bootReasonCode")
            cmd_id = log.get("cmdId")
            if reason == 1 and cmd_id:
                cmd_ids.append(cmd_id)
                
        cmd_time_map = {}
        if cmd_ids:
            try:
                cmd_res = supabase.table("deviceCmds").select("cmdId, created_at").in_("cmdId", cmd_ids).execute()
                for cmd_row in (cmd_res.data or []):
                    c_id = cmd_row.get("cmdId")
                    c_at = cmd_row.get("created_at")
                    if c_at:
                        try:
                            # Format creation time
                            dt = datetime.fromisoformat(c_at.replace("Z", "+00:00"))
                            formatted_c_at = dt.strftime("%m/%d %H:%M")
                        except:
                            formatted_c_at = c_at
                        cmd_time_map[c_id] = formatted_c_at
            except Exception as e:
                print(f"Error fetching deviceCmds: {e}")

        formatted_logs = []
        for log in boot_logs:
            d_id = log.get("deviceId")
            u_id = log.get("userId")
            c_id = log.get("cmdId")
            reason_code = log.get("bootReasonCode")
            
            # Format time
            btime = log.get("boottime", "")
            try:
                dt = datetime.fromisoformat(btime.replace("Z", "+00:00"))
                formatted_time = dt.strftime("%m/%d %H:%M")
            except:
                formatted_time = btime
                
            cmd_submitted_time = cmd_time_map.get(c_id) if c_id else None
                
            formatted_logs.append({
                "id": log.get("id"),
                "device_id": d_id,
                "time": formatted_time,
                "user_name": user_map.get(u_id, "외부인(OAuth)"),
                "imei": dev_map.get(d_id, "Unknown IMEI"),
                "pico_voltage": log.get("pico_voltage", 0.0),
                "temperature": log.get("temperature", 0.0),
                "flash_integrity": "정상" if log.get("flash_integrity") == 0 else "이상",
                "ram_test": "정상" if log.get("ram_test") == 0 else "이상",
                "at_status": "정상(OK)" if log.get("at_status") == 0 else "이상",
                "cpin_status": "정상(READY)" if log.get("cpin_status") == 0 else "이상",
                "csq_rssi": log.get("csq_rssi", 99),
                "temp_sensor_status": "정상" if log.get("temp_sensor_status") == 0 else "이상",
                "boot_reason_code": reason_code,
                "cmd_id": c_id,
                "cmd_submitted_time": cmd_submitted_time
            })
            
    except Exception as err:
        formatted_logs = []
        flash(f"기기 상태 로그를 가져오는 동안 에러 발생: {err}", "danger")
        
    return render_template("device_status.html", logs=formatted_logs)

# ----------------- Send Control Command to DeviceCmds -----------------
@app.route("/send-command", methods=["POST"])
def send_command():
    if not is_logged_in():
        return jsonify({"success": False, "error": "로그인이 필요합니다."}), 401
        
    try:
        data = request.get_json() or {}
        device_id = data.get("deviceId")
        cmd_code = data.get("cmd")
        
        if not device_id or cmd_code is None:
            return jsonify({"success": False, "error": "잘못된 요청 파라미터입니다."}), 400
            
        insert_data = {
            "deviceId": int(device_id),
            "cmd": int(cmd_code),
            "status": 0
        }
        
        res = supabase.table("deviceCmds").insert(insert_data).execute()
        if not res.data:
            return jsonify({"success": False, "error": "명령 전송에 실패했습니다."}), 500
            
        return jsonify({"success": True, "message": "명령 전송 완료 (대기 중)"})
        
    except Exception as e:
        return jsonify({"success": False, "error": str(e)}), 500

# ----------------- Temperature Status (온도 상태 상세 비교) -----------------
@app.route("/temp-status")
def temp_status():
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
        
    try:
        # Fetch sensor values
        sv_res = supabase.table("sensorvalue").select("*").order("sensorValueId", desc=True).limit(150).execute()
        sensor_values = sv_res.data or []
        
        # Load mappings
        s_res = supabase.table("sensor").select("sensorId, deviceId").execute()
        sensor_device = {s["sensorId"]: s["deviceId"] for s in (s_res.data or [])}
        
        dev_res = supabase.table("device").select("deviceId, userId, deviceIMEI").execute()
        dev_map = {d["deviceId"]: (d["deviceIMEI"], d["userId"]) for d in (dev_res.data or [])}
        
        um_res = supabase.table("usermachine").select("deviceId, userMachineId, machineId").execute()
        device_machine = {}
        for item in (um_res.data or []):
            device_machine[item["deviceId"]] = {
                "userMachineId": item["userMachineId"],
                "machineId": item["machineId"]
            }
            
        us_res = supabase.table("usersettings").select("userMachineId, tempUpperLimitValue").execute()
        settings_map = {item["userMachineId"]: float(item["tempUpperLimitValue"] or 30.0) for item in (us_res.data or [])}
        
        users_res = supabase.table("users").select("userId, userName").execute()
        user_map = {u["userId"]: u["userName"] for u in (users_res.data or [])}
        
        m_res = supabase.table("machine").select("machineId, modelName").execute()
        machine_model = {item["machineId"]: item["modelName"] for item in (m_res.data or [])}
        
        formatted_temps = []
        for sv in sensor_values:
            sens_id = sv.get("sensorId")
            val = float(sv.get("sensorValue") or 0.0)
            
            d_id = sensor_device.get(sens_id)
            imei = "Unknown IMEI"
            u_id = None
            model_name = "알 수 없는 기기"
            upper_limit = 30.0
            
            if d_id:
                if d_id in dev_map:
                    imei, u_id = dev_map[d_id]
                dm = device_machine.get(d_id)
                if dm:
                    model_name = machine_model.get(dm["machineId"], "알 수 없는 기기")
                    upper_limit = settings_map.get(dm["userMachineId"], 30.0)
                    
            user_name = user_map.get(u_id, "외부인(OAuth)")
            
            # Format time
            stime = sv.get("sensorvaluetime", "")
            try:
                dt = datetime.fromisoformat(stime.replace("Z", "+00:00"))
                formatted_time = dt.strftime("%y/%m/%d") + "<br>" + dt.strftime("%H:%M:%S")
            except:
                formatted_time = stime
                
            status_ok = val <= upper_limit
            
            formatted_temps.append({
                "id": sv.get("sensorValueId"),
                "time": formatted_time,
                "user_name": user_name,
                "model_name": model_name,
                "imei": imei,
                "value": val,
                "upper_limit": upper_limit,
                "status_ok": status_ok,
                "device_id": d_id
            })
            
    except Exception as err:
        formatted_temps = []
        flash(f"온도 상태 로그를 가져오는 동안 에러 발생: {err}", "danger")
        
    return render_template("temp_status.html", temps=formatted_temps)

# ----------------- Device Temperature History (기기별 온도 추이) -----------------
@app.route("/device-temp-history/<int:device_id>")
def device_temp_history(device_id):
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
        
    try:
        # 1. Fetch device data
        dev_res = supabase.table("device").select("deviceId, userId, deviceIMEI, userWorkplaceId").eq("deviceId", device_id).execute()
        if not dev_res.data:
            flash("해당 기기를 찾을 수 없습니다.", "danger")
            return redirect(url_for("temp_status"))
        dev = dev_res.data[0]
        imei = dev.get("deviceIMEI", "Unknown IMEI")
        u_id = dev.get("userId")
        current_workplace_id = dev.get("userWorkplaceId")
        
        # 2. Fetch owner userName
        user_name = "외부인(OAuth)"
        if u_id:
            u_res = supabase.table("users").select("userName").eq("userId", u_id).execute()
            if u_res.data:
                user_name = u_res.data[0].get("userName", "외부인(OAuth)")
                
        # 3. Fetch workplaces and user machines for dropdown lists
        workplaces = []
        user_machines_list = []
        if u_id:
            # Fetch workplaces belonging to u_id (include WorkplaceName)
            wp_res = supabase.table("userworkplace").select("userWorkplaceId, WorkplaceAddress, WorkplaceName").eq("userId", u_id).execute()
            workplaces = wp_res.data or []
            
            # Fetch devices belonging to u_id
            devs_res = supabase.table("device").select("deviceId, userWorkplaceId").eq("userId", u_id).execute()
            devices = devs_res.data or []
            dev_ids = [d["deviceId"] for d in devices]
            
            if dev_ids:
                # Fetch usermachine records linked to user's devices
                um_all_res = supabase.table("usermachine").select("userMachineId, deviceId, userMachineName").in_("deviceId", dev_ids).execute()
                um_all = um_all_res.data or []
                
                workplace_map = {w["userWorkplaceId"]: w["WorkplaceAddress"] for w in workplaces}
                workplace_name_map = {w["userWorkplaceId"]: (w.get("WorkplaceName") or "알 수 없는 영업장") for w in workplaces}
                dev_workplace_map = {d["deviceId"]: d["userWorkplaceId"] for d in devices}
                
                for um in um_all:
                    d_id = um.get("deviceId")
                    w_id = dev_workplace_map.get(d_id)
                    w_addr = workplace_map.get(w_id, "알 수 없는 영업장")
                    w_name = workplace_name_map.get(w_id, "알 수 없는 영업장")
                    user_machines_list.append({
                        "userMachineId": um.get("userMachineId"),
                        "deviceId": d_id,
                        "userMachineName": um.get("userMachineName") or "알 수 없는 기기",
                        "userWorkplaceId": w_id,
                        "workplaceAddress": w_addr,
                        "workplaceName": w_name
                    })
        
        # Determine current workplace Info
        workplace_map = {w["userWorkplaceId"]: w["WorkplaceAddress"] for w in workplaces}
        workplace_name_map = {w["userWorkplaceId"]: (w.get("WorkplaceName") or "알 수 없는 영업장") for w in workplaces}
        current_workplace_address = workplace_map.get(current_workplace_id, "알 수 없는 영업장")
        current_workplace_name = workplace_name_map.get(current_workplace_id, "알 수 없는 영업장")
                
        # 4. Fetch modelName, userMachineId, and userMachineName
        model_name = "알 수 없는 기기"
        user_machine_id = None
        user_machine_name = "알 수 없는 기기"
        um_res = supabase.table("usermachine").select("userMachineId, machineId, userMachineName").eq("deviceId", device_id).execute()
        if um_res.data:
            um = um_res.data[0]
            user_machine_id = um.get("userMachineId")
            user_machine_name = um.get("userMachineName") or "알 수 없는 기기"
            m_res = supabase.table("machine").select("modelName").eq("machineId", um.get("machineId")).execute()
            if m_res.data:
                model_name = m_res.data[0].get("modelName", "알 수 없는 기기")
                
        # 5. Fetch upper limit value
        upper_limit = 30.0
        if user_machine_id:
            us_res = supabase.table("usersettings").select("tempUpperLimitValue").eq("userMachineId", user_machine_id).execute()
            if us_res.data:
                upper_limit = float(us_res.data[0].get("tempUpperLimitValue") or 30.0)
                
        # 6. Fetch sensors for this device
        s_res = supabase.table("sensor").select("sensorId").eq("deviceId", device_id).execute()
        sensor_ids = [s["sensorId"] for s in (s_res.data or [])]
        
        # 7. Fetch last 50 sensor values
        formatted_history = []
        chart_labels = []
        chart_values = []
        
        if sensor_ids:
            sv_res = supabase.table("sensorvalue").select("*").in_("sensorId", sensor_ids).order("sensorValueId", desc=True).limit(50).execute()
            sensor_values = sv_res.data or []
            
            for sv in sensor_values:
                val = float(sv.get("sensorValue") or 0.0)
                stime = sv.get("sensorvaluetime", "")
                try:
                    dt = datetime.fromisoformat(stime.replace("Z", "+00:00"))
                    formatted_time = dt.strftime("%Y-%m-%d %H:%M:%S")
                    chart_time = dt.strftime("%m-%d %H:%M")
                except:
                    formatted_time = stime
                    chart_time = stime
                    
                formatted_history.append({
                    "id": sv.get("sensorValueId"),
                    "time": formatted_time,
                    "value": val,
                    "status_ok": val <= upper_limit
                })
                
                # Append for chart (we'll reverse this later)
                chart_labels.append(chart_time)
                chart_values.append(val)
                
            # Reverse chart lists to chronological order (past to present)
            chart_labels.reverse()
            chart_values.reverse()
            
        device_info = {
            "device_id": device_id,
            "imei": imei,
            "user_name": user_name,
            "model_name": model_name,
            "upper_limit": upper_limit,
            "workplace_id": current_workplace_id,
            "workplace_address": current_workplace_address,
            "workplace_name": current_workplace_name,
            "machine_name": user_machine_name
        }
        
    except Exception as err:
        flash(f"온도 추이 데이터를 가져오는 동안 에러 발생: {err}", "danger")
        return redirect(url_for("temp_status"))
        
    return render_template(
        "device_temp_history.html", 
        device=device_info, 
        history=formatted_history,
        chart_labels=chart_labels,
        chart_values=chart_values,
        workplaces=workplaces,
        user_machines=user_machines_list
    )

# ----------------- National Temperatures (전국 평균 온도 상세) -----------------
@app.route("/national-temperatures")
def national_temperatures():
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
    return render_template("national_temperatures.html")

@app.route("/api/national-temperatures")
def api_national_temperatures():
    if not is_logged_in():
        return jsonify({"error": "Unauthorized"}), 401
        
    try:
        # Fetch required tables from Supabase to join details in memory
        sv_res = supabase.table("sensorvalue").select("*").order("sensorValueId", desc=True).limit(80).execute()
        s_res = supabase.table("sensor").select("sensorId, deviceId").execute()
        d_res = supabase.table("device").select("deviceId, userId").execute()
        um_res = supabase.table("usermachine").select("deviceId, machineId").execute()
        m_res = supabase.table("machine").select("machineId, systemType").execute()
        u_res = supabase.table("users").select("userId, userName").execute()
        
        sensorvalues = sv_res.data or []
        sensors = s_res.data or []
        devices = d_res.data or []
        usermachines = um_res.data or []
        machines = m_res.data or []
        users = u_res.data or []
        
        # 1. Create sensor -> device mapping
        sensor_device = {item["sensorId"]: item["deviceId"] for item in sensors}
        
        # 2. Create device -> user mapping
        device_user = {item["deviceId"]: item["userId"] for item in devices}
        user_name_map = {item["userId"]: item["userName"] for item in users}
        
        # 3. Create device -> machine mapping
        device_machine = {}
        for item in usermachines:
            device_machine[item["deviceId"]] = item["machineId"]
            
        machine_system = {item["machineId"]: item["systemType"] for item in machines}
        
        # Build joined list of details
        details = []
        for sv in sensorvalues:
            sensor_id = sv.get("sensorId")
            device_id = sensor_device.get(sensor_id)
            
            # User Name lookup
            user_id = device_user.get(device_id) if device_id else None
            user_name = user_name_map.get(user_id, "외부인(OAuth)") if user_id else "알 수 없음"
            
            # System Type lookup
            machine_id = device_machine.get(device_id) if device_id else None
            system_type = machine_system.get(machine_id, "일반") if machine_id else "일반"
            
            t_str = sv.get("sensorvaluetime")
            try:
                dt = datetime.fromisoformat(t_str.replace("Z", "+00:00"))
                formatted_time = dt.strftime("%Y-%m-%d %H:%M:%S")
            except Exception:
                formatted_time = t_str
                
            details.append({
                "sensorValueId": sv.get("sensorValueId"),
                "userName": user_name,
                "systemType": system_type,
                "sensorValue": float(sv.get("sensorValue") or 0.0),
                "sensorValueTime": formatted_time
            })
            
        return jsonify({
            "success": True,
            "data": details
        })
    except Exception as err:
        return jsonify({"success": False, "error": str(err)}), 500

# ----------------- Dashboard Real-Time API -----------------
@app.route("/api/status")
def api_status():
    if not is_logged_in():
        return jsonify({"error": "Unauthorized"}), 401
        
    try:
        # 1. Fetch reference time (latest event time)
        latest_boot = supabase.table("device_boot_logs").select("boottime").order("id", desc=True).limit(1).execute()
        latest_sens = supabase.table("sensorvalue").select("sensorvaluetime").order("sensorValueId", desc=True).limit(1).execute()
        
        ref_time = datetime.now(timezone(timedelta(hours=9))).replace(tzinfo=None)
        t_candidates = []
        if latest_boot.data:
            t_candidates.append(parse_to_kst(latest_boot.data[0]["boottime"]))
        if latest_sens.data:
            t_candidates.append(parse_to_kst(latest_sens.data[0]["sensorvaluetime"]))
        if t_candidates:
            ref_time = max(t for t in t_candidates if t is not None)
            
        cutoff_time = ref_time - timedelta(hours=12)
        
        # 2. Get active devices in last 12h
        boot_res = supabase.table("device_boot_logs").select("*").gte("boottime", cutoff_time.strftime("%Y-%m-%d %H:%M:%S")).execute()
        boot_data = boot_res.data or []
        
        sens_cutoff = cutoff_time
        sens_res = supabase.table("sensorvalue").select("*").gte("sensorvaluetime", sens_cutoff.strftime("%Y-%m-%dT%H:%M:%S")).execute()
        sens_data = sens_res.data or []
        
        sensor_res = supabase.table("sensor").select("sensorId, deviceId").execute()
        sensor_map = {s["sensorId"]: s["deviceId"] for s in (sensor_res.data or [])}
        
        active_device_ids = set()
        for b in boot_data:
            active_device_ids.add(b["deviceId"])
        for s in sens_data:
            d_id = sensor_map.get(s["sensorId"])
            if d_id:
                active_device_ids.add(d_id)
                
        all_dev_res = supabase.table("device").select("deviceId").execute()
        all_devices = all_dev_res.data or []
        total_devices = len(all_devices)
        
        # If no active devices, fallback to all devices in DB
        target_device_ids = active_device_ids
        if not target_device_ids:
            target_device_ids = {d["deviceId"] for d in all_devices}
            
        # 3. Calculate health metrics
        device_scores = []
        sensor_scores = []
        conn_scores = []
        
        for d_id in target_device_ids:
            dev_boots = [b for b in boot_data if b["deviceId"] == d_id]
            blog = None
            if dev_boots:
                dev_boots.sort(key=lambda x: parse_to_kst(x["boottime"]), reverse=True)
                blog = dev_boots[0]
            else:
                fallback_res = supabase.table("device_boot_logs").select("*").eq("deviceId", d_id).order("id", desc=True).limit(1).execute()
                if fallback_res.data:
                    blog = fallback_res.data[0]
                    
            if blog:
                pico_v = float(blog.get("pico_voltage") or 0.0)
                temp = float(blog.get("temperature") or 0.0)
                flash = blog.get("flash_integrity")
                ram = blog.get("ram_test")
                
                d_score = 0
                if pico_v >= 3.1:
                    d_score += 25
                elif pico_v >= 2.8:
                    d_score += 12.5
                if temp <= 50.0: d_score += 25
                if flash == 0: d_score += 25
                if ram == 0: d_score += 25
                device_scores.append(d_score)
                
                s_status = blog.get("temp_sensor_status")
                s_score = 100 if s_status == 0 else 0
                sensor_scores.append(s_score)
                
                at = blog.get("at_status")
                cpin = blog.get("cpin_status")
                csq = blog.get("csq_rssi", 99)
                
                c_score = 0
                if at == 0: c_score += 33
                if cpin == 0: c_score += 33
                if csq != 99 and csq >= 0:
                    csq_pct = min(34.0, (csq / 31.0) * 34.0)
                    c_score += csq_pct
                conn_scores.append(c_score)
            else:
                device_scores.append(100)
                sensor_scores.append(100)
                conn_scores.append(100)
                
        device_health = round(sum(device_scores) / len(device_scores), 1) if device_scores else 100.0
        sensor_health = round(sum(sensor_scores) / len(sensor_scores), 1) if sensor_scores else 100.0
        comm_health = round(sum(conn_scores) / len(conn_scores), 1) if conn_scores else 100.0
        
        # 4. Hourly normal operation rate trend (last 12h) using carry-forward
        um_res = supabase.table("usermachine").select("deviceId, userMachineId").execute()
        um_map = {um["deviceId"]: um["userMachineId"] for um in (um_res.data or [])}
        
        us_res = supabase.table("usersettings").select("userMachineId, tempUpperLimitValue").execute()
        us_map = {us["userMachineId"]: float(us["tempUpperLimitValue"] or 30.0) for us in (us_res.data or [])}
        
        device_limits = {}
        for d_id in target_device_ids:
            um_id = um_map.get(d_id)
            limit = us_map.get(um_id, 30.0) if um_id else 30.0
            device_limits[d_id] = limit

        all_sv_res = supabase.table("sensorvalue").select("*").execute()
        all_sv = all_sv_res.data or []
        
        chart_labels = []
        chart_values = []
        
        for i in range(12):
            b_end = cutoff_time + timedelta(hours=i+1)
            
            normal_count = 0
            total_count = 0
            
            for d_id, limit in device_limits.items():
                dev_sensors = []
                for sv in all_sv:
                    s_id = sv["sensorId"]
                    s_dev_id = sensor_map.get(s_id)
                    if s_dev_id == d_id:
                        sv_time = parse_to_kst(sv["sensorvaluetime"])
                        if sv_time and sv_time <= b_end:
                            dev_sensors.append((sv_time, float(sv["sensorValue"] or 0.0)))
                
                if dev_sensors:
                    dev_sensors.sort(key=lambda x: x[0], reverse=True)
                    latest_temp = dev_sensors[0][1]
                    
                    total_count += 1
                    if latest_temp <= limit:
                        normal_count += 1
                        
            rate = round((normal_count / total_count) * 100.0, 1) if total_count > 0 else 100.0
            chart_labels.append(b_end.strftime("%H:%M"))
            chart_values.append(rate)
            
        # 5. Fallback/Standard dashboard returns
        val_res = supabase.table("sensorvalue").select("sensorValue").order("sensorValueId", desc=True).limit(50).execute()
        vals = val_res.data or []
        if vals:
            avg_temp = round(sum(float(v["sensorValue"]) for v in vals) / len(vals), 1)
        else:
            avg_temp = 3.5
            
        alerts = get_dynamic_anomalies()
        logs = get_recent_logs(8)
        
        return jsonify({
            "success": True,
            "avg_temp": avg_temp,
            "total_devices": total_devices,
            "active_devices": len(active_device_ids),
            "device_health": device_health,
            "sensor_health": sensor_health,
            "comm_health": comm_health,
            "alerts": alerts,
            "logs": logs,
            "chart_labels": chart_labels,
            "chart_values": chart_values
        })
    except Exception as err:
        return jsonify({"success": False, "error": str(err)}), 500

@app.route("/auth/edit-profile", methods=["GET", "POST"])
def edit_profile():
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
        
    user_id = session.get("user_id")
    
    if request.method == "POST":
        phone = request.form.get("phone")
        new_password = request.form.get("new_password")
        confirm_password = request.form.get("confirm_password")
        
        try:
            update_data = {}
            if phone is not None:
                update_data["userPhoneNumber"] = phone
                
            if new_password:
                if new_password != confirm_password:
                    flash("새 비밀번호와 비밀번호 확인이 일치하지 않습니다.", "danger")
                    return redirect(url_for("edit_profile"))
                from werkzeug.security import generate_password_hash
                update_data["userPassword"] = generate_password_hash(new_password)
                
            if update_data:
                supabase.table("users").update(update_data).eq("userId", user_id).execute()
                flash("회원정보가 성공적으로 수정되었습니다.", "success")
                
            return redirect(url_for("edit_profile"))
        except Exception as err:
            flash(f"회원정보 수정 중 오류 발생: {err}", "danger")
            return redirect(url_for("edit_profile"))
            
    # GET request
    try:
        res = supabase.table("users").select("*").eq("userId", user_id).limit(1).execute()
        if not res.data:
            flash("사용자 정보를 찾을 수 없습니다.", "danger")
            return redirect(url_for("dashboard"))
            
        user = res.data[0]
        
        # Format the create date if present
        if user.get("userCreateDate"):
            dt = parse_to_kst(user.get("userCreateDate"))
            if dt:
                user["userCreateDate"] = dt.strftime("%Y-%m-%d %H:%M:%S")
                
        return render_template("edit_profile.html", user=user)
    except Exception as err:
        flash(f"사용자 정보를 가져오는 도중 오류 발생: {err}", "danger")
        return redirect(url_for("dashboard"))

@app.route("/admin/members")
def admin_members():
    if not is_logged_in():
        flash("로그인이 필요한 서비스입니다.", "warning")
        return redirect(url_for("login"))
        
    if session.get("level") != 0:
        flash("접근 권한이 없습니다. 관리자만 접근 가능합니다.", "danger")
        return redirect(url_for("dashboard"))
        
    try:
        res = supabase.table("users").select("*").order("userId", desc=False).execute()
        members = res.data or []
        
        for m in members:
            if m.get("userCreateDate"):
                dt = parse_to_kst(m.get("userCreateDate"))
                if dt:
                    m["formatted_create_date"] = dt.strftime("%Y-%m-%d %H:%M")
                    
        return render_template("admin_members.html", members=members)
    except Exception as err:
        flash(f"회원 정보를 가져오는 도중 오류 발생: {err}", "danger")
        return redirect(url_for("dashboard"))

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080, debug=True)
