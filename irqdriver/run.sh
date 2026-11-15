#!/bin/bash

echo "=== Loading irqdriver (default interval 100 ms) ==="
sudo insmod irqdriver.ko
sleep 1

echo "=== Running for 2 seconds with 100 ms interval ==="
sleep 2
sudo ./irq_user
sleep 1

echo "=== Changing interval to 10 ms via /proc ==="
echo 10 | sudo tee /proc/irqdriver
sleep 2

echo "=== Reading final statistics ==="
sudo ./irq_user

echo "=== Unloading irqdriver ==="
sudo rmmod irqdriver
