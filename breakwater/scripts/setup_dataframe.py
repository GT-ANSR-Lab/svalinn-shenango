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

# build dataframe (only on server)
print("Download the dataset...")
cmd = "cd ~/{}/breakwater/apps/dataframe && ./download_dataset.py".format(ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)
print("Building core DataFrame library...")
cmd = "cd ~/{}/breakwater/apps/dataframe/deps/DataFrame && mkdir Release && cd Release &&"\
      " cmake -DCMAKE_BUILD_TYPE=Release .. && make".format(ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)

# build the dataframe programs
print("Building DataFrame client and server programs...")
cmd = "cd ~/{}/breakwater/apps/dataframe && make clean && make server".format(ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)
cmd = "cd ~/{}/breakwater/apps/dataframe && make clean && make client".format(ARTIFACT_PATH)
execute_remote(client_conns, cmd, True)

print("Done.")
