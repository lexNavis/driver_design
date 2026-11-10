#!/bin/bash

echo "=== Loading irqdriver ==="
sudo insmod irqdriver.ko
sleep 1

echo "=== Generating 10 edu interrupts ==="
for i in {1..10}; do
    echo 1 > /proc/irqdriver
    sleep 0.1
done

echo "=== Waiting for bottom halves ==="
sleep 1

echo "=== Reading statistics via user program ==="
sudo ./irq_user

echo "=== Unloading irqdriver ==="
sudo rmmod irqdriver
