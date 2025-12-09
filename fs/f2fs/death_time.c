#include "f2fs.h"
#include "linux/types.h"
#include "node.h"

#include "linux/compiler.h"
#include "linux/gfp_types.h"
#include "linux/jiffies.h"
#include "linux/slab.h"
#include "linux/stddef.h"
#include <linux/atomic.h>
#include <linux/xarray.h>
#include <trace/events/f2fs.h>

#define CHUNK_SIZE 16      // No. of sectors in each chunk
#define weight 70           // For weighted moving average calculations

static struct kmem_cache *death_time_kmem_cache;
static struct kmem_cache *chunk_dt_kmem_cache;

static atomic_t max_death_time;

inline unsigned int icbrt(unsigned int x) {
    int s;
    unsigned int y = 0;
    unsigned int b;
  
    for (s = 31; s >= 0; s -= 3) {
      y += y;
      b = 3*y*(y + 1) + 1;
      if ((x >> s) >= b) {
        x -= b << s;
        y++;
      }
    }
    return y;
  }
  

void init_death_time_info(struct f2fs_inode_info *f2fs_inode, struct f2fs_sb_info *sbi) {
    if (unlikely(death_time_kmem_cache == NULL)) {
        death_time_kmem_cache = f2fs_kmem_cache_create("f2fs_death_time_cache",
			sizeof(struct f2fs_death_time_info));
        chunk_dt_kmem_cache = f2fs_kmem_cache_create("f2fs_chunk_death_time_cache", sizeof(struct f2fs_chunk_death_time_info));
    }
    struct f2fs_death_time_info *dt_info = f2fs_kmem_cache_alloc(death_time_kmem_cache, GFP_KERNEL, true, sbi);
    xa_init(&dt_info->per_blk_info);
    f2fs_inode->death_time_info = dt_info;
    trace_f2fs_death_time_struct_init(&f2fs_inode->vfs_inode);
}

void free_death_time_info(struct f2fs_inode_info *f2fs_inode) {
    unsigned long index;
    void *entry;

    xa_for_each(&f2fs_inode->death_time_info->per_blk_info, index, entry) {
        kmem_cache_free(chunk_dt_kmem_cache, entry);
    }
    xa_destroy(&f2fs_inode->death_time_info->per_blk_info);
    kmem_cache_free(death_time_kmem_cache, (void *)f2fs_inode->death_time_info);
    trace_f2fs_death_time_struct_free(&f2fs_inode->vfs_inode);
}

inline void *_init_chunk_death_time_info(struct f2fs_sb_info *sbi) {
    return f2fs_kmem_cache_alloc(chunk_dt_kmem_cache, GFP_KERNEL, true, sbi);
}

void f2fs_update_death_time_info(struct f2fs_io_info *fio, struct f2fs_inode_info *f2fs_inode) {
    unsigned int now_msecs = jiffies_to_msecs(jiffies);
    block_t file_offset_pages = fio->folio->index;
    block_t chunk_offset = file_offset_pages / CHUNK_SIZE;

    struct f2fs_chunk_death_time_info *chunk_info = xa_load(&f2fs_inode->death_time_info->per_blk_info, chunk_offset);
    unsigned int max_death_time = atomic_read(&fio->sbi->max_death_time);

    if (chunk_info == NULL) {        
        chunk_info = f2fs_kmem_cache_alloc(chunk_dt_kmem_cache, GFP_KERNEL, true, fio->sbi);
        chunk_info->last_updated_ms = now_msecs;
        chunk_info->avg_death_time = 0;
        trace_f2fs_death_time_update(&f2fs_inode->vfs_inode, file_offset_pages, now_msecs, 0);
        xa_store(&f2fs_inode->death_time_info->per_blk_info, chunk_offset, (void *)chunk_info, GFP_KERNEL);
    } else if (chunk_info->last_updated_ms < now_msecs) {
        // Don't update too frequently
        unsigned int new_death_time = (now_msecs - chunk_info->last_updated_ms);
        unsigned int new_dt_avg = (chunk_info->avg_death_time == 0) ? new_death_time:  (chunk_info->avg_death_time * (100 - weight) + new_death_time * weight) / 100;
        chunk_info->last_updated_ms = now_msecs;
        chunk_info->avg_death_time = new_dt_avg;
        xa_store(&f2fs_inode->death_time_info->per_blk_info, chunk_offset, (void *)chunk_info, GFP_KERNEL);
        
        trace_f2fs_death_time_update(&f2fs_inode->vfs_inode, file_offset_pages, now_msecs, new_dt_avg);
        if (new_death_time > max_death_time) {
            // Don't retry if the compare exchange fails
            int ret = atomic_cmpxchg_relaxed(&fio->sbi->max_death_time, max_death_time, new_death_time);
            if (ret) {
                trace_f2fs_max_death_time_updated(max_death_time, new_death_time);
            }
        }
    }
}

// Assumption: 3 data streams
int f2fs_get_segment_type_from_death_time(struct inode *inode, block_t file_offset) {
    block_t chunk_offset = file_offset / CHUNK_SIZE;

    struct f2fs_inode_info *f2fs_inode = F2FS_I(inode);
    struct f2fs_chunk_death_time_info *chunk_info = xa_load(&f2fs_inode->death_time_info->per_blk_info, chunk_offset);
    if (unlikely(chunk_info == NULL)) return NO_CHECK_TYPE;

    struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
    unsigned int max_death_time = atomic_read(&sbi->max_death_time);
    unsigned int hot_threshold = icbrt(max_death_time);
    unsigned int warm_threshold = hot_threshold * hot_threshold;
    int segment = CURSEG_WARM_DATA;
    if (chunk_info->avg_death_time != 0) {
        segment = (chunk_info->avg_death_time < hot_threshold) ? CURSEG_HOT_DATA :
            (chunk_info->avg_death_time < warm_threshold) ? CURSEG_WARM_DATA : 
                CURSEG_COLD_DATA;
    }
    trace_f2fs_death_time_predict(inode, file_offset, chunk_info->avg_death_time, segment);
    return segment;
}