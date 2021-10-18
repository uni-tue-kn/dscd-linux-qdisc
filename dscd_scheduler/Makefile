EXTRA_CFLAGS = -I$(PWD)/include

obj-m := sch_dscd.o

ccflags-y += -include $(PWD)/include/uapi/linux/pkt_sched_dscd.h

sch_dscd-y := net/sched/sch_dscd.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
