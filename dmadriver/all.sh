#!/bin/bash

# Массив размеров для тестов
SIZES=(1024 65536 1048576 8388608 16777216)
# Размер буфера для streaming и SG (2 МБ)
BUFFER_SIZE=$((1 << 21)) 

LOG_FILE="/mnt/share/dma_all_modes_test.log"
> $LOG_FILE

echo "=== Running ALL DMA modes test ===" | tee -a $LOG_FILE

# Функция для запуска тестов для конкретного режима
run_tests() {
    local mode_name=$1
    local mode_val=$2
    local use_buffer_size=$3

    echo "========================================" | tee -a $LOG_FILE
    echo "=== MODE: $mode_name (alloc_mode=$mode_val) ===" | tee -a $LOG_FILE
    echo "========================================" | tee -a $LOG_FILE

    # 1. Отвязываем карту
    echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/unbind > /dev/null

    # 2. Загружаем модуль
    if [ "$use_buffer_size" = "yes" ]; then
        sudo insmod dmadriver.ko alloc_mode=$mode_val buffer_size=$BUFFER_SIZE
    else
        sudo insmod dmadriver.ko alloc_mode=$mode_val
    fi

    if [ $? -ne 0 ]; then
        echo "ERROR: Module failed to load for $mode_name" | tee -a $LOG_FILE
        echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
        return 1
    fi

    # 3. Прогоняем размеры
    for SIZE in "${SIZES[@]}"; do
        echo "=== Testing size: $SIZE bytes ===" | tee -a $LOG_FILE
        
        # Очищаем логи
        sudo dmesg -c > /dev/null
        
        # Инициируем передачу через /proc
        echo $SIZE | sudo tee /proc/dmadriver > /dev/null
        
        # Ждем завершения
        sleep 3
        
        # Читаем результат
        cat /proc/dmadriver | tee -a $LOG_FILE
        
        echo "----------------------------------------" | tee -a $LOG_FILE
    done

    # 4. Выгружаем модуль
    sudo rmmod dmadriver

    # 5. Возвращаем карту
    echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
}

# --- Запускаем тесты по очереди ---

# 1. Coherent (без параметра buffer_size)
run_tests "Coherent" 0 "no"

# 2. Streaming (с buffer_size)
run_tests "Streaming" 1 "yes"

# 3. Scatter-Gather (с buffer_size)
run_tests "Scatter-Gather" 2 "yes"

echo "=== ALL TESTS FINISHED ===" | tee -a $LOG_FILE
echo "Check $LOG_FILE for results." | tee -a $LOG_FILE
