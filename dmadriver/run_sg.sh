#!/bin/bash

# Один размер для теста (8 МБ)
SIZE=8388608
SIZES=(1024 65536 1048576 8388608 16777216)
BUFFER_SIZE=$((1 << 21)) # 2 МБ на сегмент

LOG_FILE="/mnt/share/dma_sg_test.log"
> $LOG_FILE

echo "=== Extended Scatter-Gather DMA Test ===" | tee -a $LOG_FILE

# 1. Отвязываем карту
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/unbind > /dev/null

# 2. Очищаем логи
sudo dmesg -c > /dev/null

# 3. Загружаем модуль в режиме SG
# buffer_size = 1 МБ, alloc_mode = 2 (SG)
sudo insmod dmadriver.ko alloc_mode=2 buffer_size=$BUFFER_SIZE

if [ $? -ne 0 ]; then
    echo "ERROR: Module failed to load in SG mode" | tee -a $LOG_FILE
    echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
    exit 1
fi

# 4. Запускаем передачу через /proc
echo "--- Writing size $SIZE to /proc/dmadriver ---" | tee -a $LOG_FILE
echo $SIZE | sudo tee /proc/dmadriver > /dev/null

# 5. Ждем завершения
sleep 3

# 6. Читаем результат из /proc
echo "--- /proc/dmadriver ---" | tee -a $LOG_FILE
cat /proc/dmadriver | tee -a $LOG_FILE

# 7. Выгружаем модуль
sudo rmmod dmadriver

# 8. Возвращаем карту
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null

echo "----------------------------------------" | tee -a $LOG_FILE
echo "Test finished. Check $LOG_FILE" | tee -a $LOG_FILE