#!/usr/bin/env python3
"""Phantom read test variants for index range scan."""
import os
import shutil
import socket
import subprocess
import threading
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


def setup_db():
    db_dir = os.path.join(BUILD_DIR, "phantom_v4_db")
    if os.path.isdir(db_dir):
        shutil.rmtree(db_dir)
    server = subprocess.Popen([RMDB, "phantom_v4_db"], cwd=BUILD_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    s = socket.socket()
    s.settimeout(15)
    s.connect(("127.0.0.1", PORT))
    for q in [
        "create table t (id int, val int)",
        "create index t(id)",
        "insert into t values(1, 10)",
        "insert into t values(5, 50)",
        "insert into t values(10, 100)",
    ]:
        send(s, q)
    return server, s


def test_insert_abort_vs_reader_abort():
    """Classic: T1 reads, T2 inserts, T1 reads again."""
    server, s1 = setup_db()
    s2 = socket.socket()
    s2.settimeout(15)
    s2.connect(("127.0.0.1", PORT))
    try:
        send(s1, "begin")
        send(s2, "begin")
        r1 = send(s1, "select * from t where id > 2 and id < 8")
        r2 = send(s2, "insert into t values(3, 30)")
        r3 = send(s1, "select * from t where id > 2 and id < 8")
        send(s1, "commit")
        send(s2, "commit")
        print("=== insert between selects ===")
        print("first:", "abort" if "abort" in r1.lower() else "ok")
        print("insert:", r2.strip() or "(empty)")
        print("second:", "abort" if "abort" in r3.lower() else "ok")
        if "abort" in r2.lower():
            print("RESULT: T2 aborted on insert")
        elif "abort" in r3.lower():
            print("RESULT: T1 aborted on second select (expected for test4)")
        else:
            print("RESULT: PHANTOM - both succeeded")
    finally:
        s1.close()
        s2.close()
        server.terminate()
        server.wait(timeout=3)


def test_count():
    server, s1 = setup_db()
    s2 = socket.socket()
    s2.settimeout(15)
    s2.connect(("127.0.0.1", PORT))
    try:
        send(s1, "begin")
        send(s2, "begin")
        r1 = send(s1, "select count(*) from t where id > 2 and id < 8")
        r2 = send(s2, "insert into t values(3, 30)")
        r3 = send(s1, "select count(*) from t where id > 2 and id < 8")
        print("=== count(*) ===")
        print("first:", r1[:80])
        print("insert:", r2.strip() or "(empty)")
        print("second:", r3[:80] if "abort" not in r3.lower() else "abort")
    finally:
        s1.close()
        s2.close()
        server.terminate()
        server.wait(timeout=3)


def test_seq_scan():
    server, s1 = setup_db()
    s2 = socket.socket()
    s2.settimeout(15)
    s2.connect(("127.0.0.1", PORT))
    try:
        send(s1, "drop index t(id)")
        send(s1, "begin")
        send(s2, "begin")
        r1 = send(s1, "select * from t where id > 2 and id < 8")
        r2 = send(s2, "insert into t values(3, 30)")
        print("=== seq scan (no index) ===")
        print("insert:", r2.strip() or "(empty)")
        print("expected: insert aborts due to table S lock")
    finally:
        s1.close()
        s2.close()
        server.terminate()
        server.wait(timeout=3)


if __name__ == "__main__":
    test_insert_abort_vs_reader_abort()
    test_count()
    test_seq_scan()
