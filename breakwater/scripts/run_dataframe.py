#!/usr/bin/env python3

import paramiko
import os
from time import sleep, time
from util import *
from config_remote import *
from datetime import datetime
import random
import sys
import pandas as pd

################################
### Experiemnt Configuration ###
################################

# Core allocator settings
RUNTIME_SCHED = "simple"
RUNTIME_SCHED_THRESHOLD = 5
RUNTIME_SPIN_SERVER = True
RUNTIME_ENABLE_DIRECTPATH = True
RUNTIME_DISABLE_WATCHDOG = False
RUNTIME_MEM_INFO_POLL_INTERVAL = 0

# Overload controller settings
OVERLOAD_ALG = "pcc"

# Memory semaphore settings
MSEM_ENABLE = True
MSEM_CTL_DELAY_US = 500
MSEM_ALPHA = 0.6
MSEM_TARGET_NORM_MEMBW = 1.0
MSEM_EXPLR_PROB = 0.3
MSEM_REWARD_EWMA_WEIGHT = 0.8

# Total number of client connections
NUM_CONNS = 100
# Total number of client machines (master and agents)
NUM_CLIENTS = len(CLIENTS)
# Total number of agents
NUM_AGENTS = len(AGENTS)

# List of offered load
NUM_SAMPLES = 10
MAX_OFFERED_LOAD = 2000000
OFFERED_LOADS = [int((i+1) * (MAX_OFFERED_LOAD/NUM_SAMPLES)) for i in range(NUM_SAMPLES)]

# Network RTT on the testbed
NET_RTT = 10
# SLO = 10 * (average RPC processing time + network RTT)
SLO = 900

# Dataframe settings
DF_CSV_DATASET_PATH = "~/{}/breakwater/apps/dataframe/data/eur_usd_fx.csv".format(ARTIFACT_PATH)
DF_OPS_MIX = "max:99,rmv:0,kmeans:0,ppo:0,decom:0,decay:1,ad:0"

# Client and Server Caladan Runtime IP addresses
SERVER_RUNTIME_IP = "192.168.1.200"
CLIENT_RUNTIME_IP = "192.168.1.100"
AGENT_RUNTIME_IPS = [ "192.168.1." + str(101 + i) for i in range(NUM_AGENTS)]
RUNTIME_NETMASK = "255.255.255.0"
RUNTIME_GATEWAY = "192.168.1.1"

# Provides the opportunity to replace the files in all the machines
# Helps in testing quickly by updating the required files
FILES_TO_REPLACE = [
    # {
    #     "src": "dataframe_client.cc",
    #     "dst": "dataframe_client.cc",
    # },
    # {
    #     "src": "dataframe_server.cc",
    #     "dst": "dataframe_server.cc",
    # },
]

############################
### End of configuration ###
############################

##################################
### Function definitions start ###
##################################

# Function to generate the workload-specific shenango config
def generate_caladan_config(conn, is_server, latency_critical,
                            ip, netmask, gateway, num_cores,
                            directpath, spin, disable_watchdog):
    config_name = ""
    config_string = ""

    if is_server:
        config_name = "server.config"
    else:
        config_name = "client.config"
    config_string = "host_addr {}".format(ip)\
                    + "\nhost_netmask {}".format(netmask)\
                    + "\nhost_gateway {}".format(gateway)\
                    + "\nruntime_kthreads {:d}".format(num_cores)
    if is_server:
        if latency_critical:
            config_string += "\nruntime_priority lc"
        else:
            config_string += "\nruntime_priority be"
        config_string += "\nruntime_qdelay_us {:d}".format(RUNTIME_SCHED_THRESHOLD)
    if spin:
        config_string += "\nruntime_spinning_kthreads {:d}".format(num_cores)
    else:
        config_string += "\nruntime_spinning_kthreads 0"
    if directpath:
        config_string += "\nenable_directpath qs"
    if disable_watchdog:
        config_string += "\ndisable_watchdog 1"

    cmd = "cd ~/{} && echo \"{}\" > {} "\
            .format(ARTIFACT_PATH, config_string, config_name)

    return execute_remote([conn], cmd, True)

################################
### Function definitions end ###
################################

k = paramiko.RSAKey.from_private_key_file(KEY_LOCATION)

# connection to server
server_conn = paramiko.SSHClient()
server_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
server_conn.connect(hostname = SERVERS[0]["name"], username = USERNAME, pkey = k)

# connection to client
client_conn = paramiko.SSHClient()
client_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client_conn.connect(hostname = CLIENT["name"], username = USERNAME, pkey = k)

# connections to agents
agent_conns = []
for agent in AGENTS:
    agent_conn = paramiko.SSHClient()
    agent_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    agent_conn.connect(hostname = agent["name"], username = USERNAME, pkey = k)
    agent_conns.append(agent_conn)

# Clean-up environment
print("Cleaning up machines...")
cmd = "sudo pkill -f -9 dataframe_server"
execute_remote([server_conn], cmd, True, False)
cmd = "sudo pkill -f -9 dataframe_client"
execute_remote([client_conn] + agent_conns, cmd, True, False)
cmd = "sudo pkill -f -9 iokerneld"
execute_remote([server_conn, client_conn] + agent_conns,
               cmd, True, False)
sleep(1)

# Distribuing config files
print("Distributing configs...")
for node in NODES:
    cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no ./ovld_configs/*.h"\
          " {}@{}:~/{}/breakwater/src/ >/dev/null"\
          .format(KEY_LOCATION, USERNAME, node["name"], ARTIFACT_PATH)
    execute_local(cmd)

# Replace the frequently files
print("Replacing files...")
for fil in FILES_TO_REPLACE:
    for node in NODES:
        cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no ./{}"\
              " {}@{}:~/{}/{} >/dev/null"\
              .format(KEY_LOCATION, fil["src"], USERNAME,
                      node["name"], ARTIFACT_PATH, fil["dst"])
        execute_local(cmd)

# Set the memory info update frequency
print("Updating the memory information update frequency in Caladan...")
cmd = "sed -i \'s/#define IOKERNEL_MEM_INFO_POLL_INTERVAL.*/#define IOKERNEL_MEM_INFO_POLL_INTERVAL\\t\\t\\t{:d}/g\'"\
        " ~/{}/iokernel/defs.h".format(RUNTIME_MEM_INFO_POLL_INTERVAL, ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)
cmd = "sed -i \'s/#define IOKERNEL_MEM_INFO_POLL_INTERVAL.*/#define IOKERNEL_MEM_INFO_POLL_INTERVAL\\t\\t\\t0/g\'"\
        " ~/{}/iokernel/defs.h".format(ARTIFACT_PATH)
execute_remote([client_conn] + agent_conns, cmd, True)

# Set the memory semaphore parameters
print("Updating the memory semaphore parameters...")
cmd = "sed -i 's/\\(CTL_DELAY_US[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_ts_impl.hpp"\
      .format(MSEM_CTL_DELAY_US, ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)
cmd = "sed -i 's/\\(ALPHA[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_ts_impl.hpp"\
      .format(MSEM_ALPHA, ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)
cmd = "sed -i 's/\\(TARGET_NORM_MEMBW[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_ts_impl.hpp"\
      .format(MSEM_TARGET_NORM_MEMBW, ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)
cmd = "sed -i 's/\\(EXPLR_PROB[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_ts_impl.hpp"\
      .format(MSEM_EXPLR_PROB, ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)
cmd = "sed -i 's/\\(REWARD_EWMA_WEIGHT[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      .format(MSEM_REWARD_EWMA_WEIGHT, ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)


# Generating config files
print("Generating Caladan config files...")
generate_caladan_config(server_conn, True, True,
                        SERVER_RUNTIME_IP, RUNTIME_NETMASK, RUNTIME_GATEWAY, SERVERS[0]["cores"],
                        RUNTIME_ENABLE_DIRECTPATH, RUNTIME_SPIN_SERVER, RUNTIME_DISABLE_WATCHDOG)
generate_caladan_config(client_conn, False, True,
                        CLIENT_RUNTIME_IP, RUNTIME_NETMASK, RUNTIME_GATEWAY, CLIENT["cores"],
                        RUNTIME_ENABLE_DIRECTPATH, True, False)
for i in range(NUM_AGENTS):
    generate_caladan_config(agent_conns[i], False, True,
                            AGENT_RUNTIME_IPS[i], RUNTIME_NETMASK, RUNTIME_GATEWAY, AGENTS[i]["cores"],
                            RUNTIME_ENABLE_DIRECTPATH, True, False)

# Rebuild Caladan
print("Building Caladan...")
cmd = "cd ~/{} && make clean && make && make -C bindings/cc"\
        .format(ARTIFACT_PATH)
execute_remote([server_conn, client_conn] + agent_conns, cmd, True)

# Build Breakwater
print("Building Breakwater...")
cmd = "cd ~/{}/breakwater && make clean && make && make -C bindings/cc"\
        .format(ARTIFACT_PATH)
execute_remote([server_conn, client_conn] + agent_conns, cmd, True)

# build memory semaphore library
print("Building MemSemaphore...")
cmd = "cd ~/{}/m-semaphore && make clean && make all".\
    format(ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)

# Build DataFrame server
print("Building DataFrame server...")
cmd = "cd ~/{}/breakwater/apps/dataframe && make clean && make server"\
        .format(ARTIFACT_PATH)
execute_remote([server_conn], cmd, True)

# Build DataFrame client
print("Building DataFrame client...")
cmd = "cd ~/{}/breakwater/apps/dataframe && make clean && make client"\
        .format(ARTIFACT_PATH)
execute_remote([client_conn] + agent_conns, cmd, True)

# Execute IOKernel
iok_sessions = []
print("Starting IOKernel on clients and server...")
cmd = "cd ~/{} && sudo ./iokerneld {} nobw numanode {} nicpci {} >/dev/null 2>&1"\
      .format(ARTIFACT_PATH, RUNTIME_SCHED, SERVERS[0]["numa"],
              SERVERS[0]["nicpci"])
iok_sessions += execute_remote([server_conn], cmd, False)
cmd = "cd ~/{} && sudo ./iokerneld {} nobw numanode {} nicpci {} >/dev/null 2>&1"\
      .format(ARTIFACT_PATH, RUNTIME_SCHED, CLIENT["numa"],
              CLIENT["nicpci"])
iok_sessions += execute_remote([client_conn], cmd, False)
for i in range(NUM_AGENTS):
    cmd = "cd ~/{} && sudo ./iokerneld {} nobw numanode {} nicpci {} >/dev/null 2>&1"\
          .format(ARTIFACT_PATH, RUNTIME_SCHED, AGENTS[i]["numa"],
                  AGENTS[i]["nicpci"])
    iok_sessions += execute_remote([agent_conns[i]], cmd, False)
sleep(5)

# Clean old test output files
print("Removing old output files...")
cmd = "rm ~/{0}/stdout.out ~/{0}/output.csv ~/{0}/output.json"\
      " ~/{0}/membw.csv >/dev/null 2>&1".format(ARTIFACT_PATH)
execute_remote([server_conn, client_conn] + agent_conns, cmd, True, False)

# Create output directory for this test run
curr_date = datetime.now().strftime("%m_%d_%Y")
curr_time = datetime.now().strftime("%H-%M-%S")
output_dir = "outputs/dataframe/{}/{}".format(curr_date, curr_time)
if not os.path.isdir(output_dir):
   os.makedirs(output_dir)

# Generate load
for offered_load in OFFERED_LOADS:

    print("Load = {:d}".format(offered_load))

    # Start DataFrame server
    print("\tStarting DataFrame server...")
    cmd = "cd ~/{} && sudo ./breakwater/apps/dataframe/build/dataframe_server --config_path server.config"\
          " --ovld_cntl_algo {} --port 8001 --csv_path {}"\
          " {} >stdout.out 2>&1"\
          .format(ARTIFACT_PATH, OVERLOAD_ALG, DF_CSV_DATASET_PATH,
                  "--use_msem" if MSEM_ENABLE else "")
    server_session = execute_remote([server_conn], cmd, False)[0]

    # This sleep should be enough to allow the DataFrame initialization
    # as this step takes a while for large datasets
    sleep(15)

    # Start DataFrame client
    print("\tExecuting DataFrame client...")
    client_agent_sessions = []
    cmd = "cd ~/{} && sudo ./breakwater/apps/dataframe/build/dataframe_client client.config {} client"\
          " {} {} 8001 {} {} {} {} >stdout.out 2>&1"\
          .format(ARTIFACT_PATH, OVERLOAD_ALG, NUM_CONNS, SERVER_RUNTIME_IP,
                  DF_OPS_MIX, SLO, NUM_AGENTS, offered_load)
    client_agent_sessions += execute_remote([client_conn], cmd, False)
    sleep(3)

    # Start DataFrame agents
    print("\tExecuting DataFrame agents...")
    cmd = "cd ~/{} && sudo ./breakwater/apps/dataframe/build/dataframe_client client.config {} agent {}"\
          " >stdout.out 2>&1".format(ARTIFACT_PATH, OVERLOAD_ALG, CLIENT_RUNTIME_IP)
    client_agent_sessions += execute_remote(agent_conns, cmd, False)

    # Wait for some traffic to begin
    sleep(2)

    # Wait for client and agents
    print("\tWaiting for DataFrame client and agents...")
    for client_agent_session in client_agent_sessions:
        client_agent_session.recv_exit_status()

    # Kill clients
    print("\tKilling DataFrame clients...")
    cmd = "sudo pkill -f -9 dataframe_client"
    execute_remote([client_conn] + agent_conns, cmd, True, False)

    # Kill server
    print("\tKilling DataFrame server...")
    cmd = "sudo pkill -f -9 dataframe_server"
    execute_remote([server_conn], cmd, True, False)
    server_session.recv_exit_status()

    sleep(1)

# Kill IOKernel
cmd = "sudo pkill -f -9 iokerneld"
execute_remote([server_conn, client_conn] + agent_conns, cmd, True, False)

# Wait for IOKernel sessions
for iok_session in iok_sessions:
    iok_session.recv_exit_status()

# Close connections
server_conn.close()
client_conn.close()
for agent_conn in agent_conns:
    agent_conn.close()


print("Collecting outputs...")
# Collect the client stats
cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no {}@{}:~/{}/output.csv ./"\
        " >/dev/null".format(KEY_LOCATION, USERNAME, CLIENT["name"], ARTIFACT_PATH)
execute_local(cmd)
# Add the header to the raw output CSV file
header = "num_clients,offered_load,throughput,goodput,cpu"\
         ",min,mean,p50,p90,p99,p999,p9999,max,lmin,lmean,lp50,lp90,lp99,lp999"\
         ",lp9999,lmax,p1_win,mean_win,p99_win,p1_q,mean_q,p99_q,server:rx_pps"\
         ",server:tx_pps,server:rx_bps,server:tx_bps,server:rx_drops_pps,server:rx_ooo_pps"\
         ",server:winu_rx_pps,server:winu_tx_pps,server:win_tx_wps,server:req_rx_pps"\
         ",server:resp_tx_pps,client:min_tput,client:max_tput"\
         ",client:winu_rx_pps,client:winu_tx_pps,client:resp_rx_pps,client:req_tx_pps"\
         ",client:win_expired_wps,client:req_dropped_rps"\
         ",op_max_throughput,op_max_p50,op_max_p90,op_max_p99"\
         ",op_rmv_throughput,op_rmv_p50,op_rmv_p90,op_rmv_p99"\
         ",op_kmeans_throughput,op_kmeans_p50,op_kmeans_p90,op_kmeans_p99"\
         ",op_ppo_throughput,op_ppo_p50,op_ppo_p90,op_ppo_p99"\
         ",op_decom_throughput,op_decom_p50,op_decom_p90,op_decom_p99"\
         ",op_decay_throughput,op_decay_p50,op_decay_p90,op_decay_p99"\
         ",op_ad_throughput,op_ad_p50,op_ad_p90,op_ad_p99"
cmd = "echo \"{}\" > {}/output.csv".format(header, output_dir)
execute_local(cmd)
cmd = "cat output.csv >> {}/output.csv".format(output_dir)
execute_local(cmd)

# Collect the stdout from the server
print("Collecting stdout of server...")
cmd = "rsync -azh --info=progress2 -e \"ssh -i {} -o StrictHostKeyChecking=no -o"\
        " UserKnownHostsFile=/dev/null\" {}@{}:~/{}/stdout.out {}/stdout.out.server >/dev/null"\
        .format(KEY_LOCATION, USERNAME, SERVERS[0]["name"], ARTIFACT_PATH, output_dir)
execute_local(cmd)

# Collect the stdout from the client
print("Collecting stdout of client...")
cmd = "rsync -azh --info=progress2 -e \"ssh -i {} -o StrictHostKeyChecking=no -o"\
        " UserKnownHostsFile=/dev/null\" {}@{}:~/{}/stdout.out {}/stdout.out.client >/dev/null"\
        .format(KEY_LOCATION, USERNAME, CLIENT["name"], ARTIFACT_PATH, output_dir)
execute_local(cmd)

# Collect the the Caladan configs
print("Collecting the Caladan configs for server and client...")
cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no {}@{}:~/{}/server.config {}/"\
        " >/dev/null".format(KEY_LOCATION, USERNAME, SERVERS[0]["name"], ARTIFACT_PATH, output_dir)
execute_local(cmd)
cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no {}@{}:~/{}/client.config {}/"\
        " >/dev/null".format(KEY_LOCATION, USERNAME, CLIENT["name"], ARTIFACT_PATH, output_dir)
execute_local(cmd)

# Collect the config used by this test run
run_config = "runtime scheduler: {}\n".format(RUNTIME_SCHED)
run_config += "runtime scheduler queueing threshold: {} us\n".format(RUNTIME_SCHED_THRESHOLD)
run_config += "runtime scheduler spins cores for server: {}\n".format(RUNTIME_SPIN_SERVER)
run_config += "runtime enable directpath networking: {}\n".format(RUNTIME_ENABLE_DIRECTPATH)
run_config += "runtime disable watchdog: {}\n".format(RUNTIME_DISABLE_WATCHDOG)
run_config += "overload algorithm: {}\n".format(OVERLOAD_ALG)
run_config += "number of nodes: {}\n".format(len(NODES))
run_config += "number of client nodes: {}\n".format(len(CLIENTS))
run_config += "number of agent nodes: {}\n".format(len(AGENTS))
run_config += "number of connections: {}\n".format(NUM_CONNS)
run_config += "offered load (in RPS): {}\n".format(OFFERED_LOADS)
run_config += "RTT: {} us\n".format(NET_RTT)
run_config += "SLO: {} us\n".format(SLO)
run_config += "DataFrame CSV dataset path : {}\n".format(DF_CSV_DATASET_PATH)
run_config += "DataFrame operation mix : {}\n".format(DF_OPS_MIX)
cmd = "echo \"{}\" > {}/run.config".format(run_config, output_dir)
execute_local(cmd)

print("Output dumped at {}".format(output_dir))
print("Done.")
