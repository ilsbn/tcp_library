"""Run the polling server/client demos together and verify their thread handoffs."""
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import threading


def verify_threads(output):
    roles = {"main": set(), "tcp": set()}
    for thread_id, message in re.findall(r"\[thread (\d+)\] \[info\] ([^\r\n]+)", output):
        if message.startswith("Main: "):
            roles["main"].add(thread_id)
        elif message.startswith("TCP: "):
            roles["tcp"].add(thread_id)
    if len(roles["main"]) != 1 or len(roles["tcp"]) != 1 or roles["main"] & roles["tcp"]:
        raise RuntimeError(f"expected one TCP thread separate from main polling logic: {roles}")


def collect_lines(stream, lines, output):
    for line in stream:
        output.append(line)
        lines.put(line)
    lines.put(None)


def main():
    server_path, client_path = (str(Path(arg).resolve()) for arg in sys.argv[1:])
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    server = subprocess.Popen(
        [server_path, "0", "--once"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, creationflags=flags,
    )
    lines = queue.Queue()
    output = []
    reader = threading.Thread(target=collect_lines, args=(server.stdout, lines, output), daemon=True)
    reader.start()
    try:
        exits_on_disconnect = False
        while True:
            try:
                line = lines.get(timeout=5)
            except queue.Empty:
                raise RuntimeError("server did not report its listening port:\n" + "".join(output)) from None
            if line is None:
                raise RuntimeError("server exited before accepting a client:\n" + "".join(output))
            if line.startswith("LISTENING "):
                port = line.split()[1]
                exits_on_disconnect = True
                break
            started = re.search(r"Main: TCP server started on .+:(\d+)", line)
            if started:
                port = started.group(1)
                break

        client = subprocess.run(
            [client_path, "127.0.0.1", port, "--once"], capture_output=True, text=True,
            timeout=8, creationflags=flags,
        )
        print(client.stdout, end="")
        print(client.stderr, end="", file=sys.stderr)
        if client.returncode != 0:
            raise RuntimeError(f"client failed: {client.returncode}")
        if exits_on_disconnect:
            if server.wait(timeout=5) != 0:
                raise RuntimeError("server failed")
        else:
            # The manually configured server keeps running after a client disconnects.
            while True:
                try:
                    line = lines.get(timeout=5)
                except queue.Empty:
                    raise RuntimeError("server did not report client disconnection") from None
                if line is None:
                    raise RuntimeError("server exited before reporting client disconnection")
                if "TCP: client disconnected" in line:
                    break
            server.terminate()
            server.wait(timeout=3)
        reader.join(timeout=2)
        server_output = "".join(output)
        print(server_output, end="")
        for role, text in (("client", client.stdout), ("server", server_output)):
            for message in ("Heartbeat: sequence=", "ShoppingCart: login=alice, items=2"):
                if f"Main: {role} received {message}" not in text:
                    raise RuntimeError(f"{role} did not fetch and process {message}")
            verify_threads(text)

        # A failed connection must reach the polling loop and exit, rather than wait forever.
        refused = subprocess.run(
            [client_path, "127.0.0.1", port, "--once"], capture_output=True, text=True,
            timeout=5, creationflags=flags,
        )
        if refused.returncode != 1 or "connect" not in refused.stdout + refused.stderr:
            raise RuntimeError("client did not report the closed server's connection error")
        print("Separate demos exchanged both message types, kept TCP off the main thread, and reported disconnection")
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)
        reader.join(timeout=2)


if __name__ == "__main__":
    main()
