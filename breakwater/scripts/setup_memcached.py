#!/usr/bin/env python3
import os
import paramiko
from util import *
from config_remote import *

k = paramiko.RSAKey.from_private_key_file(KEY_LOCATION)

# config check
if len(NODES) < 1:
    print("[ERROR] There is no server to configure.")
    exit()

# connections to servers
conns = []
server_conn = None
client_conns = []
for node in NODES:
    node_conn = paramiko.SSHClient()
    node_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    node_conn.connect(hostname = node["name"], username = USERNAME, pkey = k)
    conns.append(node_conn)
    if node in SERVERS:
        server_conn = node_conn
    if node in CLIENTS:
        client_conns.append(node_conn)

# build memcached
print("Setting up memcached server...")
cmd = "cd ~/{}/breakwater/apps/memcached/server && ./version.sh && autoreconf -i"\
        " && ./configure --with-shenango=$HOME/{} && make clean && make"\
        .format(ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)

print("Building memcached client...")
cmd = "cd ~/{}/breakwater/apps/memcached/client && make clean && make"\
        .format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)

print("Done.")
