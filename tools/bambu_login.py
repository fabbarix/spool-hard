import requests
import json
import base64
import sys

class BambuCloud:
    def __init__(self, region="global"):
        if region == "china":
            self.base_url = "https://api.bambulab.cn/v1/user-service"
        else:
            self.base_url = "https://api.bambulab.com/v1/user-service"
        
        self.session = requests.Session()
        self.headers = {
            "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36",
            "Accept": "application/json, text/plain, */*",
            "Accept-Language": "en-US,en;q=0.9",
            "Origin": "https://bambulab.com",
            "Referer": "https://bambulab.com/",
            "X-BBL-Client-Name": "OrcaSlicer",
            "X-BBL-Client-Type": "slicer",
            "X-BBL-Client-Version": "01.09.05.51",
            "X-BBL-OS-Type": "linux",
            "Content-Type": "application/json",
        }

    def login(self, account, password):
        print(f"[*] Attempting login for {account}...")
        url = f"{self.base_url}/user/login"
        print(f"[*] Requesting URL: {url}")
        payload = {
            "account": account,
            "password": password,
            "apiError": ""
        }

        response = self.session.post(url, headers=self.headers, json=payload)
        
        if response.status_code != 200:
            print(f"[!] Login failed with status {response.status_code}")
            print(response.text)
            return None

        data = response.json()
        print(f"[*] Received login response")
        print(json.dumps(data, indent=2))

        # Handle Verification Code or TFA
        login_type = data.get("loginType")
        if login_type == "verifyCode":
            print("[!] E-mail verification code required.")
            code = input("[?] Enter the code: ")
            
            # Post code back to the same login endpoint
            payload = {"account": account, "code": code}
            print(f"[*] Requesting URL: {url}")
            response = self.session.post(url, headers=self.headers, json=payload)
            if response.status_code == 200:
                data = response.json()
            else:
                print(f"[!] Verification failed: {response.text}")
                return None

        elif login_type == "tfa":
            print("[!] Two-Factor Authentication (TFA) code required.")
            tfa_key = data.get("tfaKey")
            code = input("[?] Enter the TFA code: ")
            
            # TFA endpoint is different
            tfa_url = "https://bambulab.com/api/sign-in/tfa"
            if "bambulab.cn" in self.base_url:
                tfa_url = "https://bambulab.cn/api/sign-in/tfa"
            
            print(f"[*] Requesting URL: {tfa_url}")
            tfa_payload = {
                "tfaKey": tfa_key,
                "tfaCode": code,
            }
            response = self.session.post(tfa_url, headers=self.headers, json=tfa_payload)
            
            if response.status_code == 200:
                data = response.json()
            else:
                print(f"[!] TFA failed: {response.text}")
                return None
        print(f"[*] Received token info")
        print(json.dumps(data, indent=2))

        if "accessToken" in data:
            return data["accessToken"]

        # Check for token in cookies (TFA often returns it there)
        token = self.session.cookies.get("token")
        if token:
            return token

        print("[!] Unexpected response structure:")
        print(json.dumps(data, indent=2))
        return None

def decode_jwt_payload(token):
    try:
        # JWT format is header.payload.signature
        parts = token.split('.')
        if len(parts) != 3:
            return None
        
        payload_b64 = parts[1]
        # Add padding if necessary
        missing_padding = len(payload_b64) % 4
        if missing_padding:
            payload_b64 += '=' * (4 - missing_padding)
            
        payload_json = base64.b64decode(payload_b64).decode('utf-8')
        return json.loads(payload_json)
    except Exception as e:
        print(f"[!] Error decoding JWT: {e}")
        return None

def emit_paste_blob(token, region, account):
    """Wrap (token, region, email) into the SPOOLHARD-TOKEN: blob the
    SpoolHard console expects. The console's set-token handler detects
    the prefix and unpacks the JSON instead of treating the input as a
    raw JWT. Wrapping keeps everything on one line so the user can copy
    it from any terminal without losing fields."""
    payload = json.dumps({
        "token":  token,
        "region": region,
        "email":  account,
    }, separators=(',', ':'))
    return "SPOOLHARD-TOKEN:" + base64.b64encode(payload.encode()).decode()

def main():
    print("=== Bambu Lab Cloud Login ===")
    region = input("[?] Region (global/china) [global]: ").strip().lower() or "global"
    account = input("[?] Email/Account: ").strip()
    import getpass
    password = getpass.getpass("[?] Password: ")

    client = BambuCloud(region)
    token = client.login(account, password)

    if token:
        print("\n[+] Login Successful!")
        print(f"[+] Access Token: {token}")

        payload = decode_jwt_payload(token)
        if payload:
            print("\n[+] JWT Payload JSON:")
            print(json.dumps(payload, indent=2))

            username = payload.get("username")
            if username:
                print(f"\n[+] Bambu Username (for MQTT): {username}")

        blob = emit_paste_blob(token, region, account)
        bar  = "=" * 60
        print(f"\n{bar}")
        print("Console paste blob (copy the entire line below):")
        print()
        print(blob)
        print()
        print('Paste this into Console -> Config -> Bambu Cloud -> "I already have a token".')
        print(bar)
    else:
        print("\n[!] Login failed.")

if __name__ == "__main__":
    main()
