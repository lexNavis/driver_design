#!/bin/bash

# Streaming-режим: размер буфера 4 МБ
BUFFER_SIZE=$((1 << 21)) 
# Массив размеров для теста
SIZES=(1024 65536 1048576 2097152 4194304 8388608 16777216)

LOG_FILE="/mnt/share/dma_stream_test.log"
> $LOG_FILE

echo "=== Streaming DMA Test (buffer_size=$BUFFER_SIZE) ===" | tee -a $LOG_FILE

# 1. Отвязываем карту
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/unbind > /dev/null

# 2. Загружаем модуль в streaming-режиме
sudo insmod dmadriver.ko alloc_mode=1 buffer_size=$BUFFER_SIZE

if [ $? -ne 0 ]; then
    echo "ERROR: Module failed to load in streaming mode" | tee -a $LOG_FILE
    echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
    exit 1
fi

# 3. Прогоняем размеры
for SIZE in "${SIZES[@]}"; do
    echo "=== Testing size: $SIZE bytes ===" | tee -a $LOG_FILE
    
    # Очищаем логи ядра
    sudo dmesg -c > /dev/null
    
    # Инициируем передачу
    echo $SIZE | sudo tee /proc/dmadriver > /dev/null
    
    # Ждем завершения (для 16 МБ нужно чуть больше времени)
    sleep 3
    
    # Читаем статистику (только в файл, не в консоль)
    cat /proc/dmadriver | tee -a $LOG_FILE
    
    # Сохраняем dmesg (только если что-то пошло не так)
    sudo dmesg | grep dmadriver >> $LOG_FILE
    
    echo "----------------------------------------" | tee -a $LOG_FILE
done

# 4. Выгружаем модуль
sudo rmmod dmadriver

# 5. Возвращаем карту
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null

echo "Test finished. Check $LOG_FILE" | tee -a $LOG_FILE
