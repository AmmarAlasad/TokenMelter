import os
import json
import sys
import subprocess
import re

def get_connected_wifi_ssid():
    """Gets the SSID of the currently connected Wi-Fi network in a cross-platform way."""
    try:
        if sys.platform == "win32":
            out = subprocess.check_output(["netsh", "wlan", "show", "interfaces"], text=True, errors="ignore")
            for line in out.splitlines():
                if "SSID" in line and "BSSID" not in line:
                    return line.split(":")[1].strip()
        elif sys.platform == "darwin":
            try:
                out = subprocess.check_output(["/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport", "-I"], text=True, errors="ignore")
                for line in out.splitlines():
                    if " SSID" in line:
                        return line.split(":")[1].strip()
            except Exception:
                out = subprocess.check_output(["networksetup", "-getairportnetwork", "en0"], text=True, errors="ignore")
                if "Current Wi-Fi Network:" in out:
                    return out.split(":")[1].strip()
        elif sys.platform.startswith("linux"):
            try:
                return subprocess.check_output(["iwgetid", "-r"], text=True, errors="ignore").strip()
            except Exception:
                out = subprocess.check_output(["nmcli", "-t", "-f", "active,ssid", "dev", "wifi"], text=True, errors="ignore")
                for line in out.splitlines():
                    if line.startswith("yes:"):
                        return line.split(":")[1].strip()
    except Exception:
        pass
    return None

def get_windows_wifi_password(ssid):
    """Attempts to extract the saved plain-text Wi-Fi password on Windows (requires admin elevation)."""
    try:
        out = subprocess.check_output(["netsh", "wlan", "show", "profile", f"name={ssid}", "key=clear"], text=True, errors="ignore")
        for line in out.splitlines():
            # Match English "Key Content" or German "Schlüsselinhalt" or "Schluesselinhalt"
            if "Key Content" in line or "Schluesselinhalt" in line or "Schlüsselinhalt" in line:
                return line.split(":")[1].strip()
    except Exception:
        pass
    return None

def sync_tokens():
    print("===================================================")
    print("  Codex Token & Wi-Fi Sync Utility (Cross-Platform)")
    print("===================================================")
    
    # 1. Resolve auth.json path
    codex_home = os.environ.get("CODEX_HOME")
    if codex_home:
        auth_path = os.path.join(codex_home, "auth.json")
    else:
        auth_path = os.path.join(os.path.expanduser("~"), ".codex", "auth.json")
        
    header_path = os.path.join("src", "auth_data.h")
    gitignore_path = ".gitignore"
    
    if not os.path.exists(auth_path):
        print(f"[ERROR] Codex auth.json not found at: {auth_path}")
        print("Please run the 'codex' CLI first to login to your account.")
        sys.exit(1)
        
    print(f"[INFO] Found Codex authentication file.")
    print("[INFO] Extracting tokens dynamically...")
    
    try:
        with open(auth_path, "r", encoding="utf-8") as f:
            auth_data = json.load(f)
            
        tokens = auth_data.get("tokens", {})
        token = tokens.get("access_token")
        acc_id = tokens.get("account_id")
        
        if not token or not acc_id:
            print("[ERROR] Valid Codex access_token or account_id not found in auth.json!")
            sys.exit(1)
            
        # 2. Extract or prompt for Wi-Fi credentials
        ssid = get_connected_wifi_ssid()
        password = ""
        
        if ssid:
            print(f"[INFO] Automatically detected active Wi-Fi: {ssid}")
            # Try to auto-extract password
            password = get_windows_wifi_password(ssid)
            if password:
                print("[INFO] Automatically extracted saved Wi-Fi password.")
            else:
                print("[INFO] Could not automatically extract Wi-Fi password from system.")
                password = input(f"Please enter Wi-Fi Password for network '{ssid}': ").strip()
        else:
            print("[INFO] Wi-Fi SSID could not be auto-detected (requires Windows Location Permission and Elevation).")
            while not ssid:
                ssid = input("Please enter Wi-Fi SSID: ").strip()
            password = input("Please enter Wi-Fi Password: ").strip()
            
        # 3. Write src/auth_data.h
        os.makedirs(os.path.dirname(header_path), exist_ok=True)
        with open(header_path, "w", encoding="utf-8") as f:
            f.write("#pragma once\n\n")
            f.write("// Codex Authentication Tokens\n")
            f.write(f'#define CODEX_ACCESS_TOKEN "{token}"\n')
            f.write(f'#define CODEX_ACCOUNT_ID "{acc_id}"\n\n')
            f.write("// Wi-Fi Connection Credentials\n")
            f.write(f'#define WIFI_SSID "{ssid}"\n')
            f.write(f'#define WIFI_PASSWORD "{password}"\n')
            
        print(f"[SUCCESS] {header_path} updated with latest tokens and Wi-Fi.")
        
        # 4. Add to .gitignore if not present
        if os.path.exists(gitignore_path):
            with open(gitignore_path, "r", encoding="utf-8") as f:
                content = f.read()
            if "src/auth_data.h" not in content:
                with open(gitignore_path, "a", encoding="utf-8") as f:
                    f.write("\nsrc/auth_data.h\n")
                print("[INFO] Added src/auth_data.h to .gitignore")
                
        print("===================================================")
        print("[SUCCESS] Token & Wi-Fi sync completed successfully!")
        print("===================================================")
        
    except Exception as e:
        print(f"[ERROR] Failed to process auth.json: {e}")
        sys.exit(1)

if __name__ == "__main__":
    sync_tokens()
