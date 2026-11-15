#!/bin/bash

sudo pkill -9 af_packet
sudo rm -f /tmp/af_packet_map.txt

sudo ./af_packet > /tmp/af_packet_out.txt 2>&1 &
sleep 2
AF_PID=$(pidof af_packet)
if [ -z "$AF_PID" ]; then
    echo "af_packet not running"
    exit 1
fi

curl -4 --limit-rate 50k -s -o /dev/null https://nbg1-speed.hetzner.com/100MB.bin &
CURL_PID=$!
wget --limit-rate=50k -q -O /dev/null https://nbg1-speed.hetzner.com/100MB.bin &
WGET_PID=$!

# Снимаем карту, ПОКА все живы
sudo ../common/build_map_afpacket.sh

# Даём трафику пройти
sleep 5

# Дамп
sudo kill -USR2 "$AF_PID"   # reload
sleep 1
sudo kill -USR1 "$AF_PID"   # dump
sleep 1
sudo kill -INT "$AF_PID"
wait "$AF_PID" 2>/dev/null

# Убиваем оставшиеся процессы
kill "$CURL_PID" "$WGET_PID" "$SSL_PID" 2>/dev/null

cat /tmp/af_packet_out.txt