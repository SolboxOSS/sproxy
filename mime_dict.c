#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <search.h>
#include <string.h>
#include <ctype.h>
//#include <trace.h>
//#include <ncapi.h>
#include "common.h"
#include "scx_util.h"

/*
 * GNU  표준 c lib에 포함된 hash table로 구현
 * 주의) 테이블 포인터가 별도로 없어서 한번에 하나의 테이블만 사용 가능
 */
struct hsearch_data 	__mime_dict;


static int string_trim(char *str);
static int string_char_search(const char *string, int c, int len);
static char * string_dup_substr(const char *string, int pos_init, int pos_end);


int
mm_load(const char *path)
{
	char 		linebuf[1024];
	char 		*indent = NULL;
	int 		indent_len = -1;
	FILE 		*f;
	int 		r;
	int 		len;
	int 		line = 0;
	int 		i, n_keys = 0;
	char 		*key, *val;
	ENTRY 		kv, *kvp;


	memset(&__mime_dict, 0, sizeof(__mime_dict));

	if ((f = fopen(path, "r")) == NULL) {
		TRACE((T_WARN, "%s - could not be opened\n", path));
		return -1;
	}

	r = hcreate_r(1000,  &__mime_dict);
	if (!r) {
		TRACE((T_WARN, "%s - htable creation failed\n", path));
		return -1;
	}

	while (fgets(linebuf, sizeof(linebuf), f)) {
		len = strlen(linebuf);
		if (linebuf[len-1] == '\n') {
			linebuf[--len] = 0;
			if (len && linebuf[len - 1] == '\r') {
				linebuf[--len] = 0;
			}
		}

		line++;

		if (!linebuf[0]) continue;
		if (linebuf[0] == '#') continue;


		/* separator 정의 안됨 */
		if (!indent) {
			i = 0; 
			do {i++; } while (i < len && isblank(linebuf[i]));
			indent = string_dup_substr(linebuf, 0, i);
			indent_len = strlen(indent);
	
			/* blank indent line */
			if (i == len) {
				continue;
			}
		}

		if (strncmp(linebuf, indent, indent_len) != 0 ||
			isblank(linebuf[indent_len]) != 0) {
			TRACE((T_WARN, "%s - invalid indent level at line %d\n", path, line));
			return -1;
		}
		if (linebuf[indent_len] == '#' || indent_len == len) 
			continue;

		/*
		 * get key/value
		 */
		i = string_char_search(linebuf + indent_len, ' ', len - indent_len);

		if (i < 0) {
			TRACE((T_WARN, "%s - invalid line at line %d\n", path, line));
			return -1;
		}
		key = string_dup_substr(linebuf + indent_len, 0, i);
		if (!key) {
			TRACE((T_WARN, "%s - invalid line at line %d\n", path, line));
			return -1;
		}
		val = string_dup_substr(linebuf + indent_len + i, 1, len - indent_len);
		if (!val) {
			TRACE((T_WARN, "%s - invalid line at line %d\n", path, line));
			return -1;
		}

		kv.data = val;
		kv.key 	= key;

		i = hsearch_r(kv, ENTER, &kvp, &__mime_dict);
		if (kvp == NULL) {
			TRACE((T_WARN, "%s - registering '%s' failed at line %d\n", path, key, line));
			SCX_FREE(key);
			SCX_FREE(val);
			continue;
		}
		n_keys++;
	}
	fclose(f);
	if (indent) SCX_FREE(indent);
	TRACE((T_INFO, "%d - mime entry configured\n", n_keys));
	return n_keys;

}
static char *
string_dup_substr(const char *string, int pos_init, int pos_end)
{
	unsigned int 	size, bytes;
	char 			*buffer;

	size = (unsigned int) (pos_end - pos_init) + 1;
	if (size <= 2) size = 4;
	

	buffer = (char *)SCX_MALLOC(size);

	if (!buffer) return NULL;

	if (pos_init > pos_end) {
		SCX_FREE(buffer);
		return NULL;
	}
	bytes = (pos_end - pos_init);

	memcpy(buffer, string + pos_init, bytes);

	buffer[bytes] = '\0';
	return (char *)buffer;
}
static int 
string_char_search(const char *string, int c, int len)
{
    char *p;

	if (len < 0) {
		len = strlen(string);
	}

	p = memchr(string, c, len);
	if (p) {
		return (p - string);
	}
	return -1;
}

static int 
string_trim(char *str)
{
    unsigned int i;
    unsigned int len;
    char *left = 0, *right = 0;
    char *buf;

    buf = str;
    if (!buf) {
        return -1;
    }

    len = strlen(buf);
    left = buf;

    if(len == 0) {
        return 0;
    }

    /* left spaces */
    while (left) {
        if (isspace(*left)) {
            left++;
        }
        else {
            break;
        }
    }

    right = buf + (len - 1);
    /* Validate right v/s left */
    if (right < left) {
        buf[0] = '\0';
        return -1;
    }

    /* Move back */
    while (right != buf){
        if (isspace(*right)) {
            right--;
        }
        else {
            break;
        }
    }

    len = (right - left) + 1;
    for(i=0; i<len; i++){
        buf[i] = (char) left[i];
    }
    buf[i] = '\0';

    return 0;
}
char *
mm_lookup(const char *key)
{
	ENTRY 	kv, *kvp;
	int 		r;

	memset(&kv, 0, sizeof(kv));
	kv.key = key;
	r = hsearch_r(kv, FIND, &kvp, &__mime_dict);
	if (kvp == NULL) return NULL;

	return (char *)kvp->data;
}
void
mm_destroy()
{
	hdestroy_r(&__mime_dict);
}
