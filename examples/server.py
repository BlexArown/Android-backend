import socket


def start_server():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    server_address = ('localhost', 25565)
    server_socket.bind(server_address)

    server_socket.listen(1)
    print(f"Start-Server. Слушаем порт 25565...")

    while True:
        client_socket, client_address = server_socket.accept()
        print(f"Client-connected: {client_address}")

        try:
            data = client_socket.recv(1024)
            print(f"Получено от клиента: {data.decode()}")

            response = "Hello Client"
            client_socket.sendall(response.encode())
            print(f"Send-to-client: {response}")

        finally:
            client_socket.close()


if __name__ == "__main__":
    start_server()
