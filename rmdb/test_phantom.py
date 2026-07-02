#!/usr/bin/env python3
"""Phantom read smoke test with indexed range scan."""
import os
import shutil
import socket
import subprocess
import time

BUILD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
RMDB = os.path.join(BUILD_DIR, "bin", "rmdb")
PORT = 8765


def send(sock, q):
    msg = q if q.endswith(";") else q + ";"
    sock.sendall(msg.encode() + b"\0")
    r = b""
    while b"\0" not in r:
        r += sock.recv(65536)
    return r.decode().split("\0")[0]


def run_phantom():
    db_dir = os.path.join(BUILD_DIR, "phantom_test_db")
    if os.path.isdir(db_dir):
        shutil.rmtree(db_dir)
    server = subprocess.Popen([RMDB, "phantom_test_db"], cwd=BUILD_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    try:
        s1 = socket.socket()
        s2 = socket.socket()
        s1.settimeout(10)
        s2.settimeout(10)
        s1.connect(("127.0.0.1", PORT))
        s2.connect(("127.0.0.1", PORT))

        for q in [
            "create table t (id int, val int)",
            "create index t(id)",
            "insert into t values(1, 10)",
            "insert into t values(5, 50)",
            "insert into t values(10, 100)",
        ]:
            send(s1, q)

        send(s1, "begin")
        send(s2, "begin")
        r1 = send(s1, "select * from t where id > 2 and id < 8")
        r2 = send(s2, "insert into t values(3, 30)")
        r3 = send(s1, "select * from t where id > 2 and id < 8")
        send(s1, "commit")
        send(s2, "commit")

        s1.close()
        s2.close()
        return r1, r2, r3
    finally:
        server.terminate()
        server.wait(timeout=3)


r1, r2, r3 = run_phantom()
print("first:", r1)
print("insert:", r2)
print("second:", r3)
if "abort" in r2.lower():
    print("PASS: insert aborted (no phantom)")
elif r1 == r3:
    print("PASS: repeatable read")
else:
    print("FAIL: phantom detected")
