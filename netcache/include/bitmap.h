#ifndef __BT_BITMAP_H__
#define __BT_BITMAP_H__
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/*
 * most of constants and macros adjusted to the pre-existing linux
 * implementation
 */

#define		LONG_SZ_BYTES				sizeof(unsigned long)
#define		BITS_PER_LONG				(LONG_SZ_BYTES * 8)

#define		BITS_TO_LONGS(bitcount_)	(((bitcount_) + BITS_PER_LONG - 1)/BITS_PER_LONG)
#define		BITS_TO_LONGS_IDX(bitpos_)	((bitpos_)/BITS_PER_LONG)
#define		BITS_WORD_MASK(b_)			(1UL << ((b_) % BITS_PER_LONG))

static inline  int
test_bit(int bitpos, unsigned long *bitmap)
{
	return (bitmap[BITS_TO_LONGS_IDX(bitpos)] >> (bitpos & (BITS_PER_LONG -1))) & 1UL;
}
static inline void
clear_bit(int bitpos, unsigned long *bitmap)
{
	unsigned long	*bitword = &bitmap[BITS_TO_LONGS_IDX(bitpos)];

	*bitword &= ~(BITS_WORD_MASK(bitpos));
}
static inline void
set_bit(int bitpos, unsigned long *bitmap)
{
	unsigned long	*bitword = &bitmap[BITS_TO_LONGS_IDX(bitpos)];

	*bitword |= BITS_WORD_MASK(bitpos);
}
static inline void
bitmap_zero(unsigned long *bitmap, int bc)
{
	if (bc <= BITS_PER_LONG)
		/* single unsigned long */
		*bitmap = 0UL;
	else {
		/* multiple unsigned longs */
		int bytes = BITS_TO_LONGS(bc) * sizeof(unsigned long); /* total bytes */
		memset((unsigned char *)bitmap,  0, bytes);
	}
}
static inline void
bitmap_copy(unsigned long *db/*dest bitmap*/, unsigned long *sb/*source bitmap*/, int bc/*bit count*/)
{
	if (bc <= BITS_PER_LONG)
		*db = *sb;
	else {
		/* multiple bytes copy */
		int bytes = BITS_TO_LONGS(bc) * sizeof(unsigned long); /* total bytes */
		memcpy(db, sb, bytes);
	}
}
static inline int
bitmap_full(unsigned long *bitmap, int bs/*bit size*/)
{
	int		lc = BITS_TO_LONGS_IDX (bs); 
	int		i;


	if (bs <= BITS_PER_LONG)
		return !(~(*bitmap) & BITS_WORD_MASK(bs % BITS_PER_LONG));


	/* bitmap에서 bs에서 지정된 비트 수 중 unsigned long 크기로
	 * 접근 가능한 모든 메모리 블럭에 대해서 for loop
	 * 실행
	 */
	for (i = 0; i < lc; i++) {
		/* 대상 unsigned long 값이 full이면  invert하면 0이됨 */
		if (~bitmap[i]) return 0; 
	}
	/*
	 * unsigned long으로 접근이 안되는 나머지 영역에 대한 조사
	 */
	if ((bs % BITS_PER_LONG) > 0) {
		if (~bitmap[i] & BITS_WORD_MASK(bs % BITS_PER_LONG))
			return 0;
	}
	return 1;

}





#endif /* __BT_BITMAP_H__ */
