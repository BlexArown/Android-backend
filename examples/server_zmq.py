import zmq
from datetime import datetime

PORT = 5555
LOG_FILE = "android_messages.log"

saved = []
count = 0

def save(msg: str):
    global count
    count += 1
    saved.append(msg)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(f"{count}\t{datetime.now().isoformat(timespec='seconds')}\t{msg}\n")

def print_all():
    print("=== Сохраненные данные ===")
    for i, m in enumerate(saved, start=1):
        print(f"{i}: {m}")
    print("=================")

def main():
    ctx = zmq.Context()
    sock = ctx.socket(zmq.REP)
    sock.bind(f"tcp://*:{PORT}")
    print(f"[SERVER] Слушаю *:{PORT}")

    while True:
        msg = sock.recv_string()
        print(f"[SERVER] Получено: {msg}")
        save(msg)

        reply = "Hello from Server!"
        sock.send_string(reply)

        print_all()
        print(f"[SERVER] Всего пакетов: {count}")

if __name__ == "__main__":
    main()
