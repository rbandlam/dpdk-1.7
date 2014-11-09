sudo modprobe uio
sudo insmod /users/rbandlam/dpdk-1.7.1/x86_64-native-linuxapp-gcc/kmod/igb_uio.ko
sudo python /users/rbandlam/dpdk-1.7.1/tools/dpdk_nic_bind.py --bind igb_uio 0000:02:00.3 
sudo python /users/rbandlam/dpdk-1.7.1/tools/dpdk_nic_bind.py --bind igb_uio 0000:03:00.0
sudo python /users/rbandlam/dpdk-1.7.1/tools/dpdk_nic_bind.py --bind igb_uio 0000:03:00.1

export RTE_SDK=/users/rbandlam/dpdk-1.7.1
export RTE_TARGET=x86_64-native-linuxapp-gcc
