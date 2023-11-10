# Simple Block Device Driver
Implementation of Linux Kernel 5.4.X simple block device.

## Build
- regular:
`$ make`
- with requests debug info:
uncomment `CFLAGS_sbdd.o := -DDEBUG` in `Kbuild`

## Clean
`$ make clean`

## References
- [Linux Device Drivers](https://lwn.net/Kernel/LDD3/)
- [Linux Kernel Development](https://rlove.org)
- [Linux Kernel Teaching](https://linux-kernel-labs.github.io/refs/heads/master/labs/block_device_drivers.html)
- [Linux Kernel Sources](https://github.com/torvalds/linux)


## Task-1

```
$ qemu-img create -f raw disk1.img 1M
$ insmod sbdd.ko disk1=`pwd`/disk1.img
$ dd if=orig.txt of=/dev/sbdd
$ dd if=/dev/sbdd of=test.txt
$ cmp -xl -n `stat --printf="%s" orig.txt` orig.txt test.txt
```


