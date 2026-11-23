obj-m += dmadriver.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules 2> errors.txt
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
