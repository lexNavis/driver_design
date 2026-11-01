#!/bin/bash

# Перебираем размеры для теста
SIZES=(2097152)
BUFFER_SIZE=$((1 << 20)) # 1 МБ

# Очищаем общий файл лога перед началом
LOG_FILE="/mnt/share/dma_test.log"
> $LOG_FILE

for SIZE in "${SIZES[@]}"; do
    echo "=== Testing size: $SIZE bytes ===" | tee -a $LOG_FILE
    
    # Отвязываем карту
    echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/unbind > /dev/null
    
    # Очищаем кольцо логов ядра перед тестом
    sudo dmesg -c > /dev/null
    
    # Загружаем модуль
    sudo insmod dmadriver.ko buffer_size=$BUFFER_SIZE test_size=$SIZE
    
    if [ $? -ne 0 ]; then
        echo "ERROR: Module failed to load for size $SIZE" | tee -a $LOG_FILE
    else
        sleep 2
        sudo rmmod dmadriver
    fi
    
    # Сохраняем свежие логи в файл
    sudo dmesg | grep dmadriver | tee -a $LOG_FILE
    
    # Привязываем карту обратно
    echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
    
    echo "----------------------------------------" | tee -a $LOG_FILE
done
