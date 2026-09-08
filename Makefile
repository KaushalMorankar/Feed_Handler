CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
LDFLAGS_SIM   := -lpthread
LDFLAGS_AFXDP := -lpthread -lbpf
HOST_MULTIARCH ?= $(shell $(CC) -print-multiarch)
BPF_CFLAGS := -O2 -g -target bpf -Iinclude -I/usr/include/$(HOST_MULTIARCH)

BUILD := build

.PHONY: all sim afxdp clean test

all: sim

# ---- sim target: builds feed_handler in --mode sim and the traffic
#      generator. No libbpf, no clang, no root required. This is what
#      compiles and runs in this sandbox and in CI. ---------------------
sim: $(BUILD)/feed_handler_sim $(BUILD)/packet_gen

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/feed_handler_sim: src/feed_handler.c src/mdp3_parser.c src/seq_tracker.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS_SIM)

$(BUILD)/packet_gen: src/packet_gen.c | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

# ---- afxdp target: the real kernel-bypass deliverable. Requires:
#        - clang/llvm targeting bpf (to compile xdp_kern.c)
#        - libbpf-dev (to link xdp_loader.c)
#        - CAP_NET_ADMIN/root and a NIC to actually run against
#      `make afxdp` on a properly provisioned box builds:
#        - build/xdp_kern.o        (eBPF object, loaded into the kernel)
#        - build/feed_handler_afxdp (userspace app, -DUSE_AFXDP) --------
afxdp: $(BUILD)/xdp_kern.o $(BUILD)/feed_handler_afxdp

$(BUILD)/xdp_kern.o: src/xdp_kern.c | $(BUILD)
	clang $(BPF_CFLAGS) -c $< -o $@

$(BUILD)/feed_handler_afxdp: src/feed_handler.c src/mdp3_parser.c src/seq_tracker.c src/afxdp_socket.c src/xdp_loader.c | $(BUILD)
	$(CC) $(CFLAGS) -DUSE_AFXDP $^ -o $@ $(LDFLAGS_AFXDP)

# compile-check the AF_XDP socket module in isolation even without
# libbpf, since it has no libbpf dependency of its own
$(BUILD)/afxdp_socket.o: src/afxdp_socket.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

test: sim
	$(CC) $(CFLAGS) tests/test_seq_tracker.c src/seq_tracker.c -o $(BUILD)/test_seq_tracker
	./$(BUILD)/test_seq_tracker
	bash tests/run_sim_integration_test.sh

clean:
	rm -rf $(BUILD)
