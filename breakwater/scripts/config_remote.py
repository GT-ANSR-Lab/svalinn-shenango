###
### config_remote.py - configuration for remote servers
###

NODES = [
    "gon.cc.gatech.edu",
    "asta.cc.gatech.edu",
]
NODE_NUMA = [
    0,
    0,
]
NODE_CPU_CORES = [
    45,
    45,
]
NODE_ARTIFACT_BRANCH = [
    "gon",
    "asta",
]
NODE_NIC_PCI_ADDR = [
    "0000:17:00.1",
    "0000:17:00.0",
]
NODE_IP_ADDR = [
    "192.168.11.129",
    "192.168.10.128",
]
NODE_NETMASK = [
    "255.255.255.0",
    "255.255.255.0",
]
NODE_GATEWAY_ADDR = [
    "192.168.11.130",
    "192.168.10.130",
]

# Public domain or IP of server
SERVERS = NODES[0:1]
# Public domain or IP of intemediate
INTNODES = []
# Public domain or IP of client and agents
CLIENTS = NODES[1:]
# Public domain or IP of client
CLIENT = CLIENTS[0]
AGENTS = CLIENTS[1:]

# Public domain or IP of monitor
MONITOR = ""

# Username and SSH credential location to access
# the server, client, and agents via public IP
USERNAME = "bpardeshi3"
KEY_LOCATION = "/home/bpardeshi3/.ssh/id_rsa"

# Location of Shenango to be installed. With "", Shenango
# will be installed in the home direcotry
ARTIFACT_PARENT = ""

### End of config ###

NODE_NUMA_MAP = {x[0]:x[1] for x in list(zip(NODES, NODE_NUMA))}
NODE_CPU_CORES_MAP = {x[0]:x[1] for x in list(zip(NODES, NODE_CPU_CORES))}
NODE_ARTIFACT_BRANCH_MAP = {x[0]:x[1] for x in list(zip(NODES, NODE_ARTIFACT_BRANCH))}
NODE_NIC_PCI_ADDR_MAP = {x[0]:x[1] for x in list(zip(NODES, NODE_NIC_PCI_ADDR))}
NODE_IP_ADDR_MAP = {x[0]:x[1] for x in list(zip(NODES, NODE_IP_ADDR))}
NODE_NETMASK_MAP = {x[0]:x[1] for x in list(zip(NODES, NODE_NETMASK))}
NODE_GATEWAY_ADDR_MAP = {x[0]:x[1] for x in list(zip(NODES, NODE_GATEWAY_ADDR))}

ARTIFACT_PATH = ARTIFACT_PARENT
if ARTIFACT_PATH != "":
    ARTIFACT_PATH += "/"
ARTIFACT_PATH += "caladan-all-artifact"
