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

# install packages
cmd = "sudo apt-get install -y libgflags-dev libsnappy-dev zlib1g-dev"\
      " libbz2-dev liblz4-dev libzstd-dev"
execute_remote(conns, cmd, True)
cmd = "pip3 install pandas openpyxl xlrd"
execute_remote(conns, cmd, True)

# build rocksdb (only on server)
print("Building core RocksDB library...")
cmd = "cd ~/{}/breakwater/apps/rocksdb/deps/rocksdb && make clean && make -j16 static_lib".format(ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)

# build the rocksdb program
print("Building RocksDB client and server programs...")
cmd = "cd ~/{}/breakwater/apps/rocksdb && make clean && make server".format(ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)
cmd = "cd ~/{}/breakwater/apps/rocksdb && make clean && make client".format(ARTIFACT_PATH)
execute_remote(client_conns, cmd, True)

# Create a tmpfs filesystem on server (change the size if you want)
print("Creating 40GB in-memory file system on the server (NUMA node {})".format(NODE_NUMA_MAP[SERVERS[0]]))
cmd = "sudo umount /tmp/ramfs"
execute_remote([server_conn], cmd, True, False)
cmd = "sudo rm -rf /tmp/ramfs"
execute_remote([server_conn], cmd, True, False)
cmd = "sudo mkdir /tmp/ramfs && sudo numactl --membind={} mount -t tmpfs -o size=40G tmpfs /tmp/ramfs".format(NODE_NUMA_MAP[SERVERS[0]])
execute_remote([server_conn], cmd, True)

print("Done.")
