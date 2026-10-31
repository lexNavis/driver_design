# Скрип загружает драйвер в ядро, выводит статистику 
# через некоторый момент времени и выгружает драйвер

sudo insmod irqdriver.ko
# Вручную вызываем аппаратное прерывание
for i in {1..10}; do
	echo 1 > /proc/irqdriver
done
sleep 1
cat /proc/irqdriver
sudo rmmod irqdriver
sudo dmesg | tail -n 5
