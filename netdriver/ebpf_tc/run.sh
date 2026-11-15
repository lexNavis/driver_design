#!/bin/bash

sudo rm -rf /sys/fs/bpf/tc_prog
sudo mkdir -p /sys/fs/bpf/tc_prog

sudo clang -O2 -g -target bpf -c tc_prog.bpf.c -o tc_prog.bpf.o

sudo bpftool prog loadall tc_prog.bpf.o /sys/fs/bpf/tc_prog

sudo tc qdisc del dev eth0 clsact 2>/dev/null || true
sudo tc qdisc add dev eth0 clsact

sudo tc filter del dev eth0 ingress 2>/dev/null || true
sudo tc filter del dev eth0 egress 2>/dev/null || true
sudo tc filter add dev eth0 ingress bpf da pinned /sys/fs/bpf/tc_prog/prog_ingress
sudo tc filter add dev eth0 egress bpf da pinned /sys/fs/bpf/tc_prog/prog_egress

curl -4 --limit-rate 20k -s -o /dev/null https://nbg1-speed.hetzner.com/100MB.bin &
CURL_PID=$!
wget --limit-rate=20k -q -O /dev/null https://nbg1-speed.hetzner.com/100MB.bin &
WGET_PID=$!
./show_table.sh
