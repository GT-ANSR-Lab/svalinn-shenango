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
for node in NODES:
    node_conn = paramiko.SSHClient()
    node_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    node_conn.connect(hostname = node["name"], username = USERNAME, pkey = k)
    conns.append(node_conn)

# distributing code-base
print("Distributing sources...")
for i in range(len(NODES)):
    cmd = "rsync -azh -e \"ssh -i {} -o StrictHostKeyChecking=no"\
            " -o UserKnownHostsFile=/dev/null\" --info=progress2 ../../"\
            " {}@{}:~/{}"\
            .format(KEY_LOCATION, USERNAME, NODES[i]["name"], ARTIFACT_PATH)
    execute_local(cmd)
    # Must have a shenango patch for every node type used
    cmd = "cd ~/{} && git apply --allow-empty host_patches/{}.patch"\
          .format(ARTIFACT_PATH, NODES[i]["type"])
    execute_remote(conns[i:i+1], cmd, True)

# Perform any machine-specific setup
for i in range(len(NODES)):
    if NODES[i]["type"] == "xl170":
        pass
    elif NODES[i]["type"] == "c6525-25g":
        cmd = "sudo modprobe amd-uncore"
        execute_remote(conns[i:i+1], cmd, True)

# build caladan
print("Building Caladan...")
cmd = "cd ~/{} && make clean && make -j16".format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)
cmd = "cd ~/{}/ksched && sudo make clean && make -j16".format(ARTIFACT_PATH)
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
cmd = "cd ~/{}/m-semaphore"\
    " && make clean && make all".\
    format(ARTIFACT_PATH)
execute_remote(conns, cmd, True)

print("Done.")
