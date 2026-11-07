#First check blkdevice is avaialble, if size is 0, the use blk device
#lsblk /dev/nbd0

# connect qemu to blk device
sudo qemu-nbd --connect=/dev/nbd0 /data/sources/tmp5/xemu-state/oxos-xemu-exfat/hdd.qcow2 

#then mount
sudo mount -t exfat -o uid=0,gid=100,fmask=0002,dmask=0002 /dev/nbd0p1 /mnt/exfat_disk

