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

# change default shell to bash
print("Changing default shell to bash...")
conns = []
for server in NODES:
    print(server)
    server_conn = paramiko.SSHClient()
    server_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    server_conn.connect(hostname = server, username = USERNAME, pkey = k)
    conns.append(server_conn)

execute_remote(conns, "sudo usermod -s /bin/bash {}".format(USERNAME), True, False)

for conn in conns:
    conn.close()

# connections to servers
server_conn = paramiko.SSHClient()
server_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
server_conn.connect(hostname = SERVERS[0], username = USERNAME, pkey = k)

conns = []
conns.append(server_conn)
for node in CLIENTS:
    node_conn = paramiko.SSHClient()
    node_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    node_conn.connect(hostname = node, username = USERNAME, pkey = k)
    conns.append(node_conn)

# clean up machines
print("Cleaning up machines...")
cmd = "sudo killall -9 cstate"
execute_remote(conns, cmd, True, False)

cmd = "sudo killall -9 iokerneld"
execute_remote(conns, cmd, True, False)

cmd = "sudo rm -rf ~/{}".format(ARTIFACT_PATH)
execute_remote(conns, cmd, True, False)

# distributing code-base
print("Distributing sources...")
for server in NODES:
    cmd = "rsync -azh -e \"ssh -i {} -o StrictHostKeyChecking=no"\
            " -o UserKnownHostsFile=/dev/null\" --info=progress2 ../../"\
            " {}@{}:~/{}"\
            .format(KEY_LOCATION, USERNAME, server, ARTIFACT_PATH)
    execute_local(cmd)

# switch to the required branch
for i in range(len(NODES)):
    cmd = "cd ~/{} && git stash && git checkout {}"\
        .format(ARTIFACT_PATH, NODE_ARTIFACT_BRANCH_MAP[NODES[i]])
    execute_remote([conns[i]], cmd, True)

# install the dependencies
print("Installing listed Caladan dependencies...")
cmd = "sudo apt-get update"
execute_remote(conns, cmd, True)

cmd = "sudo apt-get -y install build-essential libnuma-dev clang autoconf"\
        " autotools-dev m4 automake libevent-dev  libpcre++-dev libtool"\
        " ragel libev-dev moreutils parallel cmake python3 python3-pip"\
        " libjemalloc-dev libaio-dev libdb5.3++-dev numactl hwloc libmnl-dev"\
        " libnl-3-dev libnl-route-3-dev uuid-dev libssl-dev libcunit1-dev pkg-config"\
        " intel-cmt-cat"
execute_remote(conns, cmd, True)
cmd = "pip3 install pandas openpyxl xlrd"
execute_remote(conns, cmd, True)

# build caladan
print("Building Caladan...")
execute_remote(conns, cmd, True)
cmd = "cd ~/{} && make clean && make submodules -j16 && make -j16".format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)
cmd = "cd ~/{}/ksched && make clean && make -j16".format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)

# build additional applications
cmd = "cd ~/{}/apps/storage_service && ./snappy.sh".format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)
for dir_to_build in ["", "shim", "bindings/cc",
                     "apps/storage_service", "apps/netbench"]:
    cmd = "cd ~/{}/{} && make".format(ARTIFACT_PATH, dir_to_build)
    execute_remote(conns, cmd, True)

# setting up machines
print("Setting up machines...")
cmd = "cd ~/{}/breakwater && sudo ./scripts/setup_machine.sh".format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)

# build breakwater
print("Building Breakwater...")
cmd = "cd ~/{}/breakwater && make clean && make -j16 &&"\
        " make -C bindings/cc".format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)

# build memory semaphore library
print("Building MemSemaphore...")
cmd = "cd ~/{}/m-semaphore && make submodules"\
    " && make clean && make all".\
    format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)

print("Done.")
