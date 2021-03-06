///////////////////////////////////////////////////////////////////////////////
///
/// @file prl_freeze.c
///
/// kernel module for suspend or resume
///
/// Copyright (c) 1999-2016 Parallels International GmbH.
/// All rights reserved.
/// http://www.parallels.com
///
///////////////////////////////////////////////////////////////////////////////


#include <linux/version.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include "prlfs_freeze_compat.h"

/**
 * /proc/driver/prl_freeze
 *
 * command protocol:
 *
 * "<command>[\n<command>]..."
 * "<fullpath>	freeze	(mountpoint path with leading '/')
 * "+<path>"	freeze
 * "-<path>"	thaw
 * "t<seconds>" arm thaw timeout timer
 * "#"		thaw all and stop timeout timer
 *
 * examples:
 *   # echo 't15	> /proc/driver/prl_freeze
 *   # echo '/mnt'	> /proc/driver/prl_freeze
 *   # echo '/'		> /proc/driver/prl_freeze
 *   arm thaw timeout to 15 seconds, freeze / and /mnt
 *
 *   # cat /proc/driver/prl_freeze
 *   shows names of forzen block devices
 *
 * submounts must be frozen _before_ parent mount.
 *
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 39)

struct inode *lookup_target(char *pathname)
{
	struct path path;
	struct inode *inode;
	int err;

	err = kern_path(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return ERR_PTR(err);
	inode = path.dentry->d_inode;
	if (inode)
		inode = igrab(inode);
	path_put(&path);
	return inode;
}

#else

struct inode *lookup_target(char *path)
{
	struct nameidata nd;
	struct inode *inode;
	int err;

	err = path_lookup(path, LOOKUP_FOLLOW, &nd);
	if (err)
		return ERR_PTR(err);
	inode = nd.path.dentry->d_inode;
	if (inode)
		inode = igrab(inode);
	path_put(&nd.path);
	return inode;
}

#endif

struct frozen_sb {
	struct list_head list;
	struct super_block *sb;
};

LIST_HEAD(frozen_sb);

# include <linux/mutex.h>
DEFINE_MUTEX(frozen_mutex);

int freeze_sb(struct super_block *sb)
{
	struct frozen_sb *fsb;
	int ret;

	if (!sb)
		return -EINVAL;

	list_for_each_entry(fsb, &frozen_sb, list)
		if (fsb->sb == sb)
			return -EEXIST;

	fsb = kmalloc(sizeof(struct frozen_sb), GFP_KERNEL);
	if (!fsb)
		return -ENOMEM;

	ret = prl_freeze_bdev(sb->s_bdev);
	if (ret) {
		kfree(fsb);
		return ret;
	}

	fsb->sb = sb;
	list_add_tail(&fsb->list, &frozen_sb);
	return 0;
}

int thaw_sb(struct super_block *sb)
{
	struct frozen_sb *fsb;

	list_for_each_entry(fsb, &frozen_sb, list) {
		if (fsb->sb != sb)
			continue;
		prl_thaw_bdev(fsb->sb->s_bdev, fsb->sb);
		list_del(&fsb->list);
		kfree(fsb);
		return 0;
	}
	return -ENOENT;
}

int process_path(char *path, int freeze)
{
	struct inode *inode;
	int ret;

	inode = lookup_target(path);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	mutex_lock(&frozen_mutex);
	if (freeze)
		ret = freeze_sb(inode->i_sb);
	else
		ret = thaw_sb(inode->i_sb);
	mutex_unlock(&frozen_mutex);

	iput(inode);

	return ret;
}

int thaw_all(void)
{
	struct frozen_sb *fsb, *tmp;

	mutex_lock(&frozen_mutex);
	list_for_each_entry_safe(fsb, tmp, &frozen_sb, list) {
		prl_thaw_bdev(fsb->sb->s_bdev, fsb->sb);
		kfree(fsb);
	}
	INIT_LIST_HEAD(&frozen_sb);
	mutex_unlock(&frozen_mutex);

	return 0;
}

void thaw_timeout(struct work_struct *work)
{
	thaw_all();
}
DECLARE_WORK(thaw_work, thaw_timeout);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)

void thaw_timer_fn(struct timer_list *unused)
{
	schedule_work(&thaw_work);
}
DEFINE_TIMER(thaw_timer, thaw_timer_fn);

#else

void thaw_timer_fn(unsigned long unused)
{
	schedule_work(&thaw_work);
}
DEFINE_TIMER(thaw_timer, thaw_timer_fn, 0, 0);

#endif

bool schedule_thaw_work(struct work_struct *work, unsigned long timeout)
{
	if (timer_pending(&thaw_timer))
		return false;

	mod_timer(&thaw_timer, jiffies + HZ * timeout);
	return true;
}

void cancel_timeout(void)
{
	del_timer_sync(&thaw_timer);
	flush_scheduled_work();
}

int arm_timeout(char *arg)
{
	unsigned long timeout;
	char *p;
	int ret;

	timeout = simple_strtoul(arg, &p, 10);
	if (!p || *p)
		return -EINVAL;

	mutex_lock(&frozen_mutex);
	ret = schedule_thaw_work(&thaw_work, timeout) ? 0 : -EBUSY;
	mutex_unlock(&frozen_mutex);

	return ret;
}

ssize_t freeze_write(struct file *file, const char __user *userbuf,
		size_t count, loff_t *ppos)
{
	char *buf, *ptr, *sep;
	int ret;

	if (count >= PATH_MAX)
		return -ENAMETOOLONG;

	buf = kmalloc(count+1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, userbuf, count)) {
		ret = -EFAULT;
		goto out;
	}
	buf[count] = 0;

	ptr = buf;
	do {
		sep = strchr(ptr, '\n');
		if (sep)
			*sep = 0;

		switch (ptr[0]) {
			case '/':
				ret = process_path(ptr, 1);
				break;
			case '+':
				ret = process_path(ptr+1, 1);
				break;
			case '-':
				ret = process_path(ptr+1, 0);
				break;
			case '*':
				/* freeze_all() not implemented yet */
				ret = -ENOSYS;
				break;
			case '#':
				ret = thaw_all();
				cancel_timeout();
				break;
			case 't':
				ret = arm_timeout(ptr+1);
				break;
			case '\0':
				ret = 0;
				break;
			default:
				ret = -EINVAL;
				break;
		}

		ptr = sep+1;
	} while (sep && !ret);

out:
	kfree(buf);
	if (!ret) {
		ret = count;
		*ppos += count;
	}
	return ret;
}

static void *seq_start(struct seq_file *file, loff_t *pos)
{
	loff_t off = *pos;
	struct list_head *lh;

	mutex_lock(&frozen_mutex);
	list_for_each(lh, &frozen_sb)
		if (!off--)
			return lh;
	return NULL;
}

static void *seq_next(struct seq_file *file, void *data, loff_t *pos)
{
	struct list_head *lh = data;

	++*pos;
	return (lh->next == &frozen_sb) ? NULL : lh->next;
}

static void seq_stop(struct seq_file *file, void *data)
{
	mutex_unlock(&frozen_mutex);
}

int seq_show(struct seq_file *file, void *data)
{
	struct frozen_sb *fsb;
	char buf[BDEVNAME_SIZE];

	fsb = list_entry((struct list_head*)data, struct frozen_sb, list);
	bdevname(fsb->sb->s_bdev, buf);
	seq_printf(file, "%s\n", buf);
	return 0;
}

static struct seq_operations freeze_seq_ops = {
	.start	= seq_start,
	.next	= seq_next,
	.stop	= seq_stop,
	.show	= seq_show,
};

int freeze_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &freeze_seq_ops);
}

static struct proc_ops freeze_ops = PRLFS_FREEZE_PROC_OPS_INIT(
		THIS_MODULE,
		freeze_open,
		seq_read,
		freeze_write,
		seq_lseek,
		seq_release);

int __init init_module(void)
{
	struct proc_dir_entry *entry;

	entry = prlfs_freeze_proc_create("driver/prl_freeze", S_IFREG | 0664, NULL,
		&freeze_ops);
	if (!entry)
		return -ENOMEM;
	return 0;
}

void __exit cleanup_module(void)
{
	remove_proc_entry("driver/prl_freeze", NULL);
	thaw_all();
	cancel_timeout();
}

MODULE_AUTHOR ("Parallels International GmbH");
MODULE_DESCRIPTION ("Parallels suspend/resume helper");
MODULE_LICENSE("Parallels");
MODULE_INFO(supported, "external");
