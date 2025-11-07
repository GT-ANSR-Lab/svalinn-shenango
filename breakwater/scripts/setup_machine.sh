#!/bin/bash
# run with sudo

pushd ..

# Build ksched.ko
cd ksched && make clean && make && cd ..

# Shenango setup
./scripts/setup_machine.sh

# turn on cstate
killall cstate
cd scripts
gcc cstate.c -o cstate
./cstate 0 &
cd ..

# Disable frequency scaling
echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Disable turbo boost
scaling_driver="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver)"
if [[ "$scaling_driver" == "intel_pstate" ]]; then
    echo 1 | tee /sys/devices/system/cpu/intel_pstate/no_turbo
elif [[ "$scaling_driver" == "acpi-cpufreq" ]]; then
    echo 0 | tee /sys/devices/system/cpu/cpufreq/boost
fi

popd
