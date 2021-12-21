# Colors
# 30 - black   34 - blue          40 - black    44 - blue
# 31 - red     35 - magenta       41 - red      45 - magenta
# 32 - green   36 - cyan          42 - green    46 - cyan
# 33 - yellow  37 - white         43 - yellow   47 - white

D=`date +%Y%m%d`


    color='
	    /STAT/              {print "\033[1;32m" $0 "\033[0m"; next}
	    /(WARN|WARNING)/    {print "\033[1;33m" $0 "\033[0m"; next}
	    /(ERROR|CRIT)/      {print "\033[1;31m" $0 "\033[0m"; next}
		/mdb_/				{print "\033[1;33m" $0 "\033[0m"; next}
	    //                  {print "\033[37m" $0 "\033[39m"; next}
				    '

#    //                         {print "\033[37m" $0 "\033[39m"}


stdbuf -i0 -o0 tail -F /usr/service/logs/solproxy/sp_$D.log |stdbuf -i0 -o0 egrep -v "(httpn_request_add_ims_header|Opening|mode|sx_logger|dm_make_report|ID\(KEY\)|Response |Modification|Valid|Dev-ID|Size|Object-properties|Caching|Allocated|reset|probing|update_probe|mdc_|main_|strm_|concurrent|scx_write_system_status|origin_monitor|thread_pool_status|lb_pool|bcm_|yyerror|vm_|blk_ri_destroy|lb_create|httpn_rebind|scx_site|signal_|pl_|handle_try_result|activity_monitor|lb_pool|handle_post|nc_request|lb_|nce_|mpx_handler|Inode Count|Session Alloc|Statistics|disk.IO|: open|Cache Chunk|Block Cache|Inode Cache|Block Refs|cache_monitor|na_adjust|scx_nc_read_complete|skipped|scx_wait_job|object_read|healthcheck)"| stdbuf -i0 -o0 awk "$color"
