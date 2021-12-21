#include <netcache_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <netcache_types.h>
#include <util.h>
#include <ncapi.h>
#include <assert.h>

typedef struct tag_vstring {
	nc_int16_t		len;
	char			data[4]; /* for alignement purpose */
} nc_vstring_t;
#define		NC_VSTRING_SIZE(_l)	(sizeof(nc_uint16_t) + ((_l + sizeof(nc_uint16_t)-1)/sizeof(nc_uint16_t))*sizeof(nc_uint16_t))
typedef char *			cptr;
int
main()
{

#define		MAX_TEST		10000
#define 	MAX_PATTERN 	100
	cptr		*test_pattern;
	int			i, j, l;
	int			ra = 0;
	char		canvas[1024*1024];
	char		*pcan = canvas;
	char		*punpack;
	int 		len;
	int			test_idx[MAX_PATTERN];
	int			tn, tc, ok=0;
	int			tr = 0;

	
	nc_setup_log(NC_ERROR, "/tmp/x.log", 10, 10);
	nc_init();
	srand(time(NULL));

	fprintf(stderr, "initializing test patterns...\n");
	test_pattern = XCALLOC(MAX_PATTERN, sizeof(char *), AI_ETC);

	XVERIFY(test_pattern);
	for (i = 0; i < MAX_PATTERN; i++) {
		ra = 1 + rand() % 200;
		test_pattern[i] = XMALLOC(ra, AI_ETC);
		for (j = 0; j < ra-1; j++) {
			test_pattern[i][j] = '0' + (j%10);
		}
		XVERIFY(test_pattern[i]);
	}

	XVERIFY(test_pattern);
	for (l = 0; l < MAX_TEST; l++) {
		tc = 1+rand() % MAX_PATTERN;
		pcan = canvas;
		XVERIFY(test_pattern);
		assert(tc <= MAX_PATTERN);
		tr = 0;
		for (i = 0; i < tc; i++) {
			tn = rand() % MAX_PATTERN;
			assert(tn < MAX_PATTERN);
			test_idx[i] = tn;
			XVERIFY(test_pattern[tn]);
			pcan = vstring_pack(pcan, (canvas + sizeof(canvas) - pcan), test_pattern[tn], -1);
		}
		pcan = canvas;
		XVERIFY(test_pattern);
		for (j = 0; j < tc; j++) {
			XVERIFY(test_pattern[test_idx[j]]);

			vstring_unpack(pcan, &punpack, &len);
			if (strcmp(test_pattern[test_idx[j]], punpack) != 0) {
				fprintf(stderr, "match fail:['%s', '%s']\n", test_pattern[test_idx[j]], punpack);
				tr = -1;
			}
		}
		if (tr >= 0) ok++;
	}
	fprintf(stderr, "%d test, %d passed\n", l, ok);
	nc_shutdown();
}

