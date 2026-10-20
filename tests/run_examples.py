"""Run the actual command-line programs together using an ephemeral port."""
import os
import queue
import subprocess
import sys
import threading


def collect_lines(stream, lines):
    for line in stream:
        lines.put(line)


def main():
    server_path, client_path = sys.argv[1:]
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    server = subprocess.Popen(
        [server_path, "0", "--once"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        creationflags=flags,
    )
    lines = queue.Queue()
    threading.Thread(target=collect_lines, args=(server.stdout, lines), daemon=True).start()
    try:
        while True:
            line = lines.get(timeout=5)
            print(line, end="")
            if line.startswith("LISTENING "):
                port = line.split()[1]
                break
        client = subprocess.run(
            [client_path, "127.0.0.1", port, "example smoke message"],
            capture_output=True, text=True, timeout=7, creationflags=flags,
        )
        print(client.stdout, end="")
        print(client.stderr, end="", file=sys.stderr)
        if client.returncode != 0:
            raise RuntimeError(f"echo client failed: {client.returncode}")
        if "Echo received: example smoke message" not in client.stdout:
            raise RuntimeError("echo client did not report the expected message")
        if server.wait(timeout=5) != 0:
            raise RuntimeError("echo server failed")
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)


if __name__ == "__main__":
    main()
