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
 */
#include "ctree.h"
#include "dedupe.h"
#include "btrfs_inode.h"
#include "transaction.h"
#include "delayed-ref.h"

struct inmem_hash {
	struct rb_node hash_node;
	struct rb_node bytenr_node;
	struct list_head lru_list;

	u64 bytenr;
	u32 num_bytes;

	u8 hash[];
};

static inline struct inmem_hash *inmem_alloc_hash(u16 algo)
{
	if (WARN_ON(algo >= ARRAY_SIZE(btrfs_hash_sizes)))
		return NULL;
	return kzalloc(sizeof(struct inmem_hash) + btrfs_hash_sizes[algo],
			GFP_NOFS);
}

static int init_dedupe_info(struct btrfs_dedupe_info **ret_info,
			    struct btrfs_ioctl_dedupe_args *dargs)
{
	struct btrfs_dedupe_info *dedupe_info;

	dedupe_info = kzalloc(sizeof(*dedupe_info), GFP_NOFS);
	if (!dedupe_info)
		return -ENOMEM;

	dedupe_info->hash_algo = dargs->hash_algo;
	dedupe_info->backend = dargs->backend;
	dedupe_info->blocksize = dargs->blocksize;
	dedupe_info->limit_nr = dargs->limit_nr;

	/* only support SHA256 yet */
	dedupe_info->dedupe_driver = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(dedupe_info->dedupe_driver)) {
		int ret;

		ret = PTR_ERR(dedupe_info->dedupe_driver);
		kfree(dedupe_info);
		return ret;
	}

	dedupe_info->hash_root = RB_ROOT;
	dedupe_info->bytenr_root = RB_ROOT;
	dedupe_info->current_nr = 0;
	INIT_LIST_HEAD(&dedupe_info->lru_list);
	mutex_init(&dedupe_info->lock);

	*ret_info = dedupe_info;
	return 0;
}

/*
 * Helper to check if parameters are valid.
 * The first invalid field will be set to (-1), to info user which parameter
 * is invalid.
 * Except dargs->limit_nr or dargs->limit_mem, in that case, 0 will returned
 * to info user, since user can specify any value to limit, except 0.
 */
static int check_dedupe_parameter(struct btrfs_fs_info *fs_info,
				  struct btrfs_ioctl_dedupe_args *dargs)
{
	u64 blocksize = dargs->blocksize;
	u64 limit_nr = dargs->limit_nr;
	u64 limit_mem = dargs->limit_mem;
	u16 hash_algo = dargs->hash_algo;
	u8 backend = dargs->backend;

	/*
	 * Set all reserved fields to -1, allow user to detect
	 * unsupported optional parameters.
	 */
	memset(dargs->__unused, -1, sizeof(dargs->__unused));
	if (blocksize > BTRFS_DEDUPE_BLOCKSIZE_MAX ||
	    blocksize < BTRFS_DEDUPE_BLOCKSIZE_MIN ||
	    blocksize < fs_info->sectorsize ||
	    !is_power_of_2(blocksize) ||
	    blocksize < PAGE_SIZE) {
		dargs->blocksize = (u64)-1;
		return -EINVAL;
	}
	if (hash_algo >= ARRAY_SIZE(btrfs_hash_sizes)) {
		dargs->hash_algo = (u16)-1;
		return -EINVAL;
	}
	if (backend >= BTRFS_DEDUPE_BACKEND_COUNT) {
		dargs->backend = (u8)-1;
		return -EINVAL;
	}

	/* Backend specific check */
	if (backend == BTRFS_DEDUPE_BACKEND_INMEMORY) {
		/* only one limit is accepted for enable*/
		if (dargs->limit_nr && dargs->limit_mem) {
			dargs->limit_nr = 0;
			dargs->limit_mem = 0;
			return -EINVAL;
		}

		if (!limit_nr && !limit_mem)
			dargs->limit_nr = BTRFS_DEDUPE_LIMIT_NR_DEFAULT;
		else {
			u64 tmp = (u64)-1;

			if (limit_mem) {
				tmp = limit_mem / (sizeof(struct inmem_hash) +
					btrfs_hash_sizes[hash_algo]);
				/* Too small limit_mem to fill a hash item */
				if (!tmp) {
					dargs->limit_mem = 0;
					dargs->limit_nr = 0;
					return -EINVAL;
				}
			}
			if (!limit_nr)
				limit_nr = (u64)-1;

			dargs->limit_nr = min(tmp, limit_nr);
		}
	}
	if (backend == BTRFS_DEDUPE_BACKEND_ONDISK)
		dargs->limit_nr = 0;

	return 0;
}

int btrfs_dedupe_enable(struct btrfs_fs_info *fs_info,
			struct btrfs_ioctl_dedupe_args *dargs)
{
	struct btrfs_dedupe_info *dedupe_info;
	int ret = 0;

	ret = check_dedupe_parameter(fs_info, dargs);
	if (ret < 0)
		return ret;

	dedupe_info = fs_info->dedupe_info;
	if (dedupe_info) {
		/* Check if we are re-enable for different dedupe config */
		if (dedupe_info->blocksize != dargs->blocksize ||
		    dedupe_info->hash_algo != dargs->hash_algo ||
		    dedupe_info->backend != dargs->backend) {
			btrfs_dedupe_disable(fs_info);
			goto enable;
		}

		/* On-fly limit change is OK */
		mutex_lock(&dedupe_info->lock);
		fs_info->dedupe_info->limit_nr = dargs->limit_nr;
		mutex_unlock(&dedupe_info->lock);
		return 0;
	}

enable:
	ret = init_dedupe_info(&dedupe_info, dargs);
	if (ret < 0)
		return ret;
	fs_info->dedupe_info = dedupe_info;
	/* We must ensure dedupe_bs is modified after dedupe_info */
	smp_wmb();
	fs_info->dedupe_enabled = 1;
	return ret;
}

int btrfs_dedupe_disable(struct btrfs_fs_info *fs_info)
{
	/* Place holder for bisect, will be implemented in later patches */
	return 0;
}

static int inmem_insert_hash(struct rb_root *root,
			     struct inmem_hash *hash, int hash_len)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, hash_node);
		if (memcmp(hash->hash, entry->hash, hash_len) < 0)
			p = &(*p)->rb_left;
		else if (memcmp(hash->hash, entry->hash, hash_len) > 0)
			p = &(*p)->rb_right;
		else
			return 1;
	}
	rb_link_node(&hash->hash_node, parent, p);
	rb_insert_color(&hash->hash_node, root);
	return 0;
}

static int inmem_insert_bytenr(struct rb_root *root,
			       struct inmem_hash *hash)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, bytenr_node);
		if (hash->bytenr < entry->bytenr)
			p = &(*p)->rb_left;
		else if (hash->bytenr > entry->bytenr)
			p = &(*p)->rb_right;
		else
			return 1;
	}
	rb_link_node(&hash->bytenr_node, parent, p);
	rb_insert_color(&hash->bytenr_node, root);
	return 0;
}

static void __inmem_del(struct btrfs_dedupe_info *dedupe_info,
			struct inmem_hash *hash)
{
	list_del(&hash->lru_list);
	rb_erase(&hash->hash_node, &dedupe_info->hash_root);
	rb_erase(&hash->bytenr_node, &dedupe_info->bytenr_root);

	if (!WARN_ON(dedupe_info->current_nr == 0))
		dedupe_info->current_nr--;

	kfree(hash);
}

/*
 * Insert a hash into in-memory dedupe tree
 * Will remove exceeding last recent use hash.
 *
 * If the hash mathced with existing one, we won't insert it, to
 * save memory
 */
static int inmem_add(struct btrfs_dedupe_info *dedupe_info,
		     struct btrfs_dedupe_hash *hash)
{
	int ret = 0;
	u16 algo = dedupe_info->hash_algo;
	struct inmem_hash *ihash;

	ihash = inmem_alloc_hash(algo);

	if (!ihash)
		return -ENOMEM;

	/* Copy the data out */
	ihash->bytenr = hash->bytenr;
	ihash->num_bytes = hash->num_bytes;
	memcpy(ihash->hash, hash->hash, btrfs_hash_sizes[algo]);

	mutex_lock(&dedupe_info->lock);

	ret = inmem_insert_bytenr(&dedupe_info->bytenr_root, ihash);
	if (ret > 0) {
		kfree(ihash);
		ret = 0;
		goto out;
	}

	ret = inmem_insert_hash(&dedupe_info->hash_root, ihash,
				btrfs_hash_sizes[algo]);
	if (ret > 0) {
		/*
		 * We only keep one hash in tree to save memory, so if
		 * hash conflicts, free the one to insert.
		 */
		rb_erase(&ihash->bytenr_node, &dedupe_info->bytenr_root);
		kfree(ihash);
		ret = 0;
		goto out;
	}

	list_add(&ihash->lru_list, &dedupe_info->lru_list);
	dedupe_info->current_nr++;

	/* Remove the last dedupe hash if we exceed limit */
	while (dedupe_info->current_nr > dedupe_info->limit_nr) {
		struct inmem_hash *last;

		last = list_entry(dedupe_info->lru_list.prev,
				  struct inmem_hash, lru_list);
		__inmem_del(dedupe_info, last);
	}
out:
	mutex_unlock(&dedupe_info->lock);
	return 0;
}

int btrfs_dedupe_add(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info,
		     struct btrfs_dedupe_hash *hash)
{
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;

	if (!fs_info->dedupe_enabled || !hash)
		return 0;

	if (WARN_ON(dedupe_info == NULL))
		return -EINVAL;

	if (WARN_ON(!btrfs_dedupe_hash_hit(hash)))
		return -EINVAL;

	/* ignore old hash */
	if (dedupe_info->blocksize != hash->num_bytes)
		return 0;

	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY)
		return inmem_add(dedupe_info, hash);
	return -EINVAL;
}
