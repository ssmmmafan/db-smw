#!/usr/bin/env python3
"""BNLJ smoke test: equi-join + non-equi join + order by."""
import os
import shutil
import socket
import subprocess
import time

BUILD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
RMDB = os.path.join(BUILD_DIR, "bin", "rmdb")
PORT = 8765


def run_db(db_name, queries, timeout=30):
    db_dir = os.path.join(BUILD_DIR, db_name)
    if os.path.isdir(db_dir):
        shutil.rmtree(db_dir)
    server = subprocess.Popen([RMDB, db_name], cwd=BUILD_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    try:
        sock = socket.socket()
        sock.settimeout(timeout)
        sock.connect(("127.0.0.1", PORT))
        for q in queries:
            msg = q if q.endswith(";") else q + ";"
            sock.sendall(msg.encode() + b"\0")
            r = b""
            while b"\0" not in r:
                r += sock.recv(4096)
        sock.close()
    finally:
        server.terminate()
        server.wait(timeout=3)
    out = os.path.join(db_dir, "output.txt")
    return open(out).read() if os.path.exists(out) else ""


queries = [
    "create table t1 (id int, val int)",
    "create table t2 (t_id int, name int)",
    "insert into t1 values(1, 10)",
    "insert into t1 values(2, 20)",
    "insert into t1 values(3, 30)",
    "insert into t2 values(1, 100)",
    "insert into t2 values(2, 200)",
    "insert into t2 values(4, 400)",
    "select * from t1, t2 where t1.id = t2.t_id order by t1.id",
    "select * from t1, t2 where t1.id < t2.t_id and t2.t_id < 1000",
]

t0 = time.time()
got = run_db("bnlj_smoke_db", queries)
elapsed = time.time() - t0
print(f"elapsed={elapsed:.2f}s")
print(got)
assert "failure" not in got.lower()
assert "| 1 |" in got
print("BNLJ smoke OK")
