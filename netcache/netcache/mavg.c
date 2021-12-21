#include <config.h>
#include <netcache_config.h>
#include <netcache.h>
#include <stdlib.h>
#include <math.h>


mavg_t  *
mavg_create(char *name)
{
	mavg_t	* t = NULL;

	t = (mavg_t *)XMALLOC(sizeof(mavg_t), AI_ETC);
	t->name = name; /* no strdup */

	return t;
}
char *
mavg_name(mavg_t *hm)
{
	return hm->name;
}
void
mavg_stat(mavg_t *hm, long *cnt, double *vmin, double *vmax, double *vavg)
{
	*vmin 		= hm->vmin;	
	*vmax 		= hm->vmax;	
	if (hm->count > 0)
		*vavg 		= (hm->sum/(double)hm->count);	
	else
		*vavg 		= -1.0;
	*cnt		= hm->count;

	hm->vmin 	= 
	hm->vmax 	= 
	hm->sum  	=  0;
	hm->count  	=  0;
}

void
mavg_update(mavg_t *hm, double v)
{
	hm->count++;
	hm->sum	+= v;
	if (hm->count == 1) {
		hm->vmin = v;
		hm->vmax = v;
	}
	else {
		hm->vmin = min(v, hm->vmin);
		hm->vmax = max(v, hm->vmax);
	}
}
