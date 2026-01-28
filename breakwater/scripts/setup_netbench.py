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
server_conn = paramiko.SSHClient()
server_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
server_conn.connect(hostname = SERVERS[0], username = USERNAME, pkey = k)

conns = []
client_conns = []
conns.append(server_conn)
for node in CLIENTS:
    node_conn = paramiko.SSHClient()
    node_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    node_conn.connect(hostname = node, username = USERNAME, pkey = k)
    conns.append(node_conn)
    client_conns.append(node_conn)

# build the netbench program
print("Building netbench client and server programs...")
cmd = "cd ~/{}/breakwater/apps/netbench/src && make clean && make".format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)

print("Done.")
