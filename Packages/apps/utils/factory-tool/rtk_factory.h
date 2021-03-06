#ifndef RTK_FACTORY_H
#define RTK_FACTORY_H
#include <stdio.h>

#define FACTORY_RO_SUPPORT	1

int factory_init();
int factory_load();
#ifdef FACTORY_RO_SUPPORT
int factory_init_ro();
int factory_load_ro();
#endif
int factory_flush(unsigned int factory_start, unsigned int factory_size);
extern char factory_dir[32];
#endif
