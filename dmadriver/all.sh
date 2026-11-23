#!/bin/bash

# Array of sizes for testing
SIZES=(1024 65536 1048576) # Removed 16MB for RISC-V to save time
# Buffer size for streaming and SG (2 MB)
BUFFER_SIZE=$((1 << 21)) 

# PCI slot for the second e1000 card (eth1) on RISC-V QEMU
TARGET_SLOT="0000:00:02.0"

# Create logs directory if it doesn't exist
mkdir -p ./logs
LOG_FILE="./logs/dma_all_modes_test.log"
> $LOG_FILE

echo "=== Running ALL DMA modes test ===" | tee -a $LOG_FILE

# Function to run tests for a specific mode
run_tests() {
    local mode_name=$1
    local mode_val=$2
    local use_buffer_size=$3

    echo "========================================" | tee -a $LOG_FILE
    echo "=== MODE: $mode_name (alloc_mode=$mode_val) ===" | tee -a $LOG_FILE
    echo "========================================" | tee -a $LOG_FILE

    # 1. Unbind the card from the standard e1000 driver
    echo $TARGET_SLOT | sudo tee /sys/bus/pci/drivers/e1000/unbind > /dev/null

    # 2. Load the module with the correct target slot
    if [ "$use_buffer_size" = "yes" ]; then
        sudo insmod dmadriver.ko alloc_mode=$mode_val buffer_size=$BUFFER_SIZE target_slot=$TARGET_SLOT
    else
        sudo insmod dmadriver.ko alloc_mode=$mode_val target_slot=$TARGET_SLOT
    fi

    if [ $? -ne 0 ]; then
        echo "ERROR: Module failed to load for $mode_name" | tee -a $LOG_FILE
        echo $TARGET_SLOT | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
        return 1
    fi

    # 3. Run tests for each size
    for SIZE in "${SIZES[@]}"; do
        echo "=== Testing size: $SIZE bytes ===" | tee -a $LOG_FILE
        
        # Clear dmesg to avoid mixing logs
        sudo dmesg -c > /dev/null
        
        sudo ./user_app.sh $SIZE | tee -a $LOG_FILE
        
        echo "----------------------------------------" | tee -a $LOG_FILE
    done

    # 4. Unload the module
    sudo rmmod dmadriver

    # 5. Rebind the card to the standard driver
    echo $TARGET_SLOT | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
}

# --- Run tests sequentially ---

# 1. Coherent (no buffer_size)
run_tests "Coherent" 0 "no"

# 2. Streaming (with buffer_size)
run_tests "Streaming" 1 "yes"

# 3. Scatter-Gather (with buffer_size)
run_tests "Scatter-Gather" 2 "yes"

echo "=== ALL TESTS FINISHED ===" | tee -a $LOG_FILE
echo "Check $LOG_FILE for results." | tee -a $LOG_FILE
