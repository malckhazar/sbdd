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
	int			stripe;	// in sectors
	struct gendisk          *gd;
	struct request_queue    *q;

	struct block_device	**bd;
	int			disks;
};

static struct sbdd      __sbdd;
static int              __sbdd_major = 0;
static unsigned long    __sbdd_capacity_mib = 100;

enum {
	RAID_0 = 0,
	RAID_1 = 1,
};

static unsigned short	__sbdd_mode = RAID_1;
static int		__sbdd_stripe = PAGE_SIZE;

static char*		__sbdd_disks = NULL;

static blk_qc_t sbdd_make_request(struct request_queue *q, struct bio *bio)
{
	struct bio *bio2 = NULL;
	if (atomic_read(&__sbdd.deleting)) {
		pr_err("unable to process bio while deleting\n");
		bio_io_error(bio);
		return BLK_STS_IOERR;
	}

	atomic_inc(&__sbdd.refs_cnt);

	switch(__sbdd_mode) {
	case RAID_0:
	{
		unsigned drive = (unsigned)(bio->bi_iter.bi_sector % __sbdd.disks);
		sector_t pos = bio->bi_iter.bi_sector / __sbdd.disks;

		if (bio->bi_iter.bi_sector > __sbdd.capacity) {
			pr_err("sector=%llu > disk_capacity=%llu \n", bio->bi_iter.bi_sector, __sbdd.capacity);
			bio_io_error(bio);
			return BLK_STS_IOERR;
		}

		while (bio2 != bio) {
			bio2 = bio_next_split(bio, __sbdd.stripe, GFP_NOIO, &q->bio_split);
			if (!bio2) {
				pr_err("bio_next_split: unable to allocate memory\n");
				bio_io_error(bio);
				return BLK_STS_IOERR;
			}
			
			bio_set_dev(bio2, __sbdd.bd[drive]);
			bio2->bi_iter.bi_sector = pos;

			pr_info("split bio to stripes, n=%u pos=%llu bio2.sector=%llu bio.sector=%llu\n",
					drive, pos, bio2->bi_iter.bi_sector, bio->bi_iter.bi_sector);

			if (bio2 != bio)
				bio_chain(bio2, bio);
			submit_bio(bio2);

			drive = (drive + 1) % __sbdd.disks;
			if (drive == 0)
				pos += __sbdd.stripe;
		}
		break;
	}
	case RAID_1:
		bio2 = bio_clone_fast(bio, GFP_NOIO, &q->bio_split);

		bio_set_dev(bio2, __sbdd.bd[1]);
		bio_chain(bio2, bio);
		submit_bio(bio2);

		bio_set_dev(bio, __sbdd.bd[0]);
		submit_bio(bio);

		break;

	default:
		pr_err("unknown RAID mode %d\n", __sbdd_mode);
		bio_io_error(bio);
	}

	pr_info("bio done =====================\n");
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
	char* d, *p;
	int n;
	sector_t capacity;

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

	pr_info("argument disks=%s\n", __sbdd_disks);

	// parse disks, count them and alloc array of pointers
	for (p = __sbdd_disks; *p; p++) {
		if (*p == ',')
			__sbdd.disks++;
	}
	__sbdd.disks++;

	pr_info("Found %d disks\n", __sbdd.disks);
	if ((__sbdd_mode == RAID_1) && (__sbdd.disks != 2))
	{
		pr_err("RAID-1 mode supports only 2 disks!\n");
		return -EINVAL;
	}

	__sbdd.bd = vzalloc(sizeof(struct block_device*) * __sbdd.disks);

	d = __sbdd_disks;
	n = 0;
	for (p = __sbdd_disks; *p; p++) {
		if (*p == ',') {
			*p = '\0';
			pr_info("attaching disk %s\n", d);
			ret = sbdd_attach_disk(d, &__sbdd.bd[n]);
			if (ret) {
				vfree(__sbdd.bd);
				return ret;
			}
			*p = ',';
			d = ++p;

			capacity = get_capacity(__sbdd.bd[n]->bd_disk);
			if ((__sbdd.capacity > capacity) || (!__sbdd.capacity))
				__sbdd.capacity = capacity;

			n++;
		}
	}

	if (d) {
		ret = sbdd_attach_disk(d, &__sbdd.bd[n]);
		if (ret) {
			vfree(__sbdd.bd);
			return ret;
		}

		capacity = get_capacity(__sbdd.bd[n]->bd_disk);
		if ((__sbdd.capacity > capacity) || (!__sbdd.capacity))
			__sbdd.capacity = capacity;
	}

	if (__sbdd_mode == RAID_0)
		__sbdd.capacity *= __sbdd.disks;

	__sbdd.stripe = __sbdd_stripe >> SECTOR_SHIFT;

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
	int i;
	atomic_set(&__sbdd.deleting, 1);

	wait_event(__sbdd.exitwait, !atomic_read(&__sbdd.refs_cnt));

	/* gd will be removed only after the last reference put */
	if (__sbdd.gd) {
		pr_info("deleting disk\n");
		del_gendisk(__sbdd.gd);
	}

	if(__sbdd.bd) {
		for(i = 0; i < __sbdd.disks; i++) {
			if (__sbdd.bd[i]) {
				pr_info("releasing phys disk %i\n", i);
				blkdev_put(__sbdd.bd[i], FMODE_READ|FMODE_WRITE|FMODE_EXCL|FMODE_NDELAY);
				__sbdd.bd[i] = NULL;
			}
		}
		vfree(__sbdd.bd);
	}

	if (__sbdd.q) {
		pr_info("cleaning up queue\n");
		blk_cleanup_queue(__sbdd.q);
	}

	if (__sbdd.gd)
		put_disk(__sbdd.gd);

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

	if (__sbdd_mode > 1) {
		pr_err("supported RAID modes are 0 and 1\n");
		return -EINVAL;
	}

	if (!__sbdd_disks) {
		pr_err("disks must be specified!\n");
		return -EINVAL;
	}

	if ((__sbdd_stripe < 0) || (__sbdd_stripe & (SECTOR_SIZE - 1))) {
		pr_err("stripe should be positive and dividable by %d\n", SECTOR_SIZE);
		return -EINVAL;
	}

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
	int i;
	pr_info("exiting...\n");
	for (i = 0; i < __sbdd.disks; i++) {
		if (__sbdd.bd[i]) {
			blkdev_put(__sbdd.bd[i], FMODE_READ|FMODE_WRITE|FMODE_EXCL|FMODE_NDELAY);
			__sbdd.bd[i] = NULL;
			pr_info("detached blkdev 1\n");
		}
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
module_param_named(mode, __sbdd_mode, ushort, S_IRUGO);
module_param_named(disks, __sbdd_disks, charp, S_IRUGO);
module_param_named(stripe, __sbdd_stripe, int, S_IRUGO);

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");


