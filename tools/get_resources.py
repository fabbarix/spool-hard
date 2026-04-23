import requests
import json
import os
from dotenv import load_dotenv

def get_resources():
    # Load environment variables from .env
    load_dotenv()
    
    access_token = os.getenv("ACCESS_TOKEN")
    region = os.getenv("REGION", "global").lower()
    
    if not access_token:
        print("[!] Error: ACCESS_TOKEN not found in .env file.")
        return

    if region == "china":
        base_url = "https://api.bambulab.cn"
    else:
        base_url = "https://api.bambulab.com"

    endpoint = "/v1/iot-service/api/slicer/setting?version=2.4.0.5&public=true"
    url = f"{base_url}{endpoint}"
    
    headers = {
        "Authorization": f"Bearer {access_token}",
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36",
        "Accept": "application/json, text/plain, */*",
        "X-BBL-Client-Name": "OrcaSlicer",
        "X-BBL-Client-Type": "slicer",
        "X-BBL-OS-Type": "linux",
        "Content-Type": "application/json",
    }

    print(f"[*] Requesting URL: {url}")
    
    try:
        response = requests.get(url, headers=headers)
        
        print(f"[*] Response Status Code: {response.status_code}")
        
        if response.status_code == 200:
            data = response.json()
            print("\n[+] Resource Data Received:")
            print(json.dumps(data, indent=2))
        else:
            print(f"[!] Request failed: {response.text}")
            
    except Exception as e:
        print(f"[!] An error occurred: {e}")

if __name__ == "__main__":
    get_resources()
