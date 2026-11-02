#!/bin/bash

# Массив размеров для теста
SIZES=(1024 65536 1048576 2097152 4194304 8388608 16777216)

LOG_FILE="/mnt/share/dma_test.log"
> $LOG_FILE

# 1. Отвязываем карту
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/unbind > /dev/null

# 2. Загружаем модуль ОДИН РАЗ (без параметров test_size!)
sudo insmod dmadriver.ko

# Проверяем, что модуль загрузился
if [ $? -ne 0 ]; then
    echo "ERROR: Module failed to load" | tee -a $LOG_FILE
    # Возвращаем карту обратно и выходим
    echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
    exit 1
fi

# 3. Прогоняем все размеры через /proc
for SIZE in "${SIZES[@]}"; do
    echo "=== Testing size: $SIZE bytes ===" | tee -a $LOG_FILE
    
    # Очищаем логи ядра (чтобы видеть только текущий тест)
    sudo dmesg -c > /dev/null
    
    # Инициируем передачу через запись в /proc
    echo $SIZE | sudo tee /proc/dmadriver > /dev/null
    
    # Ждем, пока передача завершится (даем время)
    sleep 3
    
    # 4. Читаем результат из /proc (статистику)
    echo "--- /proc/dmadriver ---" | tee -a $LOG_FILE
    cat /proc/dmadriver | tee -a $LOG_FILE
    
    # 5. Сохраняем логи dmesg (если хочешь)
    echo "--- dmesg ---" | tee -a $LOG_FILE
    sudo dmesg | grep dmadriver | tee -a $LOG_FILE
    
    echo "----------------------------------------" | tee -a $LOG_FILE
done

# 6. Выгружаем модуль
sudo rmmod dmadriver

# 7. Возвращаем карту обратно
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind > /dev/null
