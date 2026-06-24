#!/usr/bin/env python3
import os
import shutil
import socket
import subprocess
import sys
import time

BUILD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build")
DB_NAME = "aggregator_test_db"
DB_DIR = os.path.join(BUILD_DIR, DB_NAME)
RMDB = os.path.join(BUILD_DIR, "bin", "rmdb")
PORT = 8765


def send_sql(sock, sql):
    msg = sql if sql.endswith(";") else sql + ";"
    sock.sendall(msg.encode() + b"\0")
    resp = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        resp += chunk
        if b"\0" in resp:
            break
    return resp.decode("utf-8", errors="replace").rstrip("\0")


def run_queries(queries):
    if os.path.isdir(DB_DIR):
        shutil.rmtree(DB_DIR)
    server = subprocess.Popen(
        [RMDB, DB_NAME],
        cwd=BUILD_DIR,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1)
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect(("127.0.0.1", PORT))
        for q in queries:
            send_sql(sock, q)
        sock.close()
    finally:
        server.terminate()
        try:
            server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
    output_path = os.path.join(DB_DIR, "output.txt")
    if os.path.exists(output_path):
        with open(output_path) as f:
            return f.read()
    return ""


def main():
    q1 = [
        "create table aggregate (id int,val float)",
        "insert into aggregate values(1,5.5)",
        "insert into aggregate values(3,4.5)",
        "insert into aggregate values(5,10.0)",
        "select SUM(id) as sum_id from aggregate",
        "select SUM(val) as sum_val from aggregate",
    ]
    q2 = [
        "create table aggregate (id int,val float)",
        "insert into aggregate values(1,5.5)",
        "insert into aggregate values(3,4.5)",
        "insert into aggregate values(5,10.0)",
        "select MAX(id) as max_id from aggregate",
        "select MIN(val) as min_val from aggregate",
    ]
    q3 = [
        "create table aggregate (id int,name char(8),val float)",
        "insert into aggregate values (1,'qwerasdf',1.0)",
        "insert into aggregate values (2,'qwerasdf',2.0)",
        "insert into aggregate values (3,'uiophjkl',2.0)",
        "select COUNT(*) as count_row from aggregate",
        "select COUNT(id) as count_id from aggregate",
        "select COUNT(name) as count_name from aggregate where val = 2.0",
    ]
    for label, queries in [("test1", q1), ("test2", q2), ("test3", q3)]:
        print(f"=== {label} ===")
        print(run_queries(queries))


if __name__ == "__main__":
    main()
