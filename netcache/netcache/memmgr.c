#include <config.h>
#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <malloc.h>
#ifndef WIN32
#include <alloca.h>
#endif
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <zlib.h>
#include <netcache.h>
#include <block.h>
#include <util.h>
/*
 * relationship among
 * 	# of inodes
 * 	# of inode extent
 *  # of blocks
 *  # of pages
 *
 */

int
mem_init(memmgr_heap_t *heap, long long memsz)
{
	DEBUG_ASSERT(memsz > 0);
	heap->heapsize = memsz;
	heap->heapalloc = 0;
	heap->heapalloc_aligned = 0;

	/*
	 * TODO :
	 * check if the memsz is less than the RAM size
	 */
	return 0;
}

void *
mem_alloc(memmgr_heap_t *heap, nc_size_t memsz, int aligned, int category, int force, const char *f, int l)
{
	void 	*palloc = NULL;

	if (!force && ((heap->heapalloc + memsz) > heap->heapsize)) {
		/*
		 * no more allocation if force flag is not set
		 */
		return NULL;
	}
	if (aligned) {
		palloc = nc_aligned_malloc(memsz, aligned, category, f, l);
	}
	else {
#ifdef NC_HEAPTRACK
		___trace_upper += 1;
#endif
    	palloc = XMALLOC_FL(memsz, category, f, l);
#ifdef NC_HEAPTRACK
		___trace_upper -= 1;
#endif
	}

	if (palloc) {
		if (!aligned)
			memsz = __nc_get_len(palloc);
		else
			_ATOMIC_ADD(heap->heapalloc_aligned, memsz);

		_ATOMIC_ADD(heap->heapalloc, memsz);
	}
	else {
		TRACE((T_WARN, "alloca.size=%lld, aligned=%d: cache memory run out!\n", memsz, aligned));
	}
	return palloc;
}
void *
mem_realloc(memmgr_heap_t *heap, void *p, nc_size_t memsz, int category, const char *f, int l)
{

	if (p) {
		_ATOMIC_SUB(heap->heapalloc, __nc_get_len(p));
	}
#ifdef NC_HEAPTRACK
	___trace_upper += 1;
#endif
	p = XREALLOC_FL(p, memsz, category, f, l);
#ifdef NC_HEAPTRACK
	___trace_upper -= 1;
#endif
	_ATOMIC_ADD(heap->heapalloc, __nc_get_len(p));
	return p;
}
void
mem_free(memmgr_heap_t *heap, void *p, int aligned, nc_size_t memsz, const char *f, int l)
{
	DEBUG_ASSERT(p != NULL);

	if (aligned) {
		nc_aligned_free(p);
		_ATOMIC_SUB(heap->heapalloc_aligned, memsz);
	}
	else {
		DEBUG_ASSERT(nc_check_memory(p, 0));
		memsz 	= __nc_get_len(p);
		XFREE(p);
	}
	_ATOMIC_SUB(heap->heapalloc, memsz);

}
void
mem_stat(memmgr_heap_t *heap, nc_int64_t *used, nc_int64_t *total)
{
	*used = heap->heapalloc;
	*total = heap->heapsize;
}
/*
 * always force mode
 */
char *
mem_strdup(memmgr_heap_t *heap, char *str, int category, const char *f, int l)
{
	char 	*p = NULL;
	int 	sz;

#ifdef NC_HEAPTRACK
	___trace_upper += 1;
#endif
	p = XSTRDUP_FL(str, category, f, l);
#ifdef NC_HEAPTRACK
	___trace_upper -= 1;
#endif
	sz = __nc_get_len(p);
	_ATOMIC_ADD(heap->heapalloc, sz);
	return p;
}
