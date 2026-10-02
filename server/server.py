import socket
import requests
import face_recognition
import cv2
import numpy as np
import os
import time
import sqlite3
import threading
from datetime import datetime
from flask import Flask, render_template, request, redirect, url_for, send_from_directory

# ===========================
# 1. 系統設定
# ===========================
HOST = '0.0.0.0'
PORT = 8080
ESP32_CAM_URL = os.environ.get("ESP32_CAM_URL", "http://192.168.0.100/capture")  # 改成 ESP32-CAM 的 IP
CAPTURE_DIR = "captures"
USER_DIR = "users"
DB_NAME = "database.db"

app = Flask(__name__)

# 全域變數：快取人臉特徵，避免每次辨識都要讀檔
known_faces_encodings = {}
known_faces_names = {} # UID 對應 姓名

# ===========================
# 2. 資料庫與工具函式
# ===========================
def check_camera_startup():
    """系統啟動時專用的相機檢查"""
    print("------------------------------------------------")
    print(f"📡 正在進行開機自我檢查...")
    print(f"   目標相機 IP: {ESP32_CAM_URL}")
    
    try:
        # 設定 3 秒超時，只測試連線，不下載圖片
        response = requests.get(ESP32_CAM_URL, timeout=3)
        
        if response.status_code == 200:
            print("✅ 相機連線成功！(HTTP 200)")
            print("------------------------------------------------")
            return True
        else:
            print(f"⚠️ 相機連線異常 (狀態碼: {response.status_code})")
            print("------------------------------------------------")
            return False
            
    except requests.exceptions.ConnectionError:
        print("❌ 嚴重錯誤：找不到 ESP32！")
        print("   -> 請檢查 ESP32 是否有插電？")
        print("   -> 請檢查 ESP32 的 IP 是否變了？")
        print("------------------------------------------------")
        return False
    except Exception as e:
        print(f"❌ 檢查過程發生錯誤: {e}")
        print("------------------------------------------------")
        return False
def init_db():
    """初始化資料庫"""
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    # 建立使用者表
    c.execute('''CREATE TABLE IF NOT EXISTS users 
                 (uid TEXT PRIMARY KEY, name TEXT, image_path TEXT)''')
    # 建立紀錄表
    c.execute('''CREATE TABLE IF NOT EXISTS logs 
                 (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp TEXT, uid TEXT, name TEXT, status TEXT, photo_path TEXT)''')
    conn.commit()
    conn.close()

def log_entry(uid, name, status, photo_path):
    """寫入刷卡紀錄"""
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    c.execute("INSERT INTO logs (timestamp, uid, name, status, photo_path) VALUES (?, ?, ?, ?, ?)",
              (timestamp, uid, name, status, photo_path))
    conn.commit()
    conn.close()

def load_image_safe_cv2(file_path):
    """(之前的安全讀取函式)"""
    try:
        # 這裡需要處理中文路徑問題，先用 cv2.imdecode
        stream = open(file_path, "rb")
        bytes = bytearray(stream.read())
        numpyarray = np.asarray(bytes, dtype=np.uint8)
        img = cv2.imdecode(numpyarray, cv2.IMREAD_UNCHANGED)
        
        if img is None: return None
        return cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    except Exception as e:
        print(f"❌ 讀圖失敗: {e}")
        return None

def reload_users():
    """從資料庫重新載入使用者特徵到記憶體"""
    global known_faces_encodings, known_faces_names
    print("🔄 正在重新載入使用者資料庫...")
    
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    c.execute("SELECT uid, name, image_path FROM users")
    users = c.fetchall()
    conn.close()

    known_faces_encodings = {}
    known_faces_names = {}

    if not os.path.exists(USER_DIR): os.makedirs(USER_DIR)

    for uid, name, image_path in users:
        filepath = os.path.join(USER_DIR, image_path)
        rgb_img = load_image_safe_cv2(filepath)
        
        if rgb_img is not None:
            try:
                encodings = face_recognition.face_encodings(rgb_img)
                if len(encodings) > 0:
                    known_faces_encodings[uid] = encodings[0]
                    known_faces_names[uid] = name
                    print(f"  ✅ 已載入: {name} ({uid})")
            except Exception as e:
                print(f"  ❌ 編碼失敗 {name}: {e}")
    print("🎉 使用者資料重載完成。")

def get_photo_from_cam(filename):
    """從 ESP32 拍照 (含連線檢查)"""
    try:
        print(f"📡 正在檢查相機連線 ({ESP32_CAM_URL})... ", end="")
        
        # 設定 timeout=3 秒，如果 3 秒沒反應就當作沒連上
        response = requests.get(ESP32_CAM_URL, timeout=3)
        
        if response.status_code == 200:
            print("✅ 連線正常，下載照片中...")
            with open(filename, 'wb') as f:
                f.write(response.content)
            return True
        else:
            print(f"❌ 連線成功但狀態碼錯誤 (HTTP {response.status_code})")
            return False
    except requests.exceptions.ConnectionError:
        print("❌ 失敗：無法連上 ESP32 (請確認 ESP32 是否有電或 IP 正確)")
        return False
    except requests.exceptions.Timeout:
        print("❌ 失敗：連線逾時 (ESP32 反應太慢)")
        return False
    except Exception as e:
        print(f"❌ 未知錯誤: {e}")
        return False

# ===========================
# 3. 核心邏輯：Socket Server (跑在獨立執行緒)
# ===========================
def rfid_server_thread():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind((HOST, PORT))
        server_socket.listen(5)
        print(f"🚀 RFID Server 啟動於 Port {PORT}")
    except Exception as e:
        print(f"❌ Socket 啟動失敗: {e}")
        return

    if not os.path.exists(CAPTURE_DIR): os.makedirs(CAPTURE_DIR)

    while True:
        try:
            client_socket, addr = server_socket.accept()
            client_socket.settimeout(10.0)
            data = client_socket.recv(1024)
            
            if data:
                rfid_uid = data.decode('utf-8').strip()
                print(f"\n🔔 收到卡號: {rfid_uid}")
                
                # 產生檔名
                ts = datetime.now().strftime("%Y%m%d-%H%M%S")
                safe_uid = rfid_uid.replace(" ", "")
                filename = f"{ts}_{safe_uid}.jpg"
                save_path = os.path.join(CAPTURE_DIR, filename)

                # ==========================================
                # ★★★ 這裡就是你要的「先看有沒有連上相機」 ★★★
                # ==========================================
                if get_photo_from_cam(save_path):
                    # --- 相機連線成功，才開始辨識 ---
                    status = "FAIL"
                    name = "未知訪客"
                    
                    if rfid_uid in known_faces_encodings:
                        name = known_faces_names[rfid_uid]
                        target_encoding = known_faces_encodings[rfid_uid]
                        
                        unknown_img = load_image_safe_cv2(save_path)
                        if unknown_img is not None:
                            unknown_encs = face_recognition.face_encodings(unknown_img)
                            if len(unknown_encs) > 0:
                                matches = face_recognition.compare_faces([target_encoding], unknown_encs[0], tolerance=0.5)
                                if matches[0]:
                                    status = "PASS"
                                    print(f"✅ {name} 驗證通過！")
                                    # Python 送出 "PASS" 給 ESP32，ESP32 收到後會亮燈
                                    client_socket.send(b"PASS") 
                                else:
                                    print(f"🙅‍♂️ {name} 臉部不符")
                                    client_socket.send(b"FAIL")
                            else:
                                print("⚠️ 沒看到臉")
                                client_socket.send(b"FAIL")
                        else:
                            client_socket.send(b"ERROR")
                    else:
                        print("⛔ 卡號未註冊")
                        client_socket.send(b"FAIL")
                    
                    log_entry(rfid_uid, name, status, filename)

                else:
                    # --- 相機連不上 ---
                    print("⛔ 放棄辨識：因為相機沒連上。")
                    # 回傳 ERROR 讓 ESP32 知道出事了
                    client_socket.send(b"ERROR")

            client_socket.close()
        except Exception as e:
            print(f"⚠️ Socket 錯誤: {e}")

# ===========================
# 4. 網頁伺服器：Flask Routes
# ===========================

@app.route('/')
def index():
    """首頁：顯示紀錄"""
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    c.execute("SELECT timestamp, uid, name, status, photo_path FROM logs ORDER BY id DESC LIMIT 50")
    logs = [{'timestamp': r[0], 'uid': r[1], 'name': r[2], 'status': r[3], 'photo_path': r[4]} for r in c.fetchall()]
    conn.close()
    return render_template('index.html', logs=logs)

@app.route('/logs_content')
def logs_content():
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    c.execute("SELECT timestamp, uid, name, status, photo_path FROM logs ORDER BY id DESC LIMIT 50")
    logs = [{'timestamp': r[0], 'uid': r[1], 'name': r[2], 'status': r[3], 'photo_path': r[4]} for r in c.fetchall()]
    conn.close()
    
    # 這裡必須回傳 logs_table.html，不能是 index.html
    return render_template('logs_table.html', logs=logs)

@app.route('/users')
def users_page():
    """人員管理頁面"""
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    c.execute("SELECT uid, name, image_path FROM users")
    users = [{'uid': r[0], 'name': r[1], 'image_path': r[2]} for r in c.fetchall()]
    conn.close()
    return render_template('users.html', users=users)

@app.route('/add_user', methods=['POST'])
def add_user():
    """新增人員 API"""
    uid = request.form['uid'].strip()
    name = request.form['name'].strip()
    file = request.files['file']

    if file:
        filename = f"{uid.replace(' ', '')}.jpg" # 用卡號當檔名
        filepath = os.path.join(USER_DIR, filename)
        file.save(filepath)

        # 寫入資料庫
        try:
            conn = sqlite3.connect(DB_NAME)
            c = conn.cursor()
            c.execute("INSERT INTO users (uid, name, image_path) VALUES (?, ?, ?)", (uid, name, filename))
            conn.commit()
            conn.close()
            
            # 重新載入特徵
            reload_users()
        except sqlite3.IntegrityError:
            return "錯誤：該 UID 已存在！"
        
    return redirect(url_for('users_page'))

@app.route('/delete_user/<uid>')
def delete_user(uid):
    """刪除人員 API"""
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    
    # 刪除檔案 (選擇性，也可以保留)
    c.execute("SELECT image_path FROM users WHERE uid=?", (uid,))
    result = c.fetchone()
    if result:
        try:
            os.remove(os.path.join(USER_DIR, result[0]))
        except:
            pass

    c.execute("DELETE FROM users WHERE uid=?", (uid,))
    conn.commit()
    conn.close()
    
    reload_users() # 更新記憶體
    return redirect(url_for('users_page'))

# 設定圖片資料夾路由，讓網頁讀得到圖
@app.route('/captures/<filename>')
def uploaded_file(filename):
    return send_from_directory(CAPTURE_DIR, filename)

@app.route('/users_img/<filename>')
def user_file(filename):
    return send_from_directory(USER_DIR, filename)

if __name__ == "__main__":
    # 1. 初始化資料庫
    init_db()
    
    # 2. 載入使用者特徵
    reload_users() 
    
    # ==========================================
    # ★★★ 新增：開機時先檢查一次相機 ★★★
    # ==========================================
    if check_camera_startup():
        print("🟢 系統狀態良好，準備啟動服務...")
    else:
        print("🔴 警告：相機連線失敗！")
        print("   (系統雖然會繼續啟動，但刷卡時可能無法拍照)")
        # 如果你希望連不上就直接不讓程式跑，可以把下面這行取消註解：
        # exit() 

    # 3. 啟動 RFID 監聽 (背景執行)
    t = threading.Thread(target=rfid_server_thread)
    t.daemon = True 
    t.start()
    
    # 4. 啟動網頁伺服器
    print("🌍 網頁介面已啟動！請瀏覽 http://127.0.0.1:5000")
    app.run(host='0.0.0.0', port=5000, debug=False)