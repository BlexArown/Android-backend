import socket

client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

client_socket.connect(('localhost', 25565))
req = 'Hello, server!'
client_socket.sendall(req.encode())
print(f"Отправлены данные {req}")
data = client_socket.recv(1024)
print(f"Получены данные: {data}")

client_socket.close()