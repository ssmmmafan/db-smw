#!/usr/bin/env python3
"""BNLJ medium-scale benchmark."""
import os, shutil, socket, subprocess, time

BUILD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
RMDB = os.path.join(BUILD_DIR, "bin", "rmdb")
PORT = 8765

N = 5000

def run():
    db = os.path.join(BUILD_DIR, "bnlj_bench_db")
    if os.path.isdir(db):
        shutil.rmtree(db)
    srv = subprocess.Popen([RMDB, "bnlj_bench_db"], cwd=BUILD_DIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    s = socket.socket()
    s.settimeout(120)
    s.connect(("127.0.0.1", PORT))

    def q(sql):
        s.sendall((sql + ";").encode() + b"\0")
        r = b""
        while b"\0" not in r:
            r += s.recv(65536)

    q("create table t1 (id int, val int)")
    q("create table t2 (t_id int, name int)")
    for i in range(N):
        q(f"insert into t1 values({i}, {i*10})")
    for i in range(1000):
        q(f"insert into t2 values({i}, {i})")

    t0 = time.time()
    q("select * from t1, t2 where t1.id = t2.t_id order by t1.id")
    t1 = time.time() - t0
    t0 = time.time()
    q("select * from t1, t2 where t1.id < t2.t_id and t2.t_id < 1000")
    t2 = time.time() - t0
    s.close()
    srv.terminate()
    srv.wait(timeout=3)
    print(f"equi-join+sort ({N} rows): {t1:.2f}s")
    print(f"non-equi join: {t2:.2f}s")

if __name__ == "__main__":
    run()
