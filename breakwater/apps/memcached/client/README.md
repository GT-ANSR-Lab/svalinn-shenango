# Memcached benchmark client application for Shenango

## Sample Usage (for client)
```
$ sudo ./mcclient [overload_control] [shenango_config] client [# threads] [server_ip] [workload] [max_key_index] [slo] [npeers] [offered_load]
```
**[overload\_control]:** `breakwater` (for Breakwater), `dagor` (for DAGOR), `seda` (for SEDA), and `nocontrol` (for no overload control)\
**[shenango\_config]:** location of the Shenango configuration file\
**[\# threads]:** the number of threads generating load\
**[server\_ip]:** Memcached server IP\
**[workload]:** currently supports `SET` (100\% SET requests), `GET` (100\% GET requests), and `USR` (99.8\% GET and 0.2\% SET requests)\
**[max\_key\_index]:** the maximum number of keys\
**[slo]:** service level objective (in us)\
**[npeers]:** the number of agents\
**[offered\_load]:** target request generation rate (in reqs/s)

## Sample Usage (for agent)
```
$ sudo ./mcclient [overload_control] [shenango_config] agent [client_ip]
```
**[overload\_control]:** `breakwater` (for Breakwater), `dagor` (for DAGOR), `seda` (for SEDA), and `nocontrol` (for no overload control)\
**[shenango\_config]:** location of the Shenango configuration file\
**[client\_ip]:** client IP to connect to

## External Resources
[Breakwater artifact repo](https://github.com/inhocho89/breakwater-artifact) provides benchmark scripts.\
[Shenango](https://github.com/shenango/caladan) is required to compile this application.

