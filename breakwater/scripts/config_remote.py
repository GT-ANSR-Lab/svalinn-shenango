###
### config_remote.py - configuration for remote servers
###

# Paremters for a few known machines
#
# xl170 cloudlab
#   type: "xl170"
#   numa: 0
#   cores: 18
#   nicpci: 0000:03:00.1
#
# c6525-25g cloudlab
#   type: "c6525-25g"
#   numa: 0
#   cores: 30
#   nicpci: 0000:41:00.0
#

NODES = [
    {
        "name": "node-0.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-1.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-2.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-3.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-4.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-5.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-6.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-7.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-8.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-9.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-10.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "xl170",
        "numa": 0,
        "cores": 18,
        "nicpci": "0000:03:00.1",
    },
    {
        "name": "node-11.bhaskar3-294079.cc-profiler-pg0.utah.cloudlab.us",
        "type": "c6525-25g",
        "numa": 0,
        "cores": 30,
        "nicpci": "0000:41:00.0",
    },
]

# Public domain or IP of server
SERVERS = NODES[10:11]
# Public domain or IP of intemediate
INTNODES = []
# Public domain or IP of client and agents
CLIENTS = NODES[0:10]
# Public domain or IP of client
CLIENT = CLIENTS[0]
AGENTS = CLIENTS[1:]

# Public domain or IP of monitor
MONITOR = ""

# Username and SSH credential location to access
# the server, client, and agents via public IP
USERNAME = "bhaskar3"
KEY_LOCATION = "/users/bhaskar3/.ssh/id_rsa"

# Location of Shenango to be installed. With "", Shenango
# will be installed in the home direcotry
ARTIFACT_PARENT = ""

### End of config ###

ARTIFACT_PATH = ARTIFACT_PARENT
if ARTIFACT_PATH != "":
    ARTIFACT_PATH += "/"
ARTIFACT_PATH += "caladan-all-artifact"


