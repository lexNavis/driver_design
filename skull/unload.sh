#!/bin/bash
set -e

MODULE="skull_driver"
DEVICE="skull"
COUNT=3

for i in $(seq 0 $((COUNT - 1))); do
    sudo rm -f /dev/${DEVICE}${i}
done

sudo rmmod ${MODULE}
echo "Unloaded ${MODULE}"
