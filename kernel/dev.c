#include <kernel.h>
#include <stage2.h>
#include <dev.h>
#include <vfs.h>
#include <debug.h>

#include <fs/bootfs.h>

int dev_init(kernel_args *ka)
{
	int err;
	TOUCH(ka);

	dprintf("dev_init: entry\n");

	err = vfs_create(NULL, "/dev", "", STREAM_TYPE_DIR);
	if(err < 0)
		panic("dev_init: error making /dev!\n");

	// bootstrap the bootfs
	bootstrap_bootfs();
	
	err = vfs_create(NULL, "/boot", "", STREAM_TYPE_DIR);
	if(err < 0)
		panic("error creating /boot\n");
	err = vfs_mount("/boot", "bootfs");
	if(err < 0)
		panic("error mounting bootfs\n");
	
	return 0;
}