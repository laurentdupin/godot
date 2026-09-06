#!/usr/bin/env python3
"""Check CGD discovery, navigation and completion through the real LSP connection."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cgd-lsp-") as folder:
        root = Path(folder)
        (root / "project.godot").write_text('config_version=5\n[application]\nconfig/name="CGD LSP Test"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n', encoding="utf-8")
        source = 'class_name CGDProtocolExample;\nextends Node3D;\nint score = 1;\nvoid _ready() {\n    print(score);\n}\n'
        script = root / "example.cgd"
        script.write_text(source, encoding="utf-8")
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        with (root / "editor.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen([str(args.godot.resolve()), "--headless", "--editor", "--path", str(root), "--lsp-port", str(port)], stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 45
                while True:
                    try:
                        connection = socket.create_connection(("127.0.0.1", port), timeout=1)
                        break
                    except OSError:
                        if process.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError("Language server did not start")
                        time.sleep(0.1)
                with connection:
                    connection.settimeout(30)
                    stream = connection.makefile("rb")
                    next_id = 0

                    def send(method, params, request_id=None):
                        message = {"jsonrpc": "2.0", "method": method, "params": params}
                        if request_id is not None:
                            message["id"] = request_id
                        payload = json.dumps(message).encode("utf-8")
                        connection.sendall(f"Content-Length: {len(payload)}\r\n\r\n".encode() + payload)

                    def request(method, params):
                        nonlocal next_id
                        next_id += 1
                        send(method, params, next_id)
                        while True:
                            headers = {}
                            while True:
                                line = stream.readline()
                                if not line:
                                    raise RuntimeError("Language server disconnected")
                                if line == b"\r\n":
                                    break
                                key, value = line.decode().split(":", 1)
                                headers[key.lower()] = value.strip()
                            message = json.loads(stream.read(int(headers["content-length"])))
                            if message.get("id") == next_id:
                                if "error" in message:
                                    raise RuntimeError(message["error"])
                                return message.get("result")

                    initialized = request("initialize", {"rootUri": root.as_uri(), "capabilities": {}})
                    assert initialized, "Missing capabilities"
                    send("initialized", {})
                    symbols = request("textDocument/documentSymbol", {"textDocument": {"uri": script.as_uri()}})
                    assert any(item["name"] == "CGDProtocolExample" for item in symbols), symbols
                    send("textDocument/didOpen", {"textDocument": {"uri": script.as_uri(), "languageId": "cgd", "version": 1, "text": source}})
                    symbols = request("textDocument/documentSymbol", {"textDocument": {"uri": script.as_uri()}})
                    assert symbols and "score" in json.dumps(symbols), symbols
                    definition = request("textDocument/definition", {"textDocument": {"uri": script.as_uri()}, "position": {"line": 4, "character": 11}})
                    assert definition and definition[0]["range"]["start"]["line"] == 2, definition
                    edited = source.replace("print(score);", "posit")
                    send("textDocument/didChange", {"textDocument": {"uri": script.as_uri(), "version": 2}, "contentChanges": [{"text": edited}]})
                    options = request("textDocument/completion", {"textDocument": {"uri": script.as_uri()}, "position": {"line": 4, "character": 9}})
                    if isinstance(options, dict):
                        options = options["items"]
                    assert any(item["label"] == "position" for item in options), options
                    print("CGD_LSP_DISCOVERY_NAVIGATION_COMPLETION_OK")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
                log.flush()
                print((root / "editor.log").read_text(encoding="utf-8", errors="replace"))


if __name__ == "__main__":
    main()
