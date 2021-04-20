/*
 *  sysctl.h
 *  Header file for sysctl.c
 *
 *  Copyright (C) 1997 Martin von L鰓is
 *  Copyright (C) 1997 R間is Duchesne
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef DEBUG
extern int ntdebug;

void ntfs_sysctl(int add);

#define SYSCTL(x)	ntfs_sysctl(x)
#else
#define SYSCTL(x)
#endif /* DEBUG */

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
