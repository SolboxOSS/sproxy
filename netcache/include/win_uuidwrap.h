#ifndef __WIN_UUID_WRAP_H__
#define __WIN_UUID_WRAP_H__
/*
 * Windows UUID wrapper
 */
#define		UUID_S_LEN	36
#define		UUID_B_LEN	16
#ifdef uuid_t
#undef uuid_t
#endif
typedef unsigned char uuid_t[UUID_B_LEN];

void uuid_clear(uuid_t);
int uuid_compare(const uuid_t, const uuid_t);
void uuid_generate_random(uuid_t);
int uuid_is_null(const uuid_t);
int uuid_parse(const char *, uuid_t);

#endif
