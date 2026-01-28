#!/usr/bin/python3

import math
import numpy as np

import csv


# ------------------------------------------------------
# Global parameters
# ------------------------------------------------------1.0
N = 45                          # number of arms (core counts 1..N)
M = 180000.0                    # true max bandwidth (unknown to TS)
M_NOISE = 3000.0                # Noise in bandwidth measurements
K = 16                          # saturation point (true environment)
T = 10000                       # time steps
REWARD_FN_TYPE = "simple"       # type of the reward function used

MAGNIFYER = 100.0                      # reward magnifyer
MU0 = 80.0                             # prior mean of the unknown true mean
SIGMA0_SQ = 625.0                      # prior variance of the unknown true mean
SIGMA_SQ = 25.0                        # known variance of the unknown rewards
mu = np.full(N, MU0)                   # posterior mean of the unknown true mean
sigma_sq = np.full(N, SIGMA0_SQ)       # posterior variance of the unknown true mean
rsum = np.zeros(N)                     # Sum of rewards for this arm
count = np.zeros(N)                    # number of times an arm was pulled
max_seen_bw = 0.0                      # maximum bandwidth seen so far

# ------------------------------------------------------
# True environment (controller does NOT know the scale M)
# ------------------------------------------------------
def memory_bandwidth(core):
    """
    True saturating bandwidth curve.
    Only here we use M (which TS doesn't know).
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
    TS does NOT know max bandwidth.
    Uses the max bandwidth *observed so far*.
    Reward = normalized bandwidth - normalized core usage.
    """
    global max_seen_bw

    if REWARD_FN_TYPE == "simple":
        ALPHA = 0.7
        norm_membw = membw_val / max_seen_bw
        norm_cpu = core / N
        reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cpu
        reward = reward * MAGNIFYER
    elif REWARD_FN_TYPE == "simple_flex":
        ALPHA = 0.8
        TARGET_MEMBW_UTIL = 0.75
        norm_membw = membw_val / max_seen_bw
        norm_membw = min(norm_membw, TARGET_MEMBW_UTIL) / TARGET_MEMBW_UTIL
        norm_cpu = core / N
        reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cpu
        reward = reward * MAGNIFYER
    elif REWARD_FN_TYPE == "hardgate":
        ALPHA = 0.8
        TARGET_MEMBW_UTIL = 0.75 # must be less than 1.0, else no penalty
        norm_membw = membw_val / max_seen_bw
        norm_cpu = core / N
        if norm_membw > TARGET_MEMBW_UTIL:
            reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cpu
        else:
            reward = ALPHA * norm_membw
        reward = reward * MAGNIFYER
    elif REWARD_FN_TYPE == "softgate":
        ALPHA = 0.8
        K = 10000.0
        TARGET_MEMBW_UTIL = 0.75 # must be less than 1.0, else no penalty
        norm_membw = membw_val / max_seen_bw
        norm_cpu = core / N
        sigmoid = 1.0 / (1.0 + np.exp(-K * (norm_membw - TARGET_MEMBW_UTIL)))
        reward = ALPHA * norm_membw - sigmoid * (1 - ALPHA) * norm_cpu
        reward = reward * MAGNIFYER
    else:
        reward = 0.0

    return reward


# ------------------------------------------------------
# Gaussian Thompson Sampling
# ------------------------------------------------------
def thompson_sampling():
    global mu, sigma_sq, count, max_seen_bw

    history = []

    for t in range(T):

        # Sample from the Gaussian posterior for each arm
        sampled_values = np.random.normal(mu, sigma_sq**0.5)

        # Choose action with highest sampled value
        action = np.argmax(sampled_values) + 1  # core count = 1..N

        # Get the memory bandwidth usage for this core
        membw_val = memory_bandwidth(action)

        # Update max observed bandwidth
        if membw_val > max_seen_bw:
            max_seen_bw = membw_val

        # Compute reward
        r = reward_function(action, membw_val)

        # Update the posterior based on the new observation
        arm = action - 1
        rsum[arm] += r
        count[arm] += 1
        sigma_sq[arm] = 1.0 / (1.0/SIGMA0_SQ + count[arm]/SIGMA_SQ)
        mu[arm] = sigma_sq[arm] * (MU0/SIGMA0_SQ + rsum[arm]/SIGMA_SQ)

        # Save the history
        history.append((t, action, membw_val, r, max_seen_bw))

    return history


# ------------------------------------------------------
# Run experiment
# ------------------------------------------------------
if __name__ == "__main__":
    #
    # Run the simulation
    #
    history = thompson_sampling()
    best_arm = np.argmax(mu) + 1
    print("Estimated best core count:", best_arm)
    print("Posterior means (mu):", mu)
    print("Posterior stddevs (sigma):", sigma_sq**0.5)
    print("Final max bandwidth seen:", max_seen_bw)

    #
    # Dump the results
    #
    with open("ts_history.csv", "w", newline="") as f:
        writer = csv.writer(f)
        # header
        writer.writerow(["timestep", "action", "measured_bw", "reward", "max_seen_bw"])
        # data rows
        for row in history:
            writer.writerow(row)

    #
    # Dump final posterior means and stddevs for all arms
    #
    with open("ts_posteriors.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["arm", "posterior_mean_mu", "posterior_stddev_sigma"])

        for arm in range(N):
            writer.writerow([arm + 1, mu[arm], math.sqrt(sigma_sq[arm])])
