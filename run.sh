# Скрипт собирает драйвер, загружает в ядро, выводит статистику 
# через некоторый момент времени и выгружает драйвер

make
sudo insmod irqdriver.ko
sleep 2
cat /proc/irqdriver
sudo rmmod irqdriver
sudo dmesg | tail -n 5
