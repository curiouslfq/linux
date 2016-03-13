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
#include "qgroup.h"

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

static struct inmem_hash *
inmem_search_bytenr(struct btrfs_dedupe_info *dedupe_info, u64 bytenr)
{
	struct rb_node **p = &dedupe_info->bytenr_root.rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, bytenr_node);

		if (bytenr < entry->bytenr)
			p = &(*p)->rb_left;
		else if (bytenr > entry->bytenr)
			p = &(*p)->rb_right;
		else
			return entry;
	}

	return NULL;
}

/* Delete a hash from in-memory dedupe tree */
static int inmem_del(struct btrfs_dedupe_info *dedupe_info, u64 bytenr)
{
	struct inmem_hash *hash;

	mutex_lock(&dedupe_info->lock);
	hash = inmem_search_bytenr(dedupe_info, bytenr);
	if (!hash) {
		mutex_unlock(&dedupe_info->lock);
		return 0;
	}

	__inmem_del(dedupe_info, hash);
	mutex_unlock(&dedupe_info->lock);
	return 0;
}

/* Remove a dedupe hash from dedupe tree */
int btrfs_dedupe_del(struct btrfs_trans_handle *trans,
		     struct btrfs_fs_info *fs_info, u64 bytenr)
{
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;

	if (!fs_info->dedupe_enabled)
		return 0;

	if (WARN_ON(dedupe_info == NULL))
		return -EINVAL;

	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY)
		return inmem_del(dedupe_info, bytenr);
	return -EINVAL;
}

static void inmem_destroy(struct btrfs_dedupe_info *dedupe_info)
{
	struct inmem_hash *entry, *tmp;

	mutex_lock(&dedupe_info->lock);
	list_for_each_entry_safe(entry, tmp, &dedupe_info->lru_list, lru_list)
		__inmem_del(dedupe_info, entry);
	mutex_unlock(&dedupe_info->lock);
}

/*
 * Helper function to wait and block all incoming writers
 *
 * Use rw_sem introduced for freeze to wait/block writers.
 * So during the block time, no new write will happen, so we can
 * do something quite safe, espcially helpful for dedupe disable,
 * as it affect buffered write.
 */
static void block_all_writers(struct btrfs_fs_info *fs_info)
{
	struct super_block *sb = fs_info->sb;

	percpu_down_write(sb->s_writers.rw_sem + SB_FREEZE_WRITE - 1);
	down_write(&sb->s_umount);
}

static void unblock_all_writers(struct btrfs_fs_info *fs_info)
{
	struct super_block *sb = fs_info->sb;

	up_write(&sb->s_umount);
	percpu_up_write(sb->s_writers.rw_sem + SB_FREEZE_WRITE - 1);
}

int btrfs_dedupe_disable(struct btrfs_fs_info *fs_info)
{
	struct btrfs_dedupe_info *dedupe_info;
	int ret;

	dedupe_info = fs_info->dedupe_info;

	if (!dedupe_info)
		return 0;

	/* Don't allow disable status change in RO mount */
	if (fs_info->sb->s_flags & MS_RDONLY)
		return -EROFS;

	/*
	 * Wait for all unfinished writers and block further writers.
	 * Then sync the whole fs so all current write will go through
	 * dedupe, and all later write won't go through dedupe.
	 */
	block_all_writers(fs_info);
	ret = sync_filesystem(fs_info->sb);
	fs_info->dedupe_enabled = 0;
	fs_info->dedupe_info = NULL;
	unblock_all_writers(fs_info);
	if (ret < 0)
		return ret;

	/* now we are OK to clean up everything */
	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY)
		inmem_destroy(dedupe_info);

	crypto_free_shash(dedupe_info->dedupe_driver);
	kfree(dedupe_info);
	return 0;
}

/*
 * Caller must ensure the corresponding ref head is not being run.
 */
static struct inmem_hash *
inmem_search_hash(struct btrfs_dedupe_info *dedupe_info, u8 *hash)
{
	struct rb_node **p = &dedupe_info->hash_root.rb_node;
	struct rb_node *parent = NULL;
	struct inmem_hash *entry = NULL;
	u16 hash_algo = dedupe_info->hash_algo;
	int hash_len = btrfs_hash_sizes[hash_algo];

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct inmem_hash, hash_node);

		if (memcmp(hash, entry->hash, hash_len) < 0) {
			p = &(*p)->rb_left;
		} else if (memcmp(hash, entry->hash, hash_len) > 0) {
			p = &(*p)->rb_right;
		} else {
			/* Found, need to re-add it to LRU list head */
			list_del(&entry->lru_list);
			list_add(&entry->lru_list, &dedupe_info->lru_list);
			return entry;
		}
	}
	return NULL;
}

static int inmem_search(struct btrfs_dedupe_info *dedupe_info,
			struct inode *inode, u64 file_pos,
			struct btrfs_dedupe_hash *hash)
{
	int ret;
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct btrfs_trans_handle *trans;
	struct btrfs_delayed_ref_root *delayed_refs;
	struct btrfs_delayed_ref_head *head;
	struct btrfs_delayed_ref_head *insert_head;
	struct btrfs_delayed_data_ref *insert_dref;
	struct btrfs_qgroup_extent_record *insert_qrecord = NULL;
	struct inmem_hash *found_hash;
	int free_insert = 1;
	int qrecord_inserted = 0;
	u64 bytenr;
	u32 num_bytes;

	insert_head = kmem_cache_alloc(btrfs_delayed_ref_head_cachep, GFP_NOFS);
	if (!insert_head)
		return -ENOMEM;
	insert_head->extent_op = NULL;
	insert_dref = kmem_cache_alloc(btrfs_delayed_data_ref_cachep, GFP_NOFS);
	if (!insert_dref) {
		kmem_cache_free(btrfs_delayed_ref_head_cachep, insert_head);
		return -ENOMEM;
	}
	if (test_bit(BTRFS_FS_QUOTA_ENABLED, &root->fs_info->flags) &&
	    is_fstree(root->root_key.objectid)) {
		insert_qrecord = kmalloc(sizeof(*insert_qrecord), GFP_NOFS);
		if (!insert_qrecord) {
			kmem_cache_free(btrfs_delayed_ref_head_cachep,
					insert_head);
			kmem_cache_free(btrfs_delayed_data_ref_cachep,
					insert_dref);
			return -ENOMEM;
		}
	}

	trans = btrfs_join_transaction(root);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		goto free_mem;
	}

again:
	mutex_lock(&dedupe_info->lock);
	found_hash = inmem_search_hash(dedupe_info, hash->hash);
	/* If we don't find a duplicated extent, just return. */
	if (!found_hash) {
		ret = 0;
		goto out;
	}
	bytenr = found_hash->bytenr;
	num_bytes = found_hash->num_bytes;

	delayed_refs = &trans->transaction->delayed_refs;

	spin_lock(&delayed_refs->lock);
	head = btrfs_find_delayed_ref_head(&trans->transaction->delayed_refs,
					   bytenr);
	if (!head) {
		/*
		 * We can safely insert a new delayed_ref as long as we
		 * hold delayed_refs->lock.
		 * Only need to use atomic inc_extent_ref()
		 */
		btrfs_add_delayed_data_ref_locked(root->fs_info, trans,
				insert_dref, insert_head, insert_qrecord,
				bytenr, num_bytes, 0, root->root_key.objectid,
				btrfs_ino(BTRFS_I(inode)), file_pos, 0,
				BTRFS_ADD_DELAYED_REF, &qrecord_inserted, NULL,
				NULL);
		spin_unlock(&delayed_refs->lock);

		/* add_delayed_data_ref_locked will free unused memory */
		free_insert = 0;
		hash->bytenr = bytenr;
		hash->num_bytes = num_bytes;
		ret = 1;
		goto out;
	}

	/*
	 * We can't lock ref head with dedupe_info->lock hold or we will cause
	 * ABBA dead lock.
	 */
	mutex_unlock(&dedupe_info->lock);
	ret = btrfs_delayed_ref_lock(trans, head);
	spin_unlock(&delayed_refs->lock);
	if (ret == -EAGAIN)
		goto again;

	mutex_lock(&dedupe_info->lock);
	/* Search again to ensure the hash is still here */
	found_hash = inmem_search_hash(dedupe_info, hash->hash);
	if (!found_hash) {
		ret = 0;
		mutex_unlock(&head->mutex);
		goto out;
	}
	ret = 1;
	hash->bytenr = bytenr;
	hash->num_bytes = num_bytes;

	/*
	 * Increase the extent ref right now, to avoid delayed ref run
	 * Or we may increase ref on non-exist extent.
	 */
	btrfs_inc_extent_ref(trans, root->fs_info, bytenr, num_bytes, 0,
			     root->root_key.objectid,
			     btrfs_ino(BTRFS_I(inode)), file_pos);
	mutex_unlock(&head->mutex);
out:
	mutex_unlock(&dedupe_info->lock);
	btrfs_end_transaction(trans);

free_mem:
	if (free_insert) {
		kmem_cache_free(btrfs_delayed_ref_head_cachep, insert_head);
		kmem_cache_free(btrfs_delayed_data_ref_cachep, insert_dref);
	}
	if (!qrecord_inserted)
		kfree(insert_qrecord);
	return ret;
}

int btrfs_dedupe_search(struct btrfs_fs_info *fs_info,
			struct inode *inode, u64 file_pos,
			struct btrfs_dedupe_hash *hash)
{
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;
	int ret = -EINVAL;

	if (!hash)
		return 0;

	/*
	 * This function doesn't follow fs_info->dedupe_enabled as it will need
	 * to ensure any hashed extent to go through dedupe routine
	 */
	if (WARN_ON(dedupe_info == NULL))
		return -EINVAL;

	if (WARN_ON(btrfs_dedupe_hash_hit(hash)))
		return -EINVAL;

	if (dedupe_info->backend == BTRFS_DEDUPE_BACKEND_INMEMORY)
		ret = inmem_search(dedupe_info, inode, file_pos, hash);

	/* It's possible hash->bytenr/num_bytenr already changed */
	if (ret == 0) {
		hash->num_bytes = 0;
		hash->bytenr = 0;
	}
	return ret;
}

int btrfs_dedupe_calc_hash(struct btrfs_fs_info *fs_info,
			   struct inode *inode, u64 start,
			   struct btrfs_dedupe_hash *hash)
{
	int i;
	int ret;
	struct page *p;
	struct btrfs_dedupe_info *dedupe_info = fs_info->dedupe_info;
	struct crypto_shash *tfm = dedupe_info->dedupe_driver;
	u64 dedupe_bs;
	u64 sectorsize = fs_info->sectorsize;

	SHASH_DESC_ON_STACK(sdesc, tfm);

	if (!fs_info->dedupe_enabled || !hash)
		return 0;

	if (WARN_ON(dedupe_info == NULL))
		return -EINVAL;

	WARN_ON(!IS_ALIGNED(start, sectorsize));

	dedupe_bs = dedupe_info->blocksize;

	sdesc->tfm = tfm;
	sdesc->flags = 0;
	ret = crypto_shash_init(sdesc);
	if (ret)
		return ret;
	for (i = 0; sectorsize * i < dedupe_bs; i++) {
		char *d;

		p = find_get_page(inode->i_mapping,
				  (start >> PAGE_SHIFT) + i);
		if (WARN_ON(!p))
			return -ENOENT;
		d = kmap(p);
		ret = crypto_shash_update(sdesc, d, sectorsize);
		kunmap(p);
		put_page(p);
		if (ret)
			return ret;
	}
	ret = crypto_shash_final(sdesc, hash->hash);
	return ret;
}
