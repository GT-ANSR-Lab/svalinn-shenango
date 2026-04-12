#!/usr/bin/env python3

from pathlib import Path
import sys

sys.path.append(str(Path(__file__).resolve().parent.parent))

import os
import paramiko
from util import *
from config_remote import *
from time import sleep

# This script will install HPE Mellanox Firmware version 1.0.24 for ConnectX-4 NICs
# This firmware version is required to run the new Caladan directpath code. The
# firmware version that will be installed will be 14.32.1908

k = paramiko.RSAKey.from_private_key_file(KEY_LOCATION)

# Update this with the list of nodes (from config_remote.NODES) that need a firmware
# upgrade. For example if nodes at index 8 and 10 in the config_remote.NODES list
# require an update, we can set NODES_TO_UPDATE = [NODES[8], NODES[10]].
# You need to manually check which node has an older firmware version using ethtool
# for your required NIC and then decide whether to add that node in this list.
NODES_TO_UPDATE = []

# connections to servers
conns = []
for node in NODES_TO_UPDATE:
    # Check if the node type is xl170
    if node["type"] != "xl170":
        continue
    node_conn = paramiko.SSHClient()
    node_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    node_conn.connect(hostname = node["name"], username = USERNAME, pkey = k)
    conns.append(node_conn)

# Download the firmware
print("Downloading the firwmare...")
cmd = "mkdir -p ~/mlnx_cx4_fw"
execute_remote(conns, cmd, True)
cmd = "cd ~/mlnx_cx4_fw && curl -fL -o \"firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.compsig\" https://downloads.hpe.com/pub/softlib2/software1/sc-linux-fw/p1182272498/v275901/firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.compsig"
execute_remote(conns, cmd, True)
cmd = "cd ~/mlnx_cx4_fw && curl -fL -o \"firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.rpm\" https://downloads.hpe.com/pub/softlib2/software1/sc-linux-fw/p1182272498/v275901/firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.rpm"
execute_remote(conns, cmd, True)

# Install the firmware
print("Installing the firmware...")
cmd = "sudo apt-get -y install alien"
execute_remote(conns, cmd, True)
cmd = "cd ~/mlnx_cx4_fw && sudo alien -i firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.rpm"
execute_remote(conns, cmd, True)
cmd = "sudo /usr/lib/x86_64-linux-gnu/scexe-compat/CP069154.scexe -s"
execute_remote(conns, cmd, True)

# Close the connections
for conn in conns:
    conn.close()
