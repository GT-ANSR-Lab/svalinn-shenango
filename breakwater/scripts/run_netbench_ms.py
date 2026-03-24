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
RUNTIME_PMC_INFO_POLL_INTERVAL = 0

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
NUM_CONNS = 144
# Total number of server machines
NUM_SERVERS = len(SERVERS)
# Total number of client machines (master and agents)
NUM_CLIENTS = len(CLIENTS)
# Total number of agents
NUM_AGENTS = len(AGENTS)

# List of offered load
NUM_SAMPLES = 10
MAX_OFFERED_LOAD = 400000
OFFERED_LOADS = [int((i+1) * (MAX_OFFERED_LOAD/NUM_SAMPLES)) for i in range(NUM_SAMPLES)]
LOAD_SHIFT = False
if LOAD_SHIFT:
    OFFERED_LOADS = [0]  # Dummy value


# Network RTT on the testbed
NET_RTT = 10
# SLO = 10 * (average RPC processing time + network RTT)
SLO = 650

# Netbench settings
CPU_BOUND_WORK_ITR = 5000
MEM_BOUND_WORK_ITR = 25
CPU_BOUND_REQ_PCNT = 50

# Provides the opportunity to replace the files in all the machines
# Helps in testing quickly by updating the required files
FILES_TO_REPLACE = [
    # {
    #     "src": "src/netbench.cc",
    #     "dst": "src/netbench.cc",
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

# connection to servers
server_conns = []
for server in SERVERS:
    server_conn = paramiko.SSHClient()
    server_conn.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    server_conn.connect(hostname = server["name"], username = USERNAME, pkey = k)
    server_conns.append(server_conn)

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
cmd = "sudo pkill -9 netbench_ms"
execute_remote(server_conns + [client_conn] + agent_conns, cmd, True, False)
cmd = "sudo pkill -9 iokerneld"
execute_remote(server_conns + [client_conn] + agent_conns,
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
cmd = "sed -i \'s/#define IOKERNEL_PMC_INFO_POLL_INTERVAL.*/#define IOKERNEL_PMC_INFO_POLL_INTERVAL\\t\\t\\t{:d}/g\'"\
        " ~/{}/iokernel/defs.h".format(RUNTIME_PMC_INFO_POLL_INTERVAL, ARTIFACT_PATH)
execute_remote(server_conns, cmd, True)
cmd = "sed -i \'s/#define IOKERNEL_PMC_INFO_POLL_INTERVAL.*/#define IOKERNEL_PMC_INFO_POLL_INTERVAL\\t\\t\\t0/g\'"\
        " ~/{}/iokernel/defs.h".format(ARTIFACT_PATH)
execute_remote([client_conn] + agent_conns, cmd, True)

# Set the memory semaphore parameters
print("Updating the memory semaphore parameters...")
cmd = "sed -i 's/\\(CTL_DELAY_US[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_ts_impl.hpp"\
      .format(MSEM_CTL_DELAY_US, ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote(server_conns, cmd, True)
cmd = "sed -i 's/\\(ALPHA[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_ts_impl.hpp"\
      .format(MSEM_ALPHA, ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote(server_conns, cmd, True)
cmd = "sed -i 's/\\(TARGET_NORM_MEMBW[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_ts_impl.hpp"\
      .format(MSEM_TARGET_NORM_MEMBW, ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote(server_conns, cmd, True)
cmd = "sed -i 's/\\(EXPLR_PROB[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_ts_impl.hpp"\
      .format(MSEM_EXPLR_PROB, ARTIFACT_PATH, ARTIFACT_PATH)
execute_remote(server_conns, cmd, True)
cmd = "sed -i 's/\\(REWARD_EWMA_WEIGHT[[:space:]]*=[[:space:]]*\\)[0-9]\\+\\(\\.[0-9]\\+\\)\\?/\\1{}/'"\
      " ~/{}/m-semaphore/inc/m_semaphore_mab_eg_impl.hpp"\
      .format(MSEM_REWARD_EWMA_WEIGHT, ARTIFACT_PATH)
execute_remote(server_conns, cmd, True)

# Generating config files
print("Generating Caladan config files...")
for i in range(NUM_SERVERS):
    generate_caladan_config(server_conns[i], True, True,
                            SERVERS[i]["ip"], SERVERS[i]["netmask"], SERVERS[i]["gateway"], SERVERS[i]["cores"],
                            RUNTIME_ENABLE_DIRECTPATH,
                            RUNTIME_SPIN_SERVER, RUNTIME_DISABLE_WATCHDOG)
generate_caladan_config(client_conn, False, True,
                        CLIENT["ip"], CLIENT["netmask"], CLIENT["gateway"], CLIENT["cores"],
                        RUNTIME_ENABLE_DIRECTPATH, True, False)
for i in range(NUM_AGENTS):
    generate_caladan_config(agent_conns[i], False, True,
                            AGENTS[i]["ip"], AGENTS[i]["netmask"], AGENTS[i]["gateway"], AGENTS[i]["cores"],
                            RUNTIME_ENABLE_DIRECTPATH, True, False)

# Rebuild Caladan
print("Building Caladan...")
cmd = "cd ~/{} && make clean && make && make -C bindings/cc"\
        .format(ARTIFACT_PATH)
execute_remote(server_conns + [client_conn] + agent_conns, cmd, True)

# Build Breakwater
print("Building Breakwater...")
cmd = "cd ~/{}/breakwater && make clean && make && make -C bindings/cc"\
        .format(ARTIFACT_PATH)
execute_remote(server_conns + [client_conn] + agent_conns, cmd, True)

# build memory semaphore library
print("Building MemSemaphore...")
cmd = "cd ~/{}/m-semaphore && make clean && make all".\
    format(ARTIFACT_PATH)
execute_remote(server_conns + [client_conn] + agent_conns, cmd, True)

# Build netbench
print("Building netbench...")
cmd = "cd ~/{}/breakwater/apps/netbench/src && make clean && make"\
        .format(ARTIFACT_PATH)
execute_remote(server_conns + [client_conn] + agent_conns, cmd, True)

# Execute IOKernel
iok_sessions = []
print("Starting IOKernel on clients and server...")
for i in range(NUM_SERVERS):
    cmd = "cd ~/{} && sudo ./iokerneld {} nobw numanode {} nicpci {} >/dev/null 2>&1"\
          .format(ARTIFACT_PATH, RUNTIME_SCHED, SERVERS[i]["numa"],
                  SERVERS[i]["nicpci"])
    iok_sessions += execute_remote([server_conns[i]], cmd, False)
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
execute_remote(server_conns + [client_conn] + agent_conns, cmd, True, False)

# Create output directory for this test run
curr_date = datetime.now().strftime("%m_%d_%Y")
curr_time = datetime.now().strftime("%H-%M-%S")
output_dir = "outputs/netbench/{}/{}".format(curr_date, curr_time)
if not os.path.isdir(output_dir):
   os.makedirs(output_dir)

# Generate the load
for offered_load in OFFERED_LOADS:

    if not LOAD_SHIFT:
        print("Load = {:d}".format(offered_load))
    else:
        print("Starting the load shift experiment")

    # Start netbench server (prepopulate with data)
    print("\tStarting netbench server...")
    cmd = " cd ~/{} && sudo ./breakwater/apps/netbench/build/netbench_ms {} server.config"\
          " server {} >stdout.out 2>&1"\
          .format(ARTIFACT_PATH, OVERLOAD_ALG, "msem" if MSEM_ENABLE else "no_msem")
    server_sessions = execute_remote(server_conns, cmd, False)

    # This sleep should be enough to complete the prepopulation at the server
    sleep(5)

    # Generate a string of IPs and connection info for all server replicas
    server_ips_arg = " ".join(["{} 1".format(server["ip"]) for server in SERVERS])

    # Start netbench client
    print("\tExecuting netbench client...")
    client_agent_sessions = []
    cmd = "cd ~/{} && sudo ./breakwater/apps/netbench/build/netbench_ms {} client.config client"\
          " {} {} {} {} {} {} {} {} {} >stdout.out 2>&1"\
          .format(ARTIFACT_PATH, OVERLOAD_ALG, NUM_CONNS,
                  CPU_BOUND_WORK_ITR, MEM_BOUND_WORK_ITR, CPU_BOUND_REQ_PCNT,
                  SLO, NUM_AGENTS, offered_load,
                  "load_shift" if LOAD_SHIFT else "no_load_shift", server_ips_arg)
    client_agent_sessions += execute_remote([client_conn], cmd, False)
    sleep(3)

    # Start netbench agents
    print("\tExecuting netbench agents...")
    cmd = "cd ~/{} && sudo ./breakwater/apps/netbench/build/netbench_ms {} client.config agent {}"\
          " >stdout.out 2>&1".format(ARTIFACT_PATH, OVERLOAD_ALG, CLIENT["ip"])
    client_agent_sessions += execute_remote(agent_conns, cmd, False)

    # Wait for some traffic to begin
    sleep(2)

    # Wait for client and agents
    print("\tWaiting for netbench client and agents...")
    for client_agent_session in client_agent_sessions:
        client_agent_session.recv_exit_status()

    # Kill clients and server
    print("\tKilling netbench clients and server...")
    cmd = "sudo pkill -9 netbench_ms"
    execute_remote(server_conns + [client_conn] + agent_conns, cmd, True, False)

    sleep(1)

# Kill IOKernel
cmd = "sudo pkill -9 iokerneld"
execute_remote(server_conns + [client_conn] + agent_conns, cmd, True)

# Wait for IOKernel sessions
for iok_session in iok_sessions:
    iok_session.recv_exit_status()

# Close connections
for server_conn in server_conns:
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
header = "num_threads,offered_load,throughput,cpu_bound_req_throughput,"\
         "mem_bound_req_throughput,goodput,cpu_bound_req_goodput,mem_bound_req_goodput,min,mean,p50,cpu_bound_req_p50,"\
         "mem_bound_req_p50,p90,cpu_bound_req_p90,mem_bound_req_p90,p99,"\
         "cpu_bound_req_p99,mem_bound_req_p99,p999,p9999,max,reject_min,"\
         "reject_mean,reject_p50,reject_p99,p1_credit,mean_credit,p99_credit,"\
         "p1_q,mean_q,p99_q,mean_stime,p99_stime,client:min_tput,client:max_tput,"\
         "client:ecredit_rx_pps,client:cupdate_tx_pps,client:resp_rx_pps,client:req_tx_pps,"\
         "client:credit_expired_cps,client:req_dropped_rps,"
for i in range(NUM_SERVERS):
    header += "server{0}:cpu_bound_req_throughput,server{0}:mem_bound_req_throughput,"\
               "server{0}:cpu_bound_req_goodput,server{0}:mem_bound_req_goodput,"\
               "server{0}:cpu,server{0}:membw,server{0}:power,server{0}:rx_pps,server{0}:tx_pps,"\
               "server{0}:rx_bps,server{0}:tx_bps,server{0}:rx_drops_pps,server{0}:rx_ooo_pps,"\
               "server{0}:cupdate_rx_pps,server{0}:ecredit_tx_pps,server{0}:credit_tx_cps,"\
               "server{0}:req_rx_pps,server{0}:req_drop_rate,server{0}:resp_tx_pps,server{0}:reject_mean,"\
               "server{0}:reject_min,server{0}:reject_p50,server{0}:reject_p99,"\
               .format(i)
cmd = "echo \"{}\" > {}/output.csv".format(header, output_dir)
execute_local(cmd)
cmd = "cat output.csv >> {}/output.csv".format(output_dir)
execute_local(cmd)
cmd = "cp {}/output.csv ./output.csv".format(output_dir)
execute_local(cmd)

# Collect the all tasks file for load shift experiment
if LOAD_SHIFT:
    cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no {}@{}:~/{}/all_tasks.csv ./"\
          " >/dev/null".format(KEY_LOCATION, USERNAME, CLIENT["name"], ARTIFACT_PATH)
    execute_local(cmd)

# Collect the stdout from the server
print("Collecting stdout of server...")
for i in range(NUM_SERVERS):
    cmd = "rsync -azh --info=progress2 -e \"ssh -i {} -o StrictHostKeyChecking=no -o"\
          " UserKnownHostsFile=/dev/null\" {}@{}:~/{}/stdout.out {}/stdout.out.server{} >/dev/null"\
          .format(KEY_LOCATION, USERNAME, SERVERS[i]["name"], ARTIFACT_PATH, output_dir, i)
    execute_local(cmd)

# Collect the stdout from the client
print("Collecting stdout of client...")
cmd = "rsync -azh --info=progress2 -e \"ssh -i {} -o StrictHostKeyChecking=no -o"\
        " UserKnownHostsFile=/dev/null\" {}@{}:~/{}/stdout.out {}/stdout.out.client >/dev/null"\
        .format(KEY_LOCATION, USERNAME, CLIENT["name"], ARTIFACT_PATH, output_dir)
execute_local(cmd)

# Collect the the Caladan configs
print("Collecting the Caladan configs for server and client...")
for i in range(NUM_SERVERS):
    cmd = "scp -P 22 -i {} -o StrictHostKeyChecking=no {}@{}:~/{}/server.config {}/server{}.config"\
          " >/dev/null".format(KEY_LOCATION, USERNAME, SERVERS[i]["name"], ARTIFACT_PATH, output_dir, i)
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
run_config += "CPU-bound workload per-request iterations: {}\n".format(CPU_BOUND_WORK_ITR)
run_config += "Memory-bound workload per-request iterations: {}\n".format(MEM_BOUND_WORK_ITR)
run_config += "CPU-bound request percentage: {}\n".format(CPU_BOUND_REQ_PCNT)
cmd = "echo \"{}\" > {}/run.config".format(run_config, output_dir)
execute_local(cmd)

print("Output dumped at {}".format(output_dir))
print("Done.")
