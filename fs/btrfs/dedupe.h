/*
 * Copyright (C) 2016 Fujitsu.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#ifndef __BTRFS_DEDUPE__
#define __BTRFS_DEDUPE__

#include <linux/btrfs.h>
#include <linux/wait.h>
#include <crypto/hash.h>
#include "btrfs_inode.h"

static const int btrfs_hash_sizes[] = { 32 };

/*
 * For caller outside of dedupe.c
 *
 * Different dedupe backends should have their own hash structure
 */
struct btrfs_dedupe_hash {
	u64 bytenr;
	u32 num_bytes;

	/* last field is a variable length array of dedupe hash */
	u8 hash[];
};

struct btrfs_dedupe_info {
	/* dedupe blocksize */
	u64 blocksize;
	u16 backend;
	u16 hash_algo;

	struct crypto_shash *dedupe_driver;

	/*
	 * Use mutex to portect both backends
	 * Even for in-memory backends, the rb-tree can be quite large,
	 * so mutex is better for such use case.
	 */
	struct mutex lock;

	/* following members are only used in in-memory backend */
	struct rb_root hash_root;
	struct rb_root bytenr_root;
	struct list_head lru_list;
	u64 limit_nr;
	u64 current_nr;
};

struct btrfs_trans_handle;

static inline u64 btrfs_dedupe_blocksize(struct btrfs_inode *inode)
{
	struct btrfs_fs_info *fs_info = inode->root->fs_info;

	return fs_info->dedupe_info->blocksize;
}

static inline int inode_need_dedupe(struct inode *inode)
{
	struct btrfs_fs_info *fs_info = BTRFS_I(inode)->root->fs_info;

	if (!fs_info->dedupe_enabled)
		return 0;

	return 1;
}

static inline int btrfs_dedupe_hash_hit(struct btrfs_dedupe_hash *hash)
{
	return (hash && hash->bytenr);
}

static inline int btrfs_dedupe_hash_size(u16 algo)
{
	if (WARN_ON(algo >= ARRAY_SIZE(btrfs_hash_sizes)))
		return -EINVAL;
	return sizeof(struct btrfs_dedupe_hash) + btrfs_hash_sizes[algo];
}

static inline struct btrfs_dedupe_hash *btrfs_dedupe_alloc_hash(u16 algo)
{
	return kzalloc(btrfs_dedupe_hash_size(algo), GFP_NOFS);
}

/*
 * Initial inband dedupe info
 * Called at dedupe enable time.
 *
 * Return 0 for success
 * Return <0 for any error
 * (from unsupported param to tree creation error for some backends)
 */
int btrfs_dedupe_enable(struct btrfs_fs_info *fs_info,
			struct btrfs_ioctl_dedupe_args *dargs);

/*
 * Reconfigure given parameter for dedupe
 * Can only be called when dedupe is already enabled
 *
 * dargs member which don't need to be modified should be left
 * with 0 for limit_nr/limit_offset or -1 for other fields
 *
 * Return 0 for success
 * Return <0 for any error
 * (Same error return value with dedupe_enable)
 */
int btrfs_dedupe_reconfigure(struct btrfs_fs_info *fs_info,
			     struct btrfs_ioctl_dedupe_args *dargs);

/*
 * Get inband dedupe info
 * Since it needs to access different backends' hash size, which
 * is not exported, we need such simple function.
 */
void btrfs_dedupe_status(struct btrfs_fs_info *fs_info,
			 struct btrfs_ioctl_dedupe_args *dargs);

/*
 * Disable dedupe and invalidate all its dedupe data.
 * Called at dedupe disable time.
 *
 * Return 0 for success
 * Return <0 for any error
 * (tree operation error for some backends)
 */
int btrfs_dedupe_disable(struct btrfs_fs_info *fs_info);

/*
 * Cleanup current btrfs_dedupe_info
 * Called in umount time
 */
int btrfs_dedupe_cleanup(struct btrfs_fs_info *fs_info);

/*
 * Calculate hash for dedupe.
 * Caller must ensure [start, start + dedupe_bs) has valid data.
 *
 * Return 0 for success
 * Return <0 for any error
 * (error from hash codes)
 */
int btrfs_dedupe_calc_hash(struct btrfs_fs_info *fs_info,
			   struct inode *inode, u64 start,
			   struct btrfs_dedupe_hash *hash);

/*
 * Search for duplicated extents by calculated hash
 * Caller must call btrfs_dedupe_calc_hash() first to get the hash.
 *
 * @inode: the inode for we are writing
 * @file_pos: offset inside the inode
 * As we will increase extent ref immediately after a hash match,
 * we need @file_pos and @inode in this case.
 *
 * Return > 0 for a hash match, and the extent ref will be
 * *INCREASED*, and hash->bytenr/num_bytes will record the existing
 * extent data.
 * Return 0 for a hash miss. Nothing is done
 * Return <0 for any error
 * (tree operation error for some backends)
 */
int btrfs_dedupe_search(struct btrfs_fs_info *fs_info,
			struct inode *inode, u64 file_pos,
			struct btrfs_dedupe_hash *hash);

/*
 * Add a dedupe hash into dedupe info
 * Return 0 for success
 * Return <0 for any error
 * (tree operation error for some backends)
 */
int btrfs_dedupe_add(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct btrfs_dedupe_hash *hash);

/*
 * Remove a dedupe hash from dedupe info
 * Return 0 for success
 * Return <0 for any error
 * (tree operation error for some backends)
 *
 * NOTE: if hash deletion error is not handled well, it will lead
 * to corrupted fs, as later dedupe write can points to non-exist or even
 * wrong extent.
 */
int btrfs_dedupe_del(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info, u64 bytenr);
#endif
