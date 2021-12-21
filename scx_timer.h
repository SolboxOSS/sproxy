#ifndef __SCX_TIMER_H__
#define __SCX_TIMER_H__

#include <bt_timer.h>

#define TIMER_RESOLUTION	100

extern void *gscx__timer_wheel_base;


int scx_timer_init();
void scx_timer_deinit();
int traffic_control(void *cr, size_t max);


#endif /* __SCX_TIMER_H__ */
