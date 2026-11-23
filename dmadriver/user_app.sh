#!/bin/bash

# User application to interact with /proc/dmadriver.
# It initiates a DMA transfer for a given size and prints the statistics.

if [ $# -ne 1 ]; then
    echo "Usage: $0 <size_in_bytes>"
    exit 1
fi

SIZE=$1

# 1. Initiate DMA transfer by writing size to /proc
if ! echo "$SIZE" > /proc/dmadriver 2>/dev/null; then
    echo "Error: Failed to write to /proc/dmadriver (is the module loaded?)"
    exit 1
fi

# 2. Wait for transfer to complete.
# 5 seconds is enough even for 16MB on a slow VM.
sleep 5

# 3. Read statistics
echo "=== Statistics for size $SIZE ==="
cat /proc/dmadriver
