obj-m := netdriver.o

COMMAND := make -C /lib/modules/$(shell uname -r)/build M=$(PWD)

all:
	$(COMMAND) modules

clean:
	$(COMMAND) clean