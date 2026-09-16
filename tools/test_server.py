#!/usr/bin/env python3
"""A minimal xiaozhi server, for testing the client without the real service.

Speaks just enough of the protocol to exercise the paths that are hard to
verify any other way: the OTA exchange, the WebSocket handshake and framing,
the hello negotiation, and binary audio frames in both directions.

The WebSocket implementation here is deliberately written from the RFC rather
than reusing the client's own logic, so the two are independent checks on each
other.

    tools/test_server.py --port 8099
    build/host/c1xiaozhi --ota-url http://127.0.0.1:8099/ota/ -v
"""

import argparse
import base64
import hashlib
import json
import socket
import struct
import threading
import time

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_TEXT = 0x1
OP_BINARY = 0x2
OP_CLOSE = 0x8
OP_PING = 0x9
OP_PONG = 0xA


def recv_exactly(conn, count):
    data = b""
    while len(data) < count:
        chunk = conn.recv(count - len(data))
        if not chunk:
            raise ConnectionError("peer closed")
        data += chunk
    return data


def read_frame(conn):
    first, second = recv_exactly(conn, 2)
    fin = bool(first & 0x80)
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", recv_exactly(conn, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", recv_exactly(conn, 8))[0]
    mask = recv_exactly(conn, 4) if masked else b""
    payload = recv_exactly(conn, length) if length else b""
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return fin, opcode, payload


def write_frame(conn, opcode, payload=b""):
    header = bytearray([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length <= 0xFFFF:
        header.append(126)
        header += struct.pack(">H", length)
    else:
        header.append(127)
        header += struct.pack(">Q", length)
    conn.sendall(bytes(header) + payload)


def read_http_request(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None, {}, b""
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    request_line = lines[0]
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()
    length = int(headers.get("content-length", "0"))
    body = rest
    while len(body) < length:
        body += conn.recv(length - len(body))
    return request_line, headers, body


class Server:
    def __init__(self, port, activation_code=None):
        self.port = port
        self.activation_code = activation_code
        self.activation_polls = 0

    def serve(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", self.port))
        listener.listen(8)
        print(f"listening on http://127.0.0.1:{self.port}/ota/")
        while True:
            conn, _ = listener.accept()
            threading.Thread(target=self.handle, args=(conn,), daemon=True).start()

    def handle(self, conn):
        try:
            request_line, headers, body = read_http_request(conn)
            if request_line is None:
                return
            method, path, _ = request_line.split(" ", 2)
            print(f"\n--> {method} {path}")
            for key in ("device-id", "client-id", "protocol-version", "activation-version"):
                if key in headers:
                    print(f"    {key}: {headers[key]}")

            if headers.get("upgrade", "").lower() == "websocket":
                self.handle_websocket(conn, headers)
                return

            if path.rstrip("/").endswith("/activate"):
                self.handle_activate(conn)
                return

            self.handle_ota(conn, body)
        except (ConnectionError, OSError, ValueError) as error:
            print(f"    connection ended: {error}")
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def handle_ota(self, conn, body):
        if body:
            try:
                info = json.loads(body)
                app = info.get("application", {})
                print(f"    device reports: {app.get('name')} {app.get('version')}, "
                      f"mac {info.get('mac_address')}, display {info.get('display')}")
            except json.JSONDecodeError:
                print("    body is not JSON")

        response = {
            "websocket": {
                "url": f"ws://127.0.0.1:{self.port}/xiaozhi/v1/",
                "token": "test-token",
                "version": 1,
            },
            "server_time": {
                "timestamp": int(time.time() * 1000),
                "timezone_offset": 480,
            },
        }
        if self.activation_code and self.activation_polls < 2:
            response["activation"] = {
                "message": "在 xiaozhi.me 输入验证码",
                "code": self.activation_code,
                "challenge": "test-challenge",
                "timeout_ms": 30000,
            }
        payload = json.dumps(response).encode()
        conn.sendall(
            b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            + f"Content-Length: {len(payload)}\r\n".encode()
            + b"Connection: close\r\n\r\n"
            + payload
        )
        print("    replied with a websocket endpoint")

    def handle_activate(self, conn):
        self.activation_polls += 1
        if self.activation_polls < 2:
            print(f"    activation poll {self.activation_polls}: still pending (202)")
            conn.sendall(b"HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\n\r\n")
        else:
            print(f"    activation poll {self.activation_polls}: activated (200)")
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")

    def handle_websocket(self, conn, headers):
        key = headers.get("sec-websocket-key", "").encode()
        accept = base64.b64encode(hashlib.sha1(key + GUID).digest()).decode()
        conn.sendall(
            b"HTTP/1.1 101 Switching Protocols\r\n"
            b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            + f"Sec-WebSocket-Accept: {accept}\r\n\r\n".encode()
        )
        print("    websocket upgraded")

        audio_frames = 0
        spoke = False
        while True:
            fin, opcode, payload = read_frame(conn)
            if opcode == OP_CLOSE:
                print("    client closed the channel")
                write_frame(conn, OP_CLOSE)
                return
            if opcode == OP_PING:
                write_frame(conn, OP_PONG, payload)
                continue
            if opcode == OP_PONG:
                continue
            if opcode == OP_BINARY:
                audio_frames += 1
                if audio_frames % 25 == 0:
                    print(f"    received {audio_frames} audio frames "
                          f"({len(payload)} bytes in the last one)")
                if audio_frames == 20 and not spoke:
                    spoke = True
                    self.speak(conn)
                continue

            message = json.loads(payload)
            kind = message.get("type")
            print(f"    <- {payload.decode()[:200]}")
            if kind == "hello":
                reply = {
                    "type": "hello",
                    "transport": "websocket",
                    "session_id": "test-session",
                    "audio_params": {
                        "format": "opus",
                        "sample_rate": 24000,
                        "channels": 1,
                        "frame_duration": 60,
                    },
                }
                write_frame(conn, OP_TEXT, json.dumps(reply).encode())
                print("    -> server hello")
            elif kind == "listen" and message.get("state") == "start":
                write_frame(conn, OP_TEXT, json.dumps(
                    {"type": "stt", "text": "测试一下"}).encode())

    def speak(self, conn):
        """Answers with a short TTS turn: text only, no real audio."""
        write_frame(conn, OP_TEXT, json.dumps({"type": "tts", "state": "start"}).encode())
        write_frame(conn, OP_TEXT, json.dumps(
            {"type": "llm", "emotion": "happy", "text": "😀"}).encode())
        write_frame(conn, OP_TEXT, json.dumps(
            {"type": "tts", "state": "sentence_start",
             "text": "你好，我是小智，这是一条测试回复。"}).encode())
        time.sleep(0.5)
        write_frame(conn, OP_TEXT, json.dumps({"type": "tts", "state": "stop"}).encode())
        print("    -> sent a test TTS turn")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8099)
    parser.add_argument("--activation-code", default=None,
                        help="exercise the activation flow with this code")
    args = parser.parse_args()
    Server(args.port, args.activation_code).serve()


if __name__ == "__main__":
    main()
