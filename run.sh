# Скрип загружает драйвер в ядро, выводит статистику 
# через некоторый момент времени и выгружает драйвер

sudo insmod irqdriver.ko
# Вручную вызываем аппаратное прерывание
echo 1 > /proc/irqdriver
sleep 1
cat /proc/irqdriver
cat /proc/interrupts | grep irqdriver
sudo rmmod irqdriver
sudo dmesg | tail -n 5
