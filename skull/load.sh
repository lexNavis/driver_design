#!/bin/bash
set -e

MODULE="skull_driver"
DEVICE="skull"
COUNT=3
MODE="666"

sudo insmod ./${MODULE}.ko MAX_SIZE=5 DEV_COUNT=${COUNT}

MAJOR=$(awk -v dev="${DEVICE}" '$2 == dev {print $1}' /proc/devices)
if [ -z "${MAJOR}" ]; then
    echo "Failed to find major for ${DEVICE}"
    exit 1
fi

for i in $(seq 0 $((COUNT - 1))); do
    sudo rm -f /dev/${DEVICE}${i}
    sudo mknod /dev/${DEVICE}${i} c ${MAJOR} ${i}
    sudo chmod ${MODE} /dev/${DEVICE}${i}
done

echo "Loaded ${MODULE} with major ${MAJOR}"
