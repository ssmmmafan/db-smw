#!/usr/bin/env python3
"""Test phantom behavior with/without explicit begin."""
import os, shutil, socket, subprocess, time

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


def setup():
    db = os.path.join(BUILD_DIR, "phantom_mode_db")
    if os.path.isdir(db):
        shutil.rmtree(db)
    srv = subprocess.Popen([RMDB, "phantom_mode_db"], cwd=BUILD_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    s1 = socket.socket()
    s2 = socket.socket()
    s1.settimeout(10)
    s2.settimeout(10)
    s1.connect(("127.0.0.1", PORT))
    s2.connect(("127.0.0.1", PORT))
    for q in [
        "create table t (id int, val int)",
        "insert into t values(1, 10)",
        "insert into t values(5, 50)",
        "insert into t values(10, 100)",
    ]:
        send(s1, q)
    return srv, s1, s2


def test_with_begin(srv, s1, s2):
    send(s1, "begin")
    send(s2, "begin")
    r1 = send(s1, "select * from t where id > 2 and id < 8")
    r2 = send(s2, "insert into t values(3, 30)")
    r3 = send(s1, "select * from t where id > 2 and id < 8")
    send(s1, "commit")
    send(s2, "commit")
    print("WITH begin:")
    print("  insert:", "abort" if "abort" in r2.lower() else "(ok)")
    print("  second:", "abort" if "abort" in r3.lower() else "(ok)")


def test_without_begin(srv, s1, s2):
    # no begin - each stmt auto-commits unless we keep same connection without commit?
    # actually without begin each select still same txn until auto-commit after EACH statement
    r1 = send(s1, "select * from t where id > 2 and id < 8")
    r2 = send(s2, "insert into t values(4, 40)")
    r3 = send(s1, "select * from t where id > 2 and id < 8")
    print("WITHOUT begin (each stmt auto-commit):")
    print("  insert:", "abort" if "abort" in r2.lower() else "(ok)")
    print("  second:", "abort" if "abort" in r3.lower() else "(ok)")


def test_index_with_begin(srv, s1, s2):
    send(s1, "create index t(id)")
    send(s1, "begin")
    send(s2, "begin")
    r1 = send(s1, "select * from t where id > 2 and id < 8")
    r2 = send(s2, "insert into t values(3, 30)")
    r3 = send(s1, "select * from t where id > 2 and id < 8")
    print("INDEX with begin:")
    print("  insert:", "abort" if "abort" in r2.lower() else "(ok)")
    print("  second:", "abort" if "abort" in r3.lower() else "(ok)")


if __name__ == "__main__":
    srv, s1, s2 = setup()
    try:
        test_with_begin(srv, s1, s2)
        test_without_begin(srv, s1, s2)
        test_index_with_begin(srv, s1, s2)
    finally:
        s1.close()
        s2.close()
        srv.terminate()
        srv.wait(timeout=3)
