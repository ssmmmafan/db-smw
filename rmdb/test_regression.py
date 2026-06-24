#!/usr/bin/env python3
"""Quick regression: plain SELECT, DATETIME, aggregates."""
import os
import shutil
import socket
import subprocess
import sys
import time

BUILD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
RMDB = os.path.join(BUILD_DIR, "bin", "rmdb")
PORT = 8765


def run_db(db_name, queries):
    db_dir = os.path.join(BUILD_DIR, db_name)
    if os.path.isdir(db_dir):
        shutil.rmtree(db_dir)
    server = subprocess.Popen([RMDB, db_name], cwd=BUILD_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect(("127.0.0.1", PORT))
        for q in queries:
            msg = q if q.endswith(";") else q + ";"
            sock.sendall(msg.encode() + b"\0")
            resp = b""
            while b"\0" not in resp:
                c = sock.recv(4096)
                if not c:
                    break
                resp += c
        sock.close()
    finally:
        server.terminate()
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
    path = os.path.join(db_dir, "output.txt")
    return open(path).read() if os.path.exists(path) else ""


def check(name, got, expected):
    ok = got.strip() == expected.strip()
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    if not ok:
        print("  expected:", repr(expected))
        print("  got:     ", repr(got))
    return ok


all_ok = True

# Plain SELECT
got = run_db("reg_plain_db", [
    "create table t(id int, val float)",
    "insert into t values(1, 2.5)",
    "insert into t values(2, 3.5)",
    "select * from t",
    "select id from t where id = 2",
])
all_ok &= check("plain select", got, """| id | val |
| 1 | 2.500000 |
| 2 | 3.500000 |
| id |
| 2 |
""")

# DATETIME
got = run_db("reg_dt_db", [
    "create table t(id int, time datetime)",
    "insert into t values(1, '2023-05-18 09:12:19')",
    "select * from t",
    "insert into t values(2, '1999-13-07 12:30:00')",
    "select * from t",
])
all_ok &= check("datetime", got, """| id | time |
| 1 | 2023-05-18 09:12:19 |
failure
| id | time |
| 1 | 2023-05-18 09:12:19 |
""")

# Aggregate (subset)
got = run_db("reg_agg_db", [
    "create table aggregate (id int,val float)",
    "insert into aggregate values(1,5.5)",
    "insert into aggregate values(3,4.5)",
    "insert into aggregate values(5,10.0)",
    "select SUM(id) as sum_id from aggregate",
])
all_ok &= check("aggregate", got, """| sum_id |
| 9 |
""")

sys.exit(0 if all_ok else 1)
