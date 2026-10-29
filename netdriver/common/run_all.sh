#!/bin/bash
cd "$(dirname "$0")"

rm -rf ../results
mkdir -p ../results

# Запускаем долгие соединения ОДИН раз (висят 20 секунд)
echo "=== Запуск трафика ==="
curl -4 --limit-rate 20k -s -o /dev/null https://nbg1-speed.hetzner.com/100MB.bin &
CURL_PID=$!
wget --limit-rate=20k -q -O /dev/null https://nbg1-speed.hetzner.com/100MB.bin &
WGET_PID=$!
timeout 20 openssl s_client -connect nbg1-speed.hetzner.com:443 -quiet < /dev/null > /dev/null 2>&1 &
SSL_PID=$!
sleep 3   # даём соединениям установиться

# 1. Netfilter (быстро)
echo "=== netfilter ==="
cd ../netfilter
sudo insmod netdriver.ko
sudo ../common/load_map.sh
sudo cat /proc/netdriver > ../results/netfilter.txt
sudo rmmod netdriver
cd ../common

# 2. AF_PACKET
echo "=== af_packet ==="
cd ../af_packet
sudo ../common/build_map_afpacket.sh
sudo ./af_packet > /tmp/af_out.txt 2>&1 &
sleep 2
AF_PID=$(pidof af_packet)
sudo kill -USR2 "$AF_PID"
sleep 1
sudo kill -USR1 "$AF_PID"
sleep 1
sudo kill -INT "$AF_PID"
wait "$AF_PID" 2>/dev/null
cp /tmp/af_out.txt ../results/af_packet.txt
cd ../common

# 3. eBPF
echo "=== ebpf ==="
cd ../ebpf_tc
./run.sh
sleep 2
./show_table.sh > ../results/ebpf.txt
cd ../common

# Убиваем трафик
kill "$CURL_PID" "$WGET_PID" "$SSL_PID" 2>/dev/null

# Сводка
echo "=== comparison ==="
for f in ../results/*.txt; do
    echo "--- $(basename "$f") ---"
    cat "$f"
    echo ""
done