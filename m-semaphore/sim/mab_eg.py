#!/usr/bin/python3

import math
import numpy as np

import csv


# ------------------------------------------------------
# Global parameters
# ------------------------------------------------------
N = 45                          # number of arms (core counts 1..N)
M = 180000.0                    # true max bandwidth (unknown to controller)
M_NOISE = 3000.0                # Noise in bandwidth measurements
K = 16                          # saturation point (true environment)
T = 10000                       # time steps
REWARD_FN_TYPE = "simple"       # type of the reward function used

LOCAL_EXPLORATION = False
ALPHA = 0.8
OMEGA = 0.8
EPSILON = 0.05
avg_rewards = np.full(N, 0.0)          # learnt average reward for each arm
max_seen_bw = 0.0                      # maximum bandwidth seen so far

# ------------------------------------------------------
# True environment (controller does NOT know the scale M)
# ------------------------------------------------------
def memory_bandwidth(core):
    """
    True saturating bandwidth curve.
    Only here we use M (which controller doesn't know).
    """
    # Deterministic base behavior
    if core >= K:
        true_bw = M
    else:
        true_bw = M * (core / K)

    # Add noise (Gaussian)
    noise = np.random.normal(loc=0.0, scale=M_NOISE)

    # Ensuring bandwidth doesn't go below zero
    return max(true_bw + noise, 0.0)


# ------------------------------------------------------
# Reward function (controller sees only membw_val)
# ------------------------------------------------------
def reward_function(core, membw_val):
    """
    Controller does NOT know max bandwidth.
    Uses the max bandwidth *observed so far*.
    Reward = normalized bandwidth - normalized core usage.
    """
    global max_seen_bw

    if REWARD_FN_TYPE == "simple":
        norm_membw = membw_val / max_seen_bw
        norm_cpu = core / N
        reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cpu
    elif REWARD_FN_TYPE == "simple_flex":
        TARGET_MEMBW_UTIL = 0.75
        norm_membw = membw_val / max_seen_bw
        norm_membw = min(norm_membw, TARGET_MEMBW_UTIL) / TARGET_MEMBW_UTIL
        norm_cpu = core / N
        reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cpu
    elif REWARD_FN_TYPE == "hardgate":
        TARGET_MEMBW_UTIL = 0.98 # must be less than 1.0, else no penalty
        norm_membw = membw_val / max_seen_bw
        norm_cpu = core / N
        if norm_membw > TARGET_MEMBW_UTIL:
            reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cpu
        else:
            reward = ALPHA * norm_membw
    elif REWARD_FN_TYPE == "softgate":
        K = 10000.0
        TARGET_MEMBW_UTIL = 0.98 # must be less than 1.0, else no penalty
        norm_membw = membw_val / max_seen_bw
        norm_cpu = core / N
        sigmoid = 1.0 / (1.0 + np.exp(-K * (norm_membw - TARGET_MEMBW_UTIL)))
        reward = ALPHA * norm_membw - sigmoid * (1 - ALPHA) * norm_cpu
    else:
        reward = 0.0

    return reward


# ------------------------------------------------------
# Epsilon-greedy MAB Controller
# ------------------------------------------------------
def epsilon_greedy_control():
    global avg_rewards, max_seen_bw

    history = []

    # Start with core count of 1
    action = 1

    for t in range(T):

        # Get the memory bandwidth usage for this core
        membw_val = memory_bandwidth(action)

        # Update max observed bandwidth
        if membw_val > max_seen_bw:
            max_seen_bw = membw_val

        # Compute reward
        r = reward_function(action, membw_val)

        # Update learned reward using EWMA
        arm = action - 1
        avg_rewards[arm] = (1.0 - OMEGA) * avg_rewards[arm] + OMEGA * r

        # Save history
        history.append((t, action, membw_val, r, max_seen_bw))

        # Pick the current best arm
        best_arm = np.argmax(avg_rewards) + 1  # convert to 1-based core count

        # Epsilon-greedy arm selection
        if np.random.rand() < EPSILON:
            if LOCAL_EXPLORATION:
                # Local exploration: pick a neighboring arm of the current best
                candidates = []
                if best_arm > 1:
                    candidates.append(best_arm - 1)
                if best_arm < N:
                    candidates.append(best_arm + 1)
                # If no neighbors exist, stay at best_arm
                action = np.random.choice(candidates) if candidates else best_arm
            else:
                candidates = [arm for arm in range(1, N + 1) if arm != best_arm]
                action = np.random.choice(candidates)
        else:
            # Exploitation
            action = best_arm

    return history


# ------------------------------------------------------
# Run experiment
# ------------------------------------------------------
if __name__ == "__main__":
    #
    # Run the simulation
    #
    history = epsilon_greedy_control()
    best_arm = np.argmax(avg_rewards) + 1
    print("Estimated best core count:", best_arm)
    print("Average rewards:", avg_rewards)
    print("Final max bandwidth seen:", max_seen_bw)

    #
    # Dump the results
    #
    with open("eg_history.csv", "w", newline="") as f:
        writer = csv.writer(f)
        # header
        writer.writerow(["timestep", "action", "measured_bw", "reward", "max_seen_bw"])
        # data rows
        for row in history:
            writer.writerow(row)

    #
    # Dump learnt average rewards for all cores
    #
    with open("eg_learnt_rewards.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["arm", "avg_reward"])

        for arm in range(N):
            writer.writerow([arm + 1, avg_rewards[arm]])
