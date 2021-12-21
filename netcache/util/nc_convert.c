

#pragma pack(8)
#define		NC_OLD_MAXCPATHLEN	256
typedef struct tag_fc_old_header_info {
	unsigned long     magic;
	char              qr_path[NC_OLD_MAXPATHLEN];
	char              qr_key[NC_OLD_MAXPATHLEN];
	time_t            ctime;
	time_t            mtime;
	long long         size;
	time_t            c_atime;/*REMARK!: this value is different from file atime */
	char              c_path[NC_OLD_MAXPATHLEN]; /* should be encrypted */
	nc_int32_t        physical_cursor;    /* the last allocated physical blk # */
	unsigned short    bitmaplen;  /* block bit count*/
	unsigned short    temporal;
	char              bitmap[0];
	/* block parity array */
	/* LP map */
} fc_old_header_info_t;
#pragma pack()
