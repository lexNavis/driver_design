#!/bin/bash

LOG_FILE="/mnt/share/dma_sg_test.log"
> $LOG_FILE

echo "=== Scatter-Gather DMA Test ===" | tee -a $LOG_FILE

# 1. Отвязываем карту
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/unbind > /dev/null

# 2. Очищаем логи ядра перед тестом
sudo dmesg -c > /dev/null

# 3. Загружаем модуль в режиме SG (alloc_mode=2)
sudo insmod dmadriver.ko alloc_mode=2

if [ $? -ne 0 ]; then
    echo "ERROR: Module failed to load in SG mode" | tee -a $LOG_FILE
    echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
    exit 1
fi

# 4. Ждем, чтобы SG-тест завершился (или упал по таймауту)
sleep 3

# 5. Сохраняем логи в файл (чтобы не скроллить консоль)
sudo dmesg | grep dmadriver >> $LOG_FILE

# 6. Выгружаем модуль
sudo rmmod dmadriver

# 7. Возвращаем карту
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null

echo "----------------------------------------" | tee -a $LOG_FILE
echo "Test finished. Check $LOG_FILE" | tee -a $LOG_FILE
