
#include <sys/syslog.h>

#include "types.h"
#include "param.h"
#include "mem/memlayout.h"
#include "fs/vfs/file.h"
#include "fs/vfs/fs.h"
#include "fs/vfs/inode.h"
#include "fs/ext4/vfs_ext4_ext.h"
#include "mem/kalloc.h"
#include "mem/buddysystem.h"
#include "fs/buf.h"
#include "dev/virtio.h"
#include "lib/print.h"
#include "platform.h"
#include "sys/syslog.h"

#ifdef RISCV
#include "defs.h"
#include "proc/plic.h"
#elif defined(LOONGARCH)
#include "trap/apic.h"
#include "trap/extioi.h"
#include "dev/pci/pci.h"
#endif

volatile static int started = 0;

// entry.S needs one stack per CPU.
__attribute__ ((aligned (16))) char stack0[NCPU][4096];


// start() jumps here in supervisor mode on all CPUs.
void main()
{
	if (started == 0)
	{
#ifdef RISCV
		consoleinit();
		printfinit();
		printf("\n");
		printf("SOS kernel is booting\n");
		printf("\n");
		kinit(); // 伙伴分配器初始化
		kvminit(); // create kernel page table

		kvminithart(); // turn on paging
		procinit(); // process table
		trapinit(); // trap vectors
		trapinithart(); // install kernel trap vector
		plicinit(); // set up interrupt controller
		plicinithart(); // ask PLIC for device interrupts


		virtio_disk_init2(); //初始化 rootfs的块设备
		virtio_disk_init(); // emulated hard disk ps:如果使用SDCard需要修改
		init_fs_table(); //fs_table init
		binit(); // buffer cache
		fileinit(); // file table
		inodeinit(); //inode table

		vfs_ext4_init(); //初始化lwext4
		initlogbuffer();
		userinit(); // first user process
		__sync_synchronize();
		started = 1;
#elif defined(LOONGARCH)

		consoleinit();
		printfinit();
		printf("\n");
		printf("SOS kernel is booting\n");
		printf("\n");
//		pci_device_init();

		virtio_probe();

		apic_init();     // set up LS7A1000 interrupt controller
		extioi_init();   // extended I/O interrupt controller
		trapinit();      // trap vectors

		kinit();         // 伙伴分配器初始化
		// printf("kinit finish\n");

		kvminit();        // create kernel page table
		// printf("kvminit finish\n");
		kvminithart();   // turn on paging
		procinit();      // process table

		virtio_disk_init2(); //初始化 rootfs的块设备
		virtio_disk_init(); // emulated hard disk ps:如果使用SDCard需要修改
		init_fs_table(); //fs_table init
		binit(); // buffer cache
		fileinit(); // file table
		inodeinit(); //inode table

		vfs_ext4_init(); //初始化lwext4
		initlogbuffer();
		userinit();      // first user process
		__sync_synchronize();
		started = 1;
#endif
	}
	else
	{
#ifdef RISCV
		while (started == 0);
		__sync_synchronize();
		printf("hart %d starting\n", cpuid());
		kvminithart(); // turn on paging
		trapinithart(); // install kernel trap vector
		plicinithart(); // ask PLIC for device interrupts
#endif

	}

	scheduler();
}
