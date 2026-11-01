# Отвязка стандартного драйвера сетевой карты
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/unbind
sudo insmod dmadriver.ko target_slot=0000:00:06.0
# Проверка, загрузился ли модуль
if [$? -ne 0]; then
	echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind
	sudo rmmod dmadriver
	exit 1	
fi
sudo dmesg | grep dmadriver > /mnt/share/dma_log.txt
sudo rmmod dmadriver
# Вернем обратно
echo 0000:00:06.0 | sudo tee /sys/bus/pci/drivers/e1000/bind
