import zmq
import json
from datetime import datetime

PORT = 5555
LOG_FILE = "server_messages.log"

count = 0
saved = []

def save(raw: str):
    global count
    count += 1
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(f"{count}\t{datetime.now().isoformat()}\t{raw}\n")
    saved.append(raw)

def print_all():
    print("=== Все сохраненные данные ===")
    for i, s in enumerate(saved, start=1):
        print(f"{i}: {s}")
    print("======================")

def main():
    ctx = zmq.Context()
    sock = ctx.socket(zmq.REP)
    sock.bind(f"tcp://*:{PORT}")
    print(f"[SERVER] Слушаем *:{PORT}")

    while True:
        raw = sock.recv_string()
        print(f"\n[SERVER] Получено пакетов #{count + 1}")

        try:
            data = json.loads(raw)
            loc = data.get("location", {})
            print(
                f"[SERVER][JSON] lat={loc.get('latitude')} "
                f"lon={loc.get('longitude')} "
                f"время={loc.get('timestamp')}"
            )
            print(
                f"[SERVER][JSON] соты: "
                f"lte={len(data.get('lte', []))} "
                f"gsm={len(data.get('gsm', []))} "
                f"nr={len(data.get('nr', []))}"
            )
        except json.JSONDecodeError:
            print("[SERVER][TEXT]", raw)

        save(raw)
        sock.send_string("Hello from Server!")

if __name__ == "__main__":
    main()
