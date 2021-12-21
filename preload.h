#ifndef __PRELOAD_H__
#define __PRELOAD_H__

void pl_init();
void pl_deinit();
int pl_add_preload(void *preq);
int pl_preload_status(void *preq);

#endif /* __PRELOAD_H__ */
