/*
** Copyright 2001-2004, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#include <kernel/kernel.h>
#include <kernel/dev/fixed.h>
#include <kernel/vfs.h>
#include <kernel/debug.h>

#include <string.h>

#ifdef ARCH_i386
#endif
#ifdef ARCH_sh4
#include <kernel/dev/arch/sh4/maple/maple_bus.h>
#include <kernel/dev/arch/sh4/keyboard/keyboard.h>
#include <kernel/dev/arch/sh4/console/console_dev.h>
#endif

/* loads all the fixed drivers */
int fixed_devs_init(kernel_args *ka)
{
	null_dev_init(ka);
	zero_dev_init(ka);
	dprint_dev_init(ka);

#ifdef ARCH_sh4
	maple_bus_init(ka);
	keyboard_dev_init(ka);
#endif

	return 0;
}
