## Notes
* The benchmarks are tested on `xl170` machines on Cloudlab.
* The image used to deploy the hosts is `urn:publicid:IDN+utah.cloudlab.us+image+creditrpc-PG0:breakwater-xl170-2`.
* You can use [this](https://www.cloudlab.us/instantiate.php?project=CreditRPC&profile=breakwater-compact&version=0) profile to deploy an experiment on Cloudlab.
* The setup scripts and test scripts assume that the workload is running on `xl170` machine on Cloudlab.

## Setup steps
* Deploy the nodes in Cloudlab using [this](https://www.cloudlab.us/instantiate.php?project=CreditRPC&profile=breakwater-compact&version=0) profile.
* SSH setup
  - Create SSH public-private key pairs on all the nodes using `ssh-keygen -t rsa`
  - Add the `~/.ssh/id_rsa.pub` of the node, where we are going to clone this repository and run the tests from (for e.g., node-1), to all the other nodes' `~/.ssh/authorized_keys`, including itself.
* Clone this repository (run on node-1)
  - `https://github.gatech.edu/HeteroBench/caladan-all.git`
* Update config_remote.py (run on node-1)
  - Update `NODES` with the DNS names or IP addresses of the nodes in your test setup.
  - `NODES[0]` is the server, rest of the nodes are the clients.
  - `NODES[1]` is the master client, rest of the clients are secondary clients (agents).
  - Update `USERNAME` with the one that has access to all the nodes.
  - Update `KEY_LOCATION` with the path to the private key `~/.ssh/id_rsa`.
* Perform general setup on all nodes (run on node-1)
  - `./setup_remote.py`
* Perform application-specific setup on all nodes (run on node-1)
  - `./setup_dataframe.py` or `./setup_rocksdb.py` or any other application-specific setup script.

## Running the tests
* Run the application-specific test script (run on node-1)
  - `./run_dataframe.py` or `./run_rocksdb.py` or any other application-specific test script.
  - This will save the output of the test run in the `outputs/<app_name>/<date>/<time>` directory.
