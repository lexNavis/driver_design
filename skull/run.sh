#!/bin/bash
if [ -z "$1" ]; then
    echo "usage: $0 <config_file>"
    exit 1
fi
source "$1"

mkdir -p logs

./load.sh

for dev in $(seq 0 $((DEV_COUNT - 1))); do
    for p in $(seq 1 $PRODUCERS_PER_DEV); do
        ./producer $dev $DELAY_PROD "data${p}" > logs/log_${dev}_prod_${p}.log 2>&1 &
    done
    for c in $(seq 1 $CONSUMERS_PER_DEV); do
        ./consumer $dev $DELAY_CONS > logs/log_${dev}_cons_${c}.log 2>&1 &
    done
    for f in $(seq 1 $FLUSHERS_PER_DEV); do
        ./flusher $dev $DELAY_FLUSH > logs/log_${dev}_flush_${f}.log 2>&1 &
    done
done

sleep $DURATION

pkill -f './producer' || true
pkill -f './consumer' || true
pkill -f './flusher' || true

./unload.sh
