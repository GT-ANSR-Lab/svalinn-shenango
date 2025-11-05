# Aspen

This Aspen repository is configured to run without any modifications on Gon (i.e., gon.cc.gatech.edu).

As of now Gon's NIC (ens255f1np1) has the following settings
1. NUMA node connected to - 0
2. DPDK Port - 1
3. PCI address - 0000:17:00.1
4. Directpath support - Yes, only for "qs", i.e., Queue Steering mode

Follow the instructions in `README.orig.md` to compile the repository. As compared to the original Aspen repository, this fork does not include the submodules for the applications and concord, as we do not need them.