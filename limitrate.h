#ifndef __LIMIT_RATE_H__
#define __LIMIT_RATE_H__

int limitrate_make(void *cr);
void limitrate_clean(void *cr);
size_t limitrate_control(void *cr, size_t max, int reader_type);
size_t limitrate_compute(void *cr, size_t max);
int limitrate_suspend(void *cr);
#endif /* __LIMIT_RATE_H__ */
