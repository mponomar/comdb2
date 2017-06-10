cmake_minimum_required (VERSION 3.5)
project (comdb2)

set(FAIL -Wno-format -Wno-format-security -Wno-pointer-to-int-cast)
set(CMAKE_C_COMPILER gcc)

set(CFLAGS -D_LINUX_SOURCE -D_GNU_SOURCE -D_XOPEN_SOURCE=600 
           -DSTDC_HEADERS -D_SYS_SYSMACROS_H -DBB_THREADED 
           -D_REENTRANT -D_THREAD_SAFE -DPER_THREAD_MALLOC 
           -DMONITOR_STACK
           ${FAIL}
           -DCOMDB2_ROOT=/opt/bb
		   -Ibb -Ibbinc -Idlmalloc)

add_library(cdb2api STATIC
        cdb2api/cdb2api.c)

add_library(bdb STATIC
    bdb/callback.c bdb/rep.c bdb/bdb.c				
    bdb/bdb_schemachange.c bdb/tranread.c bdb/threads.c bdb/count.c	
    bdb/summarize.c bdb/del.c bdb/llog_auto.c bdb/cursor_rowlocks.c	
    bdb/bdb_access.c bdb/bdb_verify.c bdb/upd.c bdb/genid.c		
    bdb/fstdump.c bdb/rowlocks.c bdb/bdb_osqlbkfill.c bdb/compact.c	
    bdb/compress.c bdb/cursor_ll.c bdb/rowlocks_util.c bdb/odh.c	
    bdb/util.c bdb/temphash.c bdb/bdb_thd_io.c bdb/error.c		
    bdb/serializable.c bdb/ll.c bdb/bdb_osqltrn.c	
    bdb/temptable.c bdb/add.c bdb/tran.c bdb/os_namemangle.c		
    bdb/lite.c bdb/bdb_net.c bdb/locks.c bdb/bdb_osqllog.c bdb/file.c	
    bdb/llmeta.c bdb/queue.c bdb/custom_recover.c bdb/info.c		
    bdb/bdb_osqlcur.c bdb/cursor.c bdb/fetch.c bdb/read.c bdb/phys.c	
    bdb/bdblock.c bdb/attr.c bdb/locktest.c bdb/berktest.c		
    bdb/bdb_llops.c bdb/bdb_blkseq.c bdb/queuedb.c
    bdb/llog_auto.c 
    )


add_library(bb STATIC
        bb/averager.c
        bb/bb_asprintf.c
        bb/bbhrtime.c
        bb/bb_oscompat.c
        bb/cheapstub.c
        bb/comdb2file.c
        bb/comdb2_pthread_create.c
        bb/ctrace.c
        bb/debug_switches.c
        bb/flibc.c
        bb/fsnapf.c
        bb/intern_strings.c
        bb/int_overflow.c
        bb/list.c
        bb/lockassert.c
        bb/logmsg.c
        bb/mem.c
        bb/misc.c
        bb/nodemap.c
        bb/object_pool.c
        bb/passfd.c
        bb/plhash.c
        bb/pool.c
        bb/pooltest.c
        bb/portmuxusr.c
        bb/queue.c
        bb/queuetest.c
        bb/roll_file.c
        bb/rtcpu.c
        bb/safestrerror.c
        bb/sbuf2.c
        bb/segstring.c
        bb/sltpck.c
        bb/ssl_support.c
        bb/str0.c
        bb/strbuf.c
        bb/switches.c
        bb/tcputil.c
        bb/thdpool.c
        bb/thread_malloc.c
        bb/thread_util.c
        bb/timers.c
        bb/utilmisc.c
        bb/walkback.c
        bb/xstring.c)

add_library(berkdb STATIC
berkdb/btree/bt_compare.c berkdb/btree/bt_conv.c        
berkdb/btree/bt_curadj.c berkdb/btree/bt_cursor.c              
berkdb/btree/bt_delete.c berkdb/btree/bt_method.c              
berkdb/btree/bt_open.c berkdb/btree/bt_prefix.c berkdb/btree/bt_put.c      
berkdb/btree/bt_rec.c berkdb/btree/bt_reclaim.c                
berkdb/btree/bt_recno.c berkdb/btree/bt_rsearch.c              
berkdb/btree/bt_search.c berkdb/btree/bt_split.c               
berkdb/btree/bt_stat.c berkdb/btree/bt_upgrade.c               
berkdb/btree/bt_verify.c berkdb/btree/bt_cache.c berkdb/btree/bt_pf.c      
berkdb/btree/bt_pgcompact.c
berkdb/common/db_byteorder.c berkdb/common/db_err.c 
berkdb/common/db_getlong.c berkdb/common/db_idspace.c              
berkdb/common/db_log2.c berkdb/common/util_cache.c             
berkdb/common/util_sig.c
berkdb/crypto/aes_method.c berkdb/crypto/crypto.c      
berkdb/crypto/mersenne/mt19937db.c
berkdb/db/crdel_rec.c berkdb/db/db.c berkdb/db/db_am.c  
berkdb/db/db_cam.c berkdb/db/db_conv.c berkdb/db/db_dispatch.c         
berkdb/db/db_dup.c berkdb/db/db_iface.c berkdb/db/db_join.c        
berkdb/db/db_meta.c berkdb/db/db_method.c berkdb/db/db_open.c          
berkdb/db/db_overflow.c berkdb/db/db_ovfl_vrfy.c berkdb/db/db_pr.c  
berkdb/db/db_rec.c berkdb/db/db_reclaim.c berkdb/db/db_remove.c        
berkdb/db/db_rename.c berkdb/db/db_ret.c berkdb/db/db_truncate.c       
berkdb/db/db_upg.c berkdb/db/db_upg_opd.c berkdb/db/db_vrfy.c          
berkdb/db/db_vrfyutil.c berkdb/db/db_pgdump.c      
berkdb/db/db_pgcompact.c
berkdb/dbreg/dbreg.c berkdb/dbreg/dbreg_rec.c       
berkdb/dbreg/dbreg_util.c
berkdb/env/db_malloc.c berkdb/env/db_salloc.c  
berkdb/env/db_shash.c berkdb/env/env_attr.c            
berkdb/env/env_file.c berkdb/env/env_method.c          
berkdb/env/env_open.c berkdb/env/env_recover.c         
berkdb/env/env_region.c berkdb/env/env_pgcompact.c
berkdb/fileops/fop_basic.c berkdb/fileops/fop_rec.c       
berkdb/fileops/fop_util.c
berkdb/hash/hash.c berkdb/hash/hash_conv.c       
berkdb/hash/hash_dup.c berkdb/hash/hash_func.c             
berkdb/hash/hash_meta.c berkdb/hash/hash_method.c          
berkdb/hash/hash_open.c berkdb/hash/hash_page.c            
berkdb/hash/hash_rec.c berkdb/hash/hash_reclaim.c          
berkdb/hash/hash_stat.c berkdb/hash/hash_upgrade.c         
berkdb/hash/hash_verify.c
berkdb/hmac/hmac.c
berkdb/lock/lock.c berkdb/lock/lock_deadlock.c       
berkdb/lock/lock_method.c berkdb/lock/lock_region.c        
berkdb/lock/lock_stat.c berkdb/lock/lock_util.c
berkdb/log/log.c berkdb/log/log_archive.c             
berkdb/log/log_compare.c berkdb/log/log_get.c berkdb/log/log_method.c      
berkdb/log/log_put.c
berkdb/mp/mp_alloc.c berkdb/mp/mp_bh.c         
berkdb/mp/mp_fget.c berkdb/mp/mp_fopen.c berkdb/mp/mp_fput.c       
berkdb/mp/mp_fset.c berkdb/mp/mp_method.c berkdb/mp/mp_region.c 
berkdb/mp/mp_register.c berkdb/mp/mp_stat.c berkdb/mp/mp_sync.c 
berkdb/mp/mp_trickle.c
berkdb/mutex/mut_pthread.c berkdb/mutex/mutex.c
berkdb/os/os_abs.c berkdb/os/os_alloc.c            
berkdb/os/os_clock.c berkdb/os/os_config.c berkdb/os/os_dir.c          
berkdb/os/os_errno.c berkdb/os/os_fid.c berkdb/os/os_fsync.c           
berkdb/os/os_handle.c berkdb/os/os_id.c berkdb/os/os_map.c         
berkdb/os/os_method.c berkdb/os/os_oflags.c berkdb/os/os_open.c        
berkdb/os/os_region.c berkdb/os/os_rename.c berkdb/os/os_root.c     
berkdb/os/os_rpath.c berkdb/os/os_rw.c berkdb/os/os_seek.c      
berkdb/os/os_sleep.c berkdb/os/os_spin.c berkdb/os/os_stat.c        
berkdb/os/os_tmpdir.c berkdb/os/os_unlink.c berkdb/os/os_falloc.c
berkdb/qam/qam.c berkdb/qam/qam_conv.c         
berkdb/qam/qam_files.c berkdb/qam/qam_method.c berkdb/qam/qam_open.c    
berkdb/qam/qam_rec.c berkdb/qam/qam_stat.c berkdb/qam/qam_upgrade.c 
berkdb/qam/qam_verify.c
berkdb/rep/rep_method.c berkdb/rep/rep_record.c    
berkdb/rep/rep_region.c berkdb/rep/rep_util.c           
berkdb/rep/rep_lc_cache.c
berkdb/btree/btree_auto.c   
berkdb/db/crdel_auto.c      
berkdb/db/db_auto.c     
berkdb/dbreg/dbreg_auto.c   
berkdb/fileops/fileops_auto.c   
berkdb/hash/hash_auto.c     
berkdb/qam/qam_auto.c 
)

add_executable(comdb2 test.c)
add_dependencies(bb mem_bb)
add_dependencies(cdb2api protobuf)
add_dependencies(bdb berkdb)
add_dependencies(comdb2 cdb2api bdb bb berkdb)

add_custom_target(mem_bb 
                  COMMENT "build mem files"
                  COMMAND SRCHOME=/home/mike/comdb2/src ./bb/mem_codegen.sh
                  COMMAND SRCHOME=/home/mike/comdb2/src ./bb/mem_codegen.sh bb bb
                  COMMAND SRCHOME=/home/mike/comdb2/src ./bb/mem_codegen.sh berkdb berkdb
                  COMMAND SRCHOME=/home/mike/comdb2/src ./bb/mem_codegen.sh bdb bdb
        )

add_custom_command(
        OUTPUT bdb/llog_auto.c
        DEPENDS bdb/llog.src
        COMMAND bdb/build_recfiles.sh llog.src bdb berkdb
        )

add_custom_target(protobuf 
                  COMMAND  protoc-c -Iprotobuf protobuf/sqlquery.proto --c_out=protobuf
                  COMMAND  protoc-c -Iprotobuf protobuf/sqlresponse.proto --c_out=protobuf
                  COMMAND  protoc-c -Iprotobuf protobuf/bpfunc.proto --c_out=protobuf)

add_custom_command( 
        OUTPUT  berkdb/btree/btree_auto.c berkdb/db/crdel_auto.c berkdb/db/db_auto.c berkdb/dbreg/dbreg_auto.c berkdb/fileops/fileops_auto.c berkdb/hash/hash_auto.c berkdb/qam/qam_auto.c  berkdb/dbinc_auto/btree_auto.h berkdb/dbinc_auto/crdel_auto.h berkdb/dbinc_auto/db_auto.h berkdb/dbinc_auto/dbreg_auto.h berkdb/dbinc_auto/fileops_auto.h berkdb/dbinc_auto/hash_auto.h berkdb/dbinc_auto/qam_auto.h berkdb/dbinc_auto/xa_ext.h berkdb/dbinc_auto/rep_ext.h berkdb/dbinc_auto/qam_ext.h berkdb/dbinc_auto/os_ext.h berkdb/dbinc_auto/mutex_ext.h berkdb/dbinc_auto/mp_ext.h berkdb/dbinc_auto/log_ext.h berkdb/dbinc_auto/lock_ext.h berkdb/dbinc_auto/hmac_ext.h berkdb/dbinc_auto/hash_ext.h berkdb/dbinc_auto/fileops_ext.h berkdb/dbinc_auto/env_ext.h berkdb/dbinc_auto/dbreg_ext.h berkdb/dbinc_auto/db_ext.h berkdb/dbinc_auto/crypto_ext.h berkdb/dbinc_auto/common_ext.h berkdb/dbinc_auto/btree_ext.h
	    DEPENDS berkdb/dbreg/dbreg.src berkdb/db/db.src berkdb/db/crdel.src berkdb/hash/hash.src berkdb/qam/qam.src berkdb/fileops/fileops.src berkdb/btree/btree.src 
		COMMAND berkdb/dist/genrec.sh berkdb/hash/hash.src berkdb/hash/hash_auto.c berkdb/dbinc_auto/hash_auto.h berkdb/dist/template/rec_hash
		COMMAND berkdb/dist/genrec.sh berkdb/qam/qam.src berkdb/qam/qam_auto.c berkdb/dbinc_auto/qam_auto.h berkdb/dist/template/rec_qam
		COMMAND berkdb/dist/genrec.sh berkdb/fileops/fileops.src berkdb/fileops/fileops_auto.c berkdb/dbinc_auto/fileops_auto.h berkdb/dist/template/rec_fileops
		COMMAND berkdb/dist/genrec.sh berkdb/dbreg/dbreg.src berkdb/dbreg/dbreg_auto.c berkdb/dbinc_auto/dbreg_auto.h berkdb/dist/template/rec_dbreg
		COMMAND berkdb/dist/genrec.sh berkdb/db/crdel.src berkdb/db/crdel_auto.c berkdb/dbinc_auto/crdel_auto.h berkdb/dist/template/rec_crdel
		COMMAND berkdb/dist/genrec.sh berkdb/db/db.src berkdb/db/db_auto.c berkdb/dbinc_auto/db_auto.h berkdb/dist/template/rec_db
		COMMAND berkdb/dist/genrec.sh berkdb/btree/btree.src berkdb/btree/btree_auto.c berkdb/dbinc_auto/btree_auto.h berkdb/dist/template/rec_btree
		COMMAND berkdb/dist/geninc.sh hash berkdb/dbinc_auto/hash_ext.h berkdb/hash/hash_auto.c berkdb/hash/hash.c berkdb/hash/hash_conv.c berkdb/hash/hash_dup.c berkdb/hash/hash_func.c berkdb/hash/hash_meta.c berkdb/hash/hash_method.c berkdb/hash/hash_open.c berkdb/hash/hash_page.c berkdb/hash/hash_rec.c berkdb/hash/hash_reclaim.c berkdb/hash/hash_stat.c berkdb/hash/hash_upgrade.c berkdb/hash/hash_verify.c
		COMMAND berkdb/dist/geninc.sh crypto berkdb/dbinc_auto/crypto_ext.h berkdb/crypto/aes_method.c berkdb/crypto/crypto.c berkdb/crypto/mersenne/mt19937db.c
		COMMAND berkdb/dist/geninc.sh hmac berkdb/dbinc_auto/hmac_ext.h berkdb/hmac/hmac.c
		COMMAND berkdb/dist/geninc.sh xa berkdb/dbinc_auto/xa_ext.h berkdb/xa/xa.c berkdb/xa/xa_db.c berkdb/xa/xa_map.c
		COMMAND berkdb/dist/geninc.sh rep berkdb/dbinc_auto/rep_ext.h berkdb/rep/rep_method.c berkdb/rep/rep_record.c berkdb/rep/rep_region.c berkdb/rep/rep_util.c berkdb/rep/rep_lc_cache.c
		COMMAND berkdb/dist/geninc.sh qam berkdb/dbinc_auto/qam_ext.h berkdb/qam/qam_auto.c berkdb/qam/qam.c berkdb/qam/qam_conv.c berkdb/qam/qam_files.c berkdb/qam/qam_method.c berkdb/qam/qam_open.c berkdb/qam/qam_rec.c berkdb/qam/qam_stat.c berkdb/qam/qam_upgrade.c berkdb/qam/qam_verify.c
		COMMAND berkdb/dist/geninc.sh os berkdb/dbinc_auto/os_ext.h berkdb/os/os_abs.c berkdb/os/os_alloc.c berkdb/os/os_clock.c berkdb/os/os_config.c berkdb/os/os_dir.c berkdb/os/os_errno.c berkdb/os/os_fid.c berkdb/os/os_fsync.c berkdb/os/os_handle.c berkdb/os/os_id.c berkdb/os/os_map.c berkdb/os/os_method.c berkdb/os/os_oflags.c berkdb/os/os_open.c berkdb/os/os_region.c berkdb/os/os_rename.c berkdb/os/os_root.c berkdb/os/os_rpath.c berkdb/os/os_rw.c berkdb/os/os_seek.c berkdb/os/os_sleep.c berkdb/os/os_spin.c berkdb/os/os_stat.c berkdb/os/os_tmpdir.c berkdb/os/os_unlink.c berkdb/os/os_falloc.c
		COMMAND berkdb/dist/geninc.sh mutex berkdb/dbinc_auto/mutex_ext.h berkdb/mutex/mut_pthread.c berkdb/mutex/mutex.c
		COMMAND berkdb/dist/geninc.sh mp berkdb/dbinc_auto/mp_ext.h berkdb/mp/mp_alloc.c berkdb/mp/mp_bh.c berkdb/mp/mp_fget.c berkdb/mp/mp_fopen.c berkdb/mp/mp_fput.c berkdb/mp/mp_fset.c berkdb/mp/mp_method.c berkdb/mp/mp_region.c berkdb/mp/mp_register.c berkdb/mp/mp_stat.c berkdb/mp/mp_sync.c berkdb/mp/mp_trickle.c
		COMMAND berkdb/dist/geninc.sh log berkdb/dbinc_auto/log_ext.h berkdb/log/log.c berkdb/log/log_archive.c berkdb/log/log_compare.c berkdb/log/log_get.c berkdb/log/log_method.c berkdb/log/log_put.c
		COMMAND berkdb/dist/geninc.sh lock berkdb/dbinc_auto/lock_ext.h berkdb/lock/lock.c berkdb/lock/lock_deadlock.c berkdb/lock/lock_method.c berkdb/lock/lock_region.c berkdb/lock/lock_stat.c berkdb/lock/lock_util.c
		COMMAND berkdb/dist/geninc.sh fileops berkdb/dbinc_auto/fileops_ext.h berkdb/fileops/fileops_auto.c berkdb/fileops/fop_basic.c berkdb/fileops/fop_rec.c berkdb/fileops/fop_util.c
		COMMAND berkdb/dist/geninc.sh env berkdb/dbinc_auto/env_ext.h berkdb/env/db_malloc.c berkdb/env/db_salloc.c berkdb/env/db_shash.c berkdb/env/env_attr.c berkdb/env/env_file.c berkdb/env/env_method.c berkdb/env/env_open.c berkdb/env/env_recover.c berkdb/env/env_region.c berkdb/env/env_pgcompact.c
		COMMAND berkdb/dist/geninc.sh dbreg berkdb/dbinc_auto/dbreg_ext.h berkdb/dbreg/dbreg_auto.c berkdb/dbreg/dbreg.c berkdb/dbreg/dbreg_rec.c berkdb/dbreg/dbreg_util.c
		COMMAND berkdb/dist/geninc.sh db berkdb/dbinc_auto/db_ext.h berkdb/db/crdel_auto.c berkdb/db/db_auto.c berkdb/db/crdel_rec.c berkdb/db/db.c berkdb/db/db_am.c berkdb/db/db_cam.c berkdb/db/db_conv.c berkdb/db/db_dispatch.c berkdb/db/db_dup.c berkdb/db/db_iface.c berkdb/db/db_join.c berkdb/db/db_meta.c berkdb/db/db_method.c berkdb/db/db_open.c berkdb/db/db_overflow.c berkdb/db/db_ovfl_vrfy.c berkdb/db/db_pr.c berkdb/db/db_rec.c berkdb/db/db_reclaim.c berkdb/db/db_remove.c berkdb/db/db_rename.c berkdb/db/db_ret.c berkdb/db/db_truncate.c berkdb/db/db_upg.c berkdb/db/db_upg_opd.c berkdb/db/db_vrfy.c berkdb/db/db_vrfyutil.c berkdb/db/db_pgdump.c berkdb/db/db_pgcompact.c
		COMMAND berkdb/dist/geninc.sh common berkdb/dbinc_auto/common_ext.h berkdb/common/crypto_stub.c berkdb/common/db_byteorder.c berkdb/common/db_err.c berkdb/common/db_getlong.c berkdb/common/db_idspace.c berkdb/common/db_log2.c berkdb/common/util_cache.c berkdb/common/util_sig.c
		COMMAND berkdb/dist/geninc.sh btree berkdb/dbinc_auto/btree_ext.h berkdb/btree/btree_auto.c berkdb/btree/bt_compare.c berkdb/btree/bt_conv.c berkdb/btree/bt_curadj.c berkdb/btree/bt_cursor.c berkdb/btree/bt_delete.c berkdb/btree/bt_method.c berkdb/btree/bt_open.c berkdb/btree/bt_prefix.c berkdb/btree/bt_put.c berkdb/btree/bt_rec.c berkdb/btree/bt_reclaim.c berkdb/btree/bt_recno.c berkdb/btree/bt_rsearch.c berkdb/btree/bt_search.c berkdb/btree/bt_split.c berkdb/btree/bt_stat.c berkdb/btree/bt_upgrade.c berkdb/btree/bt_verify.c berkdb/btree/bt_cache.c berkdb/btree/bt_pf.c berkdb/btree/bt_pgcompact.c)

target_compile_options(bb PRIVATE -Icdb2api ${CFLAGS})
target_compile_options(cdb2api PRIVATE -Iprotobuf ${CFLAGS} -DSBUF2_SERVER=0)
target_compile_options(berkdb PRIVATE -I. -I./dlmalloc  -Iberkdb -Iberkdb/build -Idlmalloc -Icomdb2rle -Icrc32c ${CFLAGS})
target_compile_options(bdb PRIVATE 
                           -DBERKDB_4_2 -I$bdb -Icrc32c 
                           -Iberkdb -Iberkdb/build           
                           -Iberkdb/dbinc -Iberkdb/dbinc_auto    
                           -Ibdb -Idb -I./net -Idlmalloc      
                           -Icomdb2rle -Iprotobuf -Icson  
                           -Ibb -I. ${CFLAGS})
