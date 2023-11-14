#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/spinlock_types.h>

#define SBDD_SECTOR_SHIFT      9
#define SBDD_SECTOR_SIZE       (1 << SBDD_SECTOR_SHIFT)
#define SBDD_MIB_SECTORS       (1 << (20 - SBDD_SECTOR_SHIFT))
#define SBDD_NAME              "sbdd"

struct sbdd {
	wait_queue_head_t       exitwait;
	spinlock_t              datalock;
	atomic_t                deleting;
	atomic_t                refs_cnt;
	sector_t                capacity;
	u8                      *data;
	struct gendisk          *gd;
	struct request_queue    *q;

	struct block_device	*bd1;
};

static struct sbdd      __sbdd;
static int              __sbdd_major = 0;
static unsigned long    __sbdd_capacity_mib = 100;

static char*		__sbdd_disk1 = NULL;

static sector_t sbdd_xfer(struct bio_vec* bvec, sector_t pos, int dir)
{
	void *buff = page_address(bvec->bv_page) + bvec->bv_offset;
	sector_t len = bvec->bv_len >> SBDD_SECTOR_SHIFT;
	size_t offset;
	size_t nbytes;

	if (pos + len > __sbdd.capacity)
		len = __sbdd.capacity - pos;

	offset = pos << SBDD_SECTOR_SHIFT;
	nbytes = len << SBDD_SECTOR_SHIFT;

	spin_lock(&__sbdd.datalock);

	if (dir)
		memcpy(__sbdd.data + offset, buff, nbytes);
	else
		memcpy(buff, __sbdd.data + offset, nbytes);

	spin_unlock(&__sbdd.datalock);

	pr_debug("pos=%6llu len=%4llu nbytes=%4lu %s\n", pos, len, nbytes, dir ? "written" : "read");

	return len;
}

static void sbdd_xfer_bio(struct bio *bio)
{
	struct bvec_iter iter;
	struct bio_vec bvec;
	int dir = bio_data_dir(bio);
	sector_t pos = bio->bi_iter.bi_sector;

	bio_for_each_segment(bvec, bio, iter)
		pos += sbdd_xfer(&bvec, pos, dir);
}

static blk_qc_t sbdd_make_request(struct request_queue *q, struct bio *bio)
{
	if (atomic_read(&__sbdd.deleting)) {
		pr_err("unable to process bio while deleting\n");
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	atomic_inc(&__sbdd.refs_cnt);

	if (__sbdd.bd1 && bio_data_dir(bio)) {
		struct bio *bio_clone;
		pr_info("%s: cloning bio write\n", __func__);
		bio_clone = bio_clone_fast(bio, GFP_KERNEL|GFP_NOIO|GFP_NOFS, &q->bio_split);

		if (bio_clone) {
			bio_set_dev(bio_clone, __sbdd.bd1);
			submit_bio(bio_clone);
		} else {
			pr_err("FAILED to clone bio...\n");
			bio_io_error(bio);
		}
	}

	sbdd_xfer_bio(bio);
	bio_endio(bio);

	if (atomic_dec_and_test(&__sbdd.refs_cnt))
		wake_up(&__sbdd.exitwait);

	return BLK_STS_OK;
}

/*
There are no read or write operations. These operations are performed by
the request() function associated with the request queue of the disk.
*/
static struct block_device_operations const __sbdd_bdev_ops = {
	.owner = THIS_MODULE,
};

static int inline sbdd_attach_disk(const char* path, struct block_device **bd)
{
	int ret = 0;

	pr_info("attaching blkdev disk %s\n", path);
	*bd = blkdev_get_by_path(path, FMODE_READ|FMODE_WRITE|FMODE_EXCL|FMODE_NDELAY, &__sbdd);

	if (IS_ERR(*bd)) {
		ret = PTR_ERR(*bd);
		pr_warn("blkdev %s attach failed: %d\n", path, ret);
		*bd = NULL;
	} else {
		pr_info("blkdev attach complete");
	}

	return ret;
}

static int sbdd_create(void)
{
	int ret = 0;

	/*
	This call is somewhat redundant, but used anyways by tradition.
	The number is to be displayed in /proc/devices (0 for auto).
	*/
	pr_info("registering blkdev\n");
	__sbdd_major = register_blkdev(0, SBDD_NAME);
	if (__sbdd_major < 0) {
		pr_err("call register_blkdev() failed with %d\n", __sbdd_major);
		return -EBUSY;
	}

	memset(&__sbdd, 0, sizeof(struct sbdd));
	__sbdd.capacity = (sector_t)__sbdd_capacity_mib * SBDD_MIB_SECTORS;

	// attach 'physical' disk
	if (__sbdd_disk1) {
		sector_t bdcap;

		ret = sbdd_attach_disk(__sbdd_disk1, &__sbdd.bd1);
		if (ret)
			return ret;

		// limit capacity of ramdisk to capacity of physical
		bdcap = get_capacity(__sbdd.bd1->bd_disk);
		if (bdcap < __sbdd.capacity) {
			pr_info("old capacity=%llu, new capacity=%llu\n", __sbdd.capacity, bdcap);
			__sbdd.capacity = bdcap;
		}
	}

	pr_info("allocating data\n");
	__sbdd.data = vzalloc(__sbdd.capacity << SBDD_SECTOR_SHIFT);
	if (!__sbdd.data) {
		pr_err("unable to alloc data\n");
		return -ENOMEM;
	}

	spin_lock_init(&__sbdd.datalock);
	init_waitqueue_head(&__sbdd.exitwait);

	pr_info("allocating queue\n");
	__sbdd.q = blk_alloc_queue(GFP_KERNEL);
	if (!__sbdd.q) {
		pr_err("call blk_alloc_queue() failed\n");
		return -EINVAL;
	}
	blk_queue_make_request(__sbdd.q, sbdd_make_request);

	/* Configure queue */
	blk_queue_logical_block_size(__sbdd.q, SBDD_SECTOR_SIZE);

	/* A disk must have at least one minor */
	pr_info("allocating disk\n");
	__sbdd.gd = alloc_disk(1);

	/* Configure gendisk */
	__sbdd.gd->queue = __sbdd.q;
	__sbdd.gd->major = __sbdd_major;
	__sbdd.gd->first_minor = 0;
	__sbdd.gd->fops = &__sbdd_bdev_ops;
	/* Represents name in /proc/partitions and /sys/block */
	scnprintf(__sbdd.gd->disk_name, DISK_NAME_LEN, SBDD_NAME);
	set_capacity(__sbdd.gd, __sbdd.capacity);

	/*
	Allocating gd does not make it available, add_disk() required.
	After this call, gd methods can be called at any time. Should not be
	called before the driver is fully initialized and ready to process reqs.
	*/
	pr_info("adding disk\n");
	add_disk(__sbdd.gd);

	return ret;
}

static void sbdd_delete(void)
{
	atomic_set(&__sbdd.deleting, 1);

	wait_event(__sbdd.exitwait, !atomic_read(&__sbdd.refs_cnt));

	if (__sbdd.bd1) {
		pr_info("releasing phys disk 1");
		blkdev_put(__sbdd.bd1, FMODE_READ|FMODE_WRITE|FMODE_EXCL|FMODE_NDELAY);
		__sbdd.bd1 = NULL;
	}

	/* gd will be removed only after the last reference put */
	if (__sbdd.gd) {
		pr_info("deleting disk\n");
		del_gendisk(__sbdd.gd);
	}

	if (__sbdd.q) {
		pr_info("cleaning up queue\n");
		blk_cleanup_queue(__sbdd.q);
	}

	if (__sbdd.gd)
		put_disk(__sbdd.gd);

	if (__sbdd.data) {
		pr_info("freeing data\n");
		vfree(__sbdd.data);
	}

	memset(&__sbdd, 0, sizeof(struct sbdd));

	if (__sbdd_major > 0) {
		pr_info("unregistering blkdev\n");
		unregister_blkdev(__sbdd_major, SBDD_NAME);
		__sbdd_major = 0;
	}
}

/*
Note __init is for the kernel to drop this function after
initialization complete making its memory available for other uses.
There is also __initdata note, same but used for variables.
*/
static int __init sbdd_init(void)
{
	int ret = 0;

	pr_info("starting initialization...\n");
	ret = sbdd_create();

	if (ret) {
		pr_warn("initialization failed\n");
		sbdd_delete();
	} else {
		pr_info("initialization complete\n");
	}

	return ret;
}

/*
Note __exit is for the compiler to place this code in a special ELF section.
Sometimes such functions are simply discarded (e.g. when module is built
directly into the kernel). There is also __exitdata note.
*/
static void __exit sbdd_exit(void)
{
	pr_info("exiting...\n");
	if (__sbdd.bd1) {
		blkdev_put(__sbdd.bd1, FMODE_READ|FMODE_WRITE|FMODE_EXCL|FMODE_NDELAY);
		__sbdd.bd1 = NULL;
		pr_info("detached blkdev 1\n");
	}

	sbdd_delete();
	pr_info("exiting complete\n");
}

/* Called on module loading. Is mandatory. */
module_init(sbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(sbdd_exit);

/* Set desired capacity with insmod */
module_param_named(capacity_mib, __sbdd_capacity_mib, ulong, S_IRUGO);
module_param_named(disk1, __sbdd_disk1, charp, S_IRUGO);

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
