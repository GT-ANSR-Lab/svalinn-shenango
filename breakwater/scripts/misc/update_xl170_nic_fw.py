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
# This firmware version is required to run the new Caladan directpath code.


# connections to servers
conns = []
for node in NODES:
    if node["type"] != "xl170":
        continue
    node_conn = paramiko.SSHClient()
    node_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    node_conn.connect(hostname = node["name"], username = USERNAME, pkey = k)
    conns.append(node_conn)

# Download the firmware
cmd = "mkdir -p ~/mlnx_cx4_fw"
execute_remote(conns, cmd, True)
cmd = "cd ~/mlnx_cx4_fw && curl -fL -o \"firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.compsig\" https://downloads.hpe.com/pub/softlib2/software1/sc-linux-fw/p1182272498/v275901/firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.compsig"
execute_remote(conns, cmd, True)
cmd = "cd ~/mlnx_cx4_fw && curl -fL -o \"firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.rpm\" https://downloads.hpe.com/pub/softlib2/software1/sc-linux-fw/p1182272498/v275901/firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.rpm"
execute_remote(conns, cmd, True)

# Install the firmware
cmd = "sudo apt-get -y install alien"
execute_remote(conns, cmd, True)
cmd = "cd ~/mlnx_cx4_fw && sudo alien -i firmware-nic-mellanox-ethernet-only-1.0.24-1.1.x86_64.rpm"
execute_remote(conns, cmd, True)
cmd = "sudo /usr/lib/x86_64-linux-gnu/scexe-compat/CP069154.scexe -s"
execute_remote(conns, cmd, True)

# Close the connections
for conn in conns:
    conn.close()
