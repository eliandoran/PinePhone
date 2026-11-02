include $(sort $(wildcard $(BR2_EXTERNAL_PINEPHONE_PATH)/package/*/*.mk))

# perf (linux-tools) builds Rust test workloads when the host has rustc, which
# fails without a Rust target for aarch64
PERF_MAKE_FLAGS += NO_RUST=1
