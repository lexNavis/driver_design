#!/bin/bash
sudo insmod netdriver.ko
curl -4 --limit-rate 20k -s -o /dev/null https://nbg1-speed.hetzner.com/100MB.bin &
CURL_PID=$!
wget --limit-rate=20k -q -O /dev/null https://nbg1-speed.hetzner.com/100MB.bin &
WGET_PID=$!
sudo ../common/load_map.sh
cat /proc/netdriver
sudo rmmod netdriver