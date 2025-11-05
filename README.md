# Aspen

This Aspen repository is configured to run without any modifications on Gon (i.e., gon.cc.gatech.edu).

As of now Gon's NIC (ens255f1np1) has the following settings
1. NUMA node connected to - 0
2. DPDK Port - 1
3. PCI address - 0000:17:00.1
4. Directpath support - Yes, only for "qs", i.e., Queue Steering mode

Follow the instructions in `README.orig.md` to compile the repository. As compared to the original Aspen repository, this fork does not include the submodules for the applications and concord, as we do not need them.

Aspen has been tested on Gon machine, booted with Ubuntu 22.04 operating system with [Intel's custom Linux kernel for user interrupt system calls](https://github.com/intel/uintr-linux-kernel.git).

## How to install the Intel's UINTR Linux kernel

### Install basic dependencies
```
sudo apt update
sudo apt install -y build-essential libncurses-dev bison flex libssl-dev libelf-dev wget git
```

### Download the Intel's kernel
```
git clone https://github.com/intel/uintr-linux-kernel.git
git checkout uintr-next
```

If you dont want the Intel's kernel, you can download any other release by doing the following
```
cd /usr/src
sudo wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.8.tar.xz
sudo tar -xvf linux-6.8.tar.xz
```
The above code installs the linux version 6.8.0. You can find the appropriate Linux version as per your requirements.

### Build the Linux kernel
```
cd uintr-linux-kernel

cp /boot/config-$(uname -r) .config
make olddefconfig

# For uintr-linux-kernel, specifically, do the following
CONFIG_X86_USER_INTERRUPTS=y

scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS

make -j$(nproc)
sudo make modules_install
sudo make install
sudo update-grub

sudo reboot
```
