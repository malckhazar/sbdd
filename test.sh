#!/bin/bash

RAID=0

modprobe nbd

disks=2

if (( $# > 0 )); then
	disks=$1;
fi

echo "disks = $disks"

i=1
arg=""
while (( $i <= $disks )); do
	rm -f disk$i.raw
	qemu-img create -f raw disk$i.raw 1M
	qemu-nbd -c /dev/nbd$i -f raw disk$i.raw

	[ ! -z "$arg" ] && arg+=",";
	arg+="/dev/nbd$i"
	((i++))
	sleep 0.5
done

sleep 1

echo "disks=$arg"

insmod sbdd.ko disks=$arg mode=$RAID stripe=512

[ ! -f test_in ] && dd if=/dev/urandom of=test_in bs=4096 count=10

dd if=test_in of=/dev/sbdd
dd if=/dev/sbdd of=test_out_dd bs=4096 count=10

sleep 3s

rmmod sbdd

i=1
while (( $i <= $disks)); do
	qemu-nbd -d /dev/nbd$i
	((i++))
done

cmp test_in test_out_dd

