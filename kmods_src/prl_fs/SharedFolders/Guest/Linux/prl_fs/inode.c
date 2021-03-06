/*
 *   prlfs/inode.c
 *
 *   Copyright (C) 1999-2016 Parallels International GmbH
 *   Author: Vasily Averin <vvs@parallels.com>
 *
 *   Parallels linux shared folders filesystem
 *
 *   Inode related functions
 */

#include <linux/module.h>
#include <linux/fs.h>
#include "prlfs.h"
#include <linux/ctype.h>
#include <linux/pagemap.h>
#include <linux/namei.h>
#include <linux/cred.h>
#include <linux/writeback.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 40)) && \
    (LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 0))
/* Fedora 15 uses 2.6.4x kernel version enumeration instead of 3.x */
#define __MINOR_3X_LINUX_VERSION LINUX_VERSION_CODE - KERNEL_VERSION(2, 6, 40)
#undef LINUX_VERSION_CODE
#define LINUX_VERSION_CODE KERNEL_VERSION(3, MINOR_3X_LINUX_VERSION, 0)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)
typedef umode_t prl_umode_t;
#else
typedef int prl_umode_t;
#endif

extern struct file_operations prlfs_file_fops;
extern struct file_operations prlfs_dir_fops;
extern struct inode *prlfs_iget(struct super_block *sb, ino_t ino);

static struct inode *prlfs_get_inode(struct super_block *sb, prl_umode_t mode);
struct dentry_operations prlfs_dentry_ops;

#define PRLFS_UID_NOBODY  65534
#define PRLFS_GID_NOGROUP 65534

static inline int prlfs_uid_valid(kuid_t uid)
{
	return !uid_eq(uid, prl_make_kuid(PRLFS_UID_NOBODY));
}

static inline int prlfs_gid_valid(kgid_t gid)
{
	return !gid_eq(gid, prl_make_kgid(PRLFS_GID_NOGROUP));
}

unsigned long *prlfs_dfl( struct dentry *de)
{
	return (unsigned long *)&(de->d_fsdata);
}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
#define prl_uaccess_kernel() false
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
#define prl_uaccess_kernel() uaccess_kernel()
#else
#define prl_uaccess_kernel() segment_eq(get_fs(), KERNEL_DS)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
#define prlfs_user_ns (init_task.cred->user_ns)
#define prlfs_setattr_copy(inode, attr) \
		setattr_copy(prlfs_user_ns, inode, attr)
#define prlfs_fillattr(inode, stat) \
		generic_fillattr(prlfs_user_ns, inode, stat)
#else
#define prlfs_setattr_copy(inode, attr) setattr_copy(inode, attr)
#define prlfs_fillattr(inode, stat) generic_fillattr(inode, stat)
#endif

void init_buffer_descriptor(struct buffer_descriptor *bd, void *buf,
			    unsigned long long len, int write, int user)
{
	bd->buf = buf;
	bd->len = len;
	bd->write = (write == 0) ? 0 : 1;
	bd->user = (user == 0) ? 0 : prl_uaccess_kernel() ? 0 : 1;
	bd->flags = TG_REQ_COMMON;
}

static int prepend(char **buffer, int *buflen, const char *str, int namelen)
{
	*buflen -= namelen;
	if (*buflen < 0)
		return -ENAMETOOLONG;
	*buffer -= namelen;
	memcpy(*buffer, str, namelen);
	return 0;
}

void *prlfs_get_path(struct dentry *dentry, void *buf, int *plen)
{
	int len;
	char *p;
	int ret;

	DPRINTK("ENTER\n");
	len = *plen;
	p = buf;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
	if ((dentry->d_name.len > NAME_MAX) || (len < 2)) {
		p = ERR_PTR(-ENAMETOOLONG);
		goto out;
	}
	p += --len;
	*p = '\0';
	spin_lock(&dcache_lock);
	while (!IS_ROOT(dentry)) {
		int nlen;
		struct dentry *parent;

                parent = dentry->d_parent;
		prefetch(parent);
		/* TODO Use prepend() here as well. */
		nlen = dentry->d_name.len;
		if (len < nlen + 1) {
			p = ERR_PTR(-ENAMETOOLONG);
			goto out_lock;
		}
		len -= nlen + 1;
		p -= nlen;
		memcpy(p, dentry->d_name.name, nlen);
		*(--p) = '/';
		dentry = parent;
	}
	if (*p != '/') {
		*(--p) = '/';
		--len;
	}
out_lock:
	spin_unlock(&dcache_lock);
	if (!IS_ERR(p))
		*plen -= len;
#else
	p = dentry_path_raw(dentry, p, len);
#endif
	ret = prepend(&p, &len,
		PRLFS_SB(dentry->d_sb)->name,
		strlen(PRLFS_SB(dentry->d_sb)->name));
	if (0 == ret)
		ret = prepend(&p, &len, "/", 1);
	if (0 == ret)
		*plen = strnlen(p, PAGE_SIZE-1) + 1;
	else
		p = ERR_PTR(ret);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38)
out:
#endif
	DPRINTK("EXIT returning %p\n", p);
	return p;
}

#define PRLFS_STD_INODE_HEAD(d)			\
	char *buf, *p;				\
	int buflen, ret;			\
	struct super_block *sb;			\
						\
	DPRINTK("ENTER\n");			\
	buflen = PATH_MAX;			\
	buf = kmalloc(buflen, GFP_KERNEL);	\
	if (buf == NULL) {			\
		ret = -ENOMEM;			\
		goto out;			\
	}					\
	memset(buf, 0, buflen);			\
	p = prlfs_get_path((d), buf, &buflen);	\
	if (IS_ERR(p)) {			\
		ret = PTR_ERR(p);		\
		goto out_free;			\
	}					\
	sb = (d)->d_sb;

#define PRLFS_STD_INODE_TAIL			\
out_free:					\
	kfree(buf);				\
out:						\
	DPRINTK("EXIT returning %d\n", ret);	\
	return ret;

static int prlfs_inode_open(struct dentry *dentry, prl_umode_t mode)
{
	struct prlfs_file_info pfi;
	PRLFS_STD_INODE_HEAD(dentry)
	init_pfi(&pfi, NULL, mode, O_CREAT | O_RDWR);
	ret = host_request_open(sb, &pfi, p, buflen);
	PRLFS_STD_INODE_TAIL
}

static int prlfs_delete(struct dentry *dentry)
{
	PRLFS_STD_INODE_HEAD(dentry)
	ret = host_request_remove(dentry->d_sb, p, buflen);
	PRLFS_STD_INODE_TAIL
}

static int do_prlfs_getattr(struct dentry *dentry, struct prlfs_attr *attr)
{
	struct buffer_descriptor bd;
	PRLFS_STD_INODE_HEAD(dentry)
	init_buffer_descriptor(&bd, attr, PATTR_STRUCT_SIZE, 1, 0);
	ret = host_request_attr(sb, p, buflen, &bd);
	PRLFS_STD_INODE_TAIL
}

#define SET_INODE_TIME(t, time)	do { (t).tv_sec = (time); } while (0)

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)
#define SET_INODE_INO(inode, ino) do { } while (0)
#else
#define SET_INODE_INO(inode, ino) do { (inode)->i_ino = ino; } while (0)
#endif

static void prlfs_change_attributes(struct inode *inode,
				    struct prlfs_attr *attr)
{
	struct prlfs_sb_info *sbi = PRLFS_SB(inode->i_sb);

	if (attr->valid & _PATTR_SIZE) {
		inode->i_blocks = ((attr->size + PAGE_SIZE - 1) / PAGE_SIZE) * 8;
		i_size_write(inode, attr->size);
	}
	if (attr->valid & _PATTR_ATIME)
		SET_INODE_TIME(inode->i_atime, attr->atime);
	if (attr->valid & _PATTR_MTIME)
		SET_INODE_TIME(inode->i_mtime, attr->mtime);
	if (attr->valid & _PATTR_CTIME)
		SET_INODE_TIME(inode->i_ctime, attr->ctime);
	if (attr->valid & _PATTR_MODE)
		inode->i_mode = (inode->i_mode & S_IFMT) | (attr->mode & 07777);
	if (attr->valid & _PATTR_UID) {
		if (attr->uid == -1)
			inode->i_uid = prl_make_kuid(PRLFS_UID_NOBODY);
		else if (sbi->plain)
			inode->i_uid = prl_make_kuid(attr->uid);
	}
	if (attr->valid & _PATTR_GID) {
		if (attr->gid == -1)
			inode->i_gid = prl_make_kgid(PRLFS_GID_NOGROUP);
		else if (sbi->plain)
			inode->i_gid = prl_make_kgid(attr->gid);
	}
	if (sbi->host_inodes && (attr->valid & _PATTR2_INO)) {
		SET_INODE_INO(inode, attr->ino);
	}
	return;
}

static int attr_to_pattr(struct iattr *attr, struct prlfs_attr *pattr)
{
	int ret;

	DPRINTK("ENTER\n");
	ret = 0;
	DPRINTK("ia_valid %x\n", attr->ia_valid);
	memset(pattr, 0, sizeof(struct prlfs_attr));
	if (attr->ia_valid & ATTR_SIZE) {
		pattr->size = attr->ia_size;
		pattr->valid |= _PATTR_SIZE;
	}
	if ((attr->ia_valid & (ATTR_ATIME | ATTR_MTIME)) ==
					(ATTR_ATIME | ATTR_MTIME)) {
		pattr->atime = attr->ia_atime.tv_sec;
		pattr->mtime = attr->ia_mtime.tv_sec;
		pattr->valid |= _PATTR_ATIME | _PATTR_MTIME;
	}
	if (attr->ia_valid & ATTR_CTIME) {
		pattr->ctime = attr->ia_ctime.tv_sec;
		pattr->valid |= _PATTR_CTIME;
	}
	if (attr->ia_valid & ATTR_MODE) {
		pattr->mode = (attr->ia_mode & 07777);
		pattr->valid |= _PATTR_MODE;
	}
	if (attr->ia_valid & ATTR_UID) {
		pattr->uid = prl_from_kuid(attr->ia_uid);
		pattr->valid = _PATTR_UID;
	}
	if (attr->ia_valid & ATTR_GID) {
		pattr->gid = prl_from_kgid(attr->ia_gid);
		pattr->valid = _PATTR_GID;
	}
	DPRINTK("EXIT returning %d\n", ret);
	return ret;
}

static int prlfs_mknod(struct inode *dir, struct dentry *dentry,
                       prl_umode_t mode)
{
	struct inode * inode;
	int ret;

	DPRINTK("ENTER\n");
	ret = 0;
	dentry->d_time = 0;
	inode = prlfs_get_inode(dir->i_sb, mode);
	if (inode)
		d_instantiate(dentry, inode);
	else
		ret = -ENOSPC;
	DPRINTK("EXIT returning %d\n", ret);
        return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int prlfs_create(struct user_namespace *mnt_userns,
                        struct inode *dir, struct dentry *dentry,
                        prl_umode_t mode, bool excl)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
static int prlfs_create(struct inode *dir, struct dentry *dentry,
                        prl_umode_t mode, bool excl)
#else
static int prlfs_create(struct inode *dir, struct dentry *dentry,
                        prl_umode_t mode, struct nameidata *nd)
#endif
{
	int ret;

	DPRINTK("ENTER\n");
	ret = prlfs_inode_open(dentry, mode | S_IFREG);
	if (ret == 0)
		ret = prlfs_mknod(dir, dentry, mode | S_IFREG);
	DPRINTK("EXIT returning %d\n", ret);
        return ret;
}

static struct dentry *prlfs_lookup(struct inode *dir, struct dentry *dentry
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,6,0)
			, unsigned int flags
#else
			, struct nameidata *nd
#endif
	)
{
	int ret;
	struct prlfs_attr *attr = 0;
	struct inode *inode;

	DPRINTK("ENTER\n");
	DPRINTK("dir ino %lld entry name \"%s\"\n",
		 (u64)dir->i_ino, dentry->d_name.name);
	attr = kmalloc(sizeof(struct prlfs_attr), GFP_KERNEL);
	if (!attr) {
		ret = -ENOMEM;
		goto out;
	}
	ret = do_prlfs_getattr(dentry, attr);
	if (ret < 0 ) {
		if (ret == -ENOENT) {
			inode = NULL;
			ret = 0;
		} else
			goto out_free;
	} else {
		inode = prlfs_get_inode(dentry->d_sb, attr->mode);
		if (inode)
			prlfs_change_attributes(inode, attr);
	}
	dentry->d_time = jiffies;
	d_add(dentry, inode);
	d_set_d_op(dentry, &prlfs_dentry_ops);
out_free:
	kfree(attr);
out:
	DPRINTK("EXIT returning %d\n", ret);
	return ERR_PTR(ret);
}

static int prlfs_unlink(struct inode *dir, struct dentry *dentry)
{
        int ret;
	unsigned long *dfl = prlfs_dfl(dentry);

	DPRINTK("ENTER\n");
	ret = prlfs_delete(dentry);
	if (!ret)
		 *dfl |= PRL_DFL_UNLINKED;
	DPRINTK("EXIT returning %d\n", ret);
        return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int prlfs_mkdir(struct user_namespace *mnt_userns, struct inode *dir,
                       struct dentry *dentry, prl_umode_t mode)
#else
static int prlfs_mkdir(struct inode *dir, struct dentry *dentry,
                       prl_umode_t mode)
#endif
{
	int ret;

	DPRINTK("ENTER\n");
	ret = prlfs_inode_open(dentry, mode | S_IFDIR);
	if (ret == 0)
		ret = prlfs_mknod(dir, dentry, mode | S_IFDIR);
	DPRINTK("EXIT returning %d\n", ret);
	return ret;
}

static int prlfs_rmdir(struct inode *dir, struct dentry *dentry)
{
        int ret;
	unsigned long *dfl = prlfs_dfl(dentry);

	DPRINTK("ENTER\n");
	ret = prlfs_delete(dentry);
	if (!ret)
		*dfl |= PRL_DFL_UNLINKED;
	DPRINTK("EXIT returning %d\n", ret);
        return ret;
}

static int __prlfs_rename(struct inode *old_dir, struct dentry *old_de,
                          struct inode *new_dir, struct dentry *new_de)
{
	void *np, *nbuf;
	int nbuflen;
	PRLFS_STD_INODE_HEAD(old_de)
	nbuflen = PATH_MAX;
	nbuf = kmalloc(nbuflen, GFP_KERNEL);
	if (nbuf == NULL) {
		ret = -ENOMEM;
		goto out_free;
	}
	memset(nbuf, 0, nbuflen);
	np = prlfs_get_path(new_de, nbuf, &nbuflen);
	if (IS_ERR(np)) {
		ret = PTR_ERR(np);
		goto out_free_nbuf;
	}
	ret = host_request_rename(sb, p, buflen, np, nbuflen);
	old_de->d_time = 0;
	new_de->d_time = 0;
out_free_nbuf:
	kfree(nbuf);
	PRLFS_STD_INODE_TAIL
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int prlfs_rename(struct user_namespace *mnt_userns,
                        struct inode *old_dir, struct dentry *old_de,
                        struct inode *new_dir, struct dentry *new_de,
                        unsigned int flags)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
static int prlfs_rename(struct inode *old_dir, struct dentry *old_de,
                        struct inode *new_dir, struct dentry *new_de,
                        unsigned int flags)
#else
static int prlfs_rename(struct inode *old_dir, struct dentry *old_de,
                        struct inode *new_dir, struct dentry *new_de)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	if (flags)
		return -EINVAL;
#endif
	return __prlfs_rename(old_dir, old_de, new_dir, new_de);
}

/*
 * FIXME: Move fs specific data to inode.
 * Current implementation used full path to as a reference to opened file.
 * So {set,get}attr result access to another not unlinked file with the same
 * path.
 */
static int check_dentry(struct dentry *dentry)
{
	return *prlfs_dfl(dentry) & PRL_DFL_UNLINKED;
}

static int prlfs_inode_setattr(struct inode *inode, struct iattr *attr)
{
	int ret = 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
	ret = inode_setattr(inode, attr);
#else
	if ((attr->ia_valid & ATTR_SIZE &&
			attr->ia_size != i_size_read(inode))) {
		ret = inode_newsize_ok(inode, attr->ia_size);
		if (ret)
			goto out;
		truncate_setsize(inode, attr->ia_size);
	}
	prlfs_setattr_copy(inode, attr);
	mark_inode_dirty(inode);
out:
#endif
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int prlfs_setattr(struct user_namespace *mnt_userns,
                         struct dentry *dentry, struct iattr *attr)
#else
static int prlfs_setattr(struct dentry *dentry, struct iattr *attr)
#endif
{
	struct prlfs_attr *pattr;
	struct buffer_descriptor bd;
	PRLFS_STD_INODE_HEAD(dentry)
	pattr = kmalloc(sizeof(struct prlfs_attr), GFP_KERNEL);
	if (!pattr) {
		ret = -ENOMEM;
		goto out_free;
	}
	ret = attr_to_pattr(attr, pattr);
	if (ret < 0)
		goto out_free_pattr;

	if (check_dentry(dentry)) {
		ret = - ESTALE;
		goto out_free_pattr;
	}
	init_buffer_descriptor(&bd, pattr, PATTR_STRUCT_SIZE, 0, 0);
	ret = host_request_attr(sb, p, buflen, &bd);
	if (ret == 0)
		ret = prlfs_inode_setattr(dentry->d_inode, attr);
	dentry->d_time = 0;
out_free_pattr:
	kfree(pattr);
	PRLFS_STD_INODE_TAIL
}

static int prlfs_i_revalidate(struct dentry *dentry)
{
	struct prlfs_attr *attr = 0;
	struct inode *inode;
	int ret;

	DPRINTK("ENTER\n");
	if (!dentry || !dentry->d_inode) {
		ret = -ENOENT;
		goto out;
	}
	if (dentry->d_time != 0 &&
	    jiffies - dentry->d_time < PRLFS_SB(dentry->d_sb)->ttl) {
		ret = 0;
		goto out;
	}
	attr = kmalloc(sizeof(struct prlfs_attr), GFP_KERNEL);
	if (!attr) {
		ret = -ENOMEM;
		goto out;
	}
	inode = dentry->d_inode;
	ret = do_prlfs_getattr(dentry, attr);
	if (ret < 0)
		goto out_free;

	if ((inode->i_mode ^ attr->mode) & S_IFMT) {
		DPRINTK("inode <%p> i_mode %x attr->mode %x\n", inode, inode->i_mode,
				attr->mode);
		make_bad_inode(inode);
		ret = -EIO;
	} else {
		prlfs_change_attributes(inode, attr);
	}
	dentry->d_time = jiffies;
out_free:
	kfree(attr);
out:
	DPRINTK("EXIT returning %d\n", ret);
	return ret;
}

static int prlfs_d_revalidate(struct dentry *dentry,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,6,0)
					struct nameidata *nd
#else
					unsigned int flags
#endif
	)
{
	int ret;

	DPRINTK("ENTER\n");
	ret = (prlfs_i_revalidate(dentry) == 0) ? 1 : 0;
	DPRINTK("EXIT returning %d\n", ret);
	return ret;
}

struct dentry_operations prlfs_dentry_ops = {
	.d_revalidate = prlfs_d_revalidate,
};


inline int __prlfs_getattr(struct dentry *dentry, struct kstat *stat)
{
	int ret;
	DPRINTK("ENTER\n");
	if (check_dentry(dentry)) {
		ret = - ESTALE;
		goto out;
	}

	ret = prlfs_i_revalidate(dentry);
	if (ret < 0)
		goto out;

	prlfs_fillattr(dentry->d_inode, stat);
	if (PRLFS_SB(dentry->d_sb)->share) {
		if (prlfs_uid_valid(stat->uid))
			stat->uid = current->cred->fsuid;
		if (prlfs_gid_valid(stat->gid))
			stat->gid = current->cred->fsgid;
	}
out:
	DPRINTK("EXIT returning %d\n", ret);
	return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int prlfs_getattr(struct user_namespace *mnt_userns,
                         const struct path *path, struct kstat *stat,
                         u32 request_mask, unsigned int query_flags)
{
	return __prlfs_getattr(path->dentry, stat);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
static int prlfs_getattr(const struct path *path, struct kstat *stat,
		u32 request_mask, unsigned int query_flags)
{
	return __prlfs_getattr(path->dentry, stat);
}
#else
static int prlfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
		 struct kstat *stat)
{
	return __prlfs_getattr(dentry, stat);
}
#endif

static int __prlfs_permission(struct inode *inode, int mask)
{
	int isdir;
	prl_umode_t mode;

	DPRINTK("ENTER\n");

	mode = inode->i_mode;
	isdir = S_ISDIR(mode);

	if (prlfs_uid_valid(inode->i_uid))
		mode = mode >> 6;
	else if (prlfs_gid_valid(inode->i_gid))
		mode = mode >> 3;
	mode &= 0007;
	mask &= MAY_READ | MAY_WRITE | MAY_EXEC;

	DPRINTK("mask 0x%x mode %o\n", mask, mode);

	if ((mask & ~mode) == 0)
		return 0;

	if (!(mask & MAY_EXEC) || (isdir || (mode & S_IXUGO)))
		if (capable(CAP_DAC_OVERRIDE))
			return 0;

	 if (mask == MAY_READ || (isdir && !(mask & MAY_WRITE)))
		if (capable(CAP_DAC_READ_SEARCH))
			return 0;

	DPRINTK("EXIT returning EACCES\n");
	return -EACCES;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int prlfs_permission(struct user_namespace *mnt_userns,
                            struct inode *inode, int mask)
{
	struct prlfs_sb_info *sbi = PRLFS_SB(inode->i_sb);

	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;
	if (!sbi->share)
		return generic_permission(prlfs_user_ns, inode, mask);

	return __prlfs_permission(inode, mask);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 1, 0)
static int prlfs_permission(struct inode *inode, int mask)
{
	struct prlfs_sb_info *sbi = PRLFS_SB(inode->i_sb);

	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;
	if (!sbi->share)
		return generic_permission(inode, mask);

	return __prlfs_permission(inode, mask);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 38)
static int prlfs_permission(struct inode *inode, int mask, unsigned int flags)
{
	struct prlfs_sb_info *sbi = PRLFS_SB(inode->i_sb);

	if (flags & IPERM_FLAG_RCU)
		return -ECHILD;
	if (!sbi->share)
		return generic_permission(inode, mask, flags, NULL);

	return __prlfs_permission(inode, mask);
}
#else
static int prlfs_permission(struct inode *inode, int mask)
{
	struct prlfs_sb_info *sbi = PRLFS_SB(inode->i_sb);

	if (!sbi->share)
		return generic_permission(inode, mask, NULL);

	return __prlfs_permission(inode, mask);
}
#endif

static char *do_read_symlink(struct dentry *dentry)
{
	char *buf, *src_path, *tgt_path;
	int src_len, tgt_len, ret;

	tgt_path = NULL;
	src_len = tgt_len = PATH_MAX;
	buf = kmalloc(src_len, GFP_KERNEL);
	if (buf == NULL) {
		tgt_path = ERR_PTR(-ENOMEM);
		goto out;
	}
	memset(buf, 0, src_len);
	src_path = prlfs_get_path(dentry, buf, &src_len);
	if (IS_ERR(src_path)) {
		tgt_path = src_path;
		goto out_free;
	}

	tgt_path = kmalloc(tgt_len, GFP_KERNEL);
	if (IS_ERR(tgt_path))
		goto out_free;
	DPRINTK("src '%s'\n", src_path);
	ret = host_request_readlink(dentry->d_sb, src_path, src_len, tgt_path, tgt_len);
	if (ret < 0) {
		kfree(tgt_path);
		tgt_path = ERR_PTR(ret);
	} else
		DPRINTK("tgt '%s'\n", tgt_path);
out_free:
	kfree(buf);
out:
	return tgt_path;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
static void prlfs_put_link(struct dentry *d, struct nameidata *nd, void *cookie)
{
	char *s = nd_get_link(nd);
	if (!IS_ERR(s))
		kfree(s);
}
#else // >= KERNEL_VERSION(3, 13, 0)
#define prlfs_put_link kfree_put_link
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
static const char *prlfs_get_link(struct dentry *dentry, struct inode *inode,
                                  struct delayed_call *dc)
{
	char *symlink;
	if (!dentry)
		return ERR_PTR(-ECHILD);
	symlink = do_read_symlink(dentry);
	set_delayed_call(dc, kfree_link, symlink);
	return symlink;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
static const char *prlfs_follow_link(struct dentry *dentry, void **cookie)
{
	char *symlink = do_read_symlink(dentry);
	*cookie = symlink;
	return symlink;
}
#else
static void *prlfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	char *symlink = do_read_symlink(dentry);
	nd_set_link(nd, symlink);
	return symlink;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
static int prlfs_symlink(struct user_namespace *mnt_userns,
                         struct inode *dir, struct dentry *dentry,
                         const char *symname)
#else
static int prlfs_symlink(struct inode *dir, struct dentry *dentry,
                         const char *symname)
#endif
{
	PRLFS_STD_INODE_HEAD(dentry)
	DPRINTK("ENTER symname = '%s'\n", symname);
	ret = host_request_symlink(sb, p, buflen, symname, strlen(symname) + 1);
	if (ret == 0)
		ret = prlfs_mknod(dir, dentry, S_IFLNK);
	PRLFS_STD_INODE_TAIL
	return ret;
}

struct inode_operations prlfs_file_iops = {
	.setattr	= prlfs_setattr,
	.permission	= prlfs_permission,
	.getattr	= prlfs_getattr,
};

struct inode_operations prlfs_dir_iops = {
	.create		= prlfs_create,
	.lookup		= prlfs_lookup,
	.unlink		= prlfs_unlink,
	.mkdir		= prlfs_mkdir,
	.rmdir		= prlfs_rmdir,
	.rename		= prlfs_rename,
	.setattr	= prlfs_setattr,
	.symlink	= prlfs_symlink,
	.permission	= prlfs_permission,
	.getattr	= prlfs_getattr,
};

struct inode_operations prlfs_symlink_iops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
	.readlink = generic_readlink,
	.setattr  = prlfs_setattr,
	.getattr  = prlfs_getattr,
	.follow_link = prlfs_follow_link,
	.put_link = prlfs_put_link,
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
	.readlink = generic_readlink,
	.setattr  = prlfs_setattr,
	.getattr  = prlfs_getattr,
	.get_link = prlfs_get_link,
#else
	.setattr  = prlfs_setattr,
	.getattr  = prlfs_getattr,
	.get_link = prlfs_get_link,
#endif
};

ssize_t prlfs_rw(struct inode *inode, char *buf, size_t size,
		                loff_t *off, unsigned int rw, int user, int flags);


int prlfs_readpage(struct file *file, struct page *page) {
	char *buf;
	ssize_t ret;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
	struct inode *inode = file->f_inode;
#else
	struct inode *inode = file->f_dentry->d_inode;
#endif
	loff_t off = page->index << PAGE_SHIFT;

	if (!file) {
		unlock_page(page);
		return -EINVAL;
	}

	if (!PageUptodate(page)) {
		buf = kmap(page);
		ret = prlfs_rw(inode, buf, PAGE_SIZE, &off, 0, 0, TG_REQ_PF_CTX);
		if (ret < 0) {
			kunmap(page);
			unlock_page(page);
			return -EIO;
		}
		if (ret < PAGE_SIZE)
			memset(buf + ret, 0, PAGE_SIZE - ret);
		kunmap(page);
		flush_dcache_page(page);
		SetPageUptodate(page);
	}
	unlock_page(page);
	return 0;
}

int prlfs_writepage(struct page *page, struct writeback_control *wbc) {
	struct inode *inode = page->mapping->host;
	loff_t i_size = inode->i_size;
	char *buf;
	ssize_t ret;
	int rc = 0;
	loff_t off = page->index << PAGE_SHIFT;
	loff_t w_remainder = i_size - off;

	DPRINTK("ENTER page=%p off=%lld\n", page, off);

	set_page_writeback(page);
	buf = kmap(page);
	ret = prlfs_rw(inode, buf,
		       w_remainder < PAGE_SIZE ? w_remainder : PAGE_SIZE,
		       &off, 1, 0, TG_REQ_COMMON);
	kunmap(page);
	if (ret < 0) {
		rc =  -EIO;
		SetPageError(page);
		mapping_set_error(page->mapping, rc);
	}

	end_page_writeback(page);
	unlock_page(page);
	DPRINTK("EXIT ret=%d\n", rc);
	return rc;
}

static int prlfs_write_end(struct file *file, struct address_space *mapping,
                           loff_t pos, unsigned int len, unsigned int copied,
                           struct page *page, void *fsdata)
{
	unsigned int from = pos & (PAGE_SIZE - 1);
	struct inode *inode = mapping->host;
	ssize_t ret;
	loff_t offset = pos;
	char *buf;

	DPRINTK("ENTER inode=%p pos=%lld len=%u copied=%u\n", inode, pos, len, copied);

	if (!PageUptodate(page) && copied < len)
		zero_user(page, from + copied, len - copied);

	buf = kmap(page);
	ret = prlfs_rw(inode, buf + from, copied, &offset, 1, 0, TG_REQ_COMMON);
	kunmap(page);

	if (ret < 0)
		goto out;

	if (!PageUptodate(page) && len == PAGE_SIZE)
		SetPageUptodate(page);

	if (pos + copied > inode->i_size)
		i_size_write(inode, pos + copied);

out:
	unlock_page(page);
	put_page(page);

	DPRINTK("EXIT ret=%ld\n", ret);
	return ret;
}

static const struct address_space_operations prlfs_aops = {
	.readpage		= prlfs_readpage,
	.writepage		= prlfs_writepage,
	.write_begin    = simple_write_begin,
	.write_end      = prlfs_write_end,
	#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
	.dirty_folio	= filemap_dirty_folio,
	#else
	.set_page_dirty = __set_page_dirty_nobuffers,
	#endif
};



static int prlfs_root_revalidate(struct dentry *dentry,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,6,0)
					struct nameidata *nd
#else
					unsigned int flags
#endif
	)
{
	return 1;
}

struct dentry_operations prlfs_root_dops = {
	.d_revalidate = prlfs_root_revalidate,
};

struct inode_operations prlfs_root_iops = {
	.lookup		= prlfs_lookup,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
#define prlfs_current_time(inode) current_time(inode)
#else
#define prlfs_current_time(inode) CURRENT_TIME
#endif

static struct inode *prlfs_get_inode(struct super_block *sb, prl_umode_t mode)
{
	struct inode * inode;
	struct prlfs_fd* pfd;

	DPRINTK("ENTER\n");
	inode = new_inode(sb);
	if (inode) {
		inode->i_mode = mode;
		inode->i_blocks = 0;
		inode->i_ctime = prlfs_current_time(inode);
		inode->i_atime = inode->i_mtime = inode->i_ctime;
		if (PRLFS_SB(sb)->share) {
			inode->i_uid = current->cred->uid;
			inode->i_gid = current->cred->gid;
		} else {
			inode->i_uid = PRLFS_SB(sb)->uid;
			inode->i_gid = PRLFS_SB(sb)->gid;
		}
		inode->i_mapping->a_ops = &prlfs_aops;

		pfd = kmalloc(sizeof(struct prlfs_fd), GFP_KERNEL);
		if (pfd != NULL)
			memset(pfd, 0, sizeof(struct prlfs_fd));
		else
			pfd = ERR_PTR(-ENOMEM);
		inode_set_pfd(inode, pfd);

		SET_INODE_INO(inode, get_next_ino());
		switch (mode & S_IFMT) {
		case S_IFDIR:
			inode->i_op = &prlfs_dir_iops;
			inode->i_fop = &prlfs_dir_fops;
			break;
		case 0: case S_IFREG:
			prlfs_hlist_init(inode);
			inode->i_op = &prlfs_file_iops;
			inode->i_fop =  &prlfs_file_fops;
			break;
		case S_IFLNK:
			inode->i_op = &prlfs_symlink_iops;
			inode->i_fop = &prlfs_file_fops;
			break;
		}
	}
	DPRINTK("EXIT returning %p\n", inode);
	return inode;
}

void prlfs_read_inode(struct inode *inode)
{
	ino_t ino = inode->i_ino;
	struct super_block *sb = inode->i_sb;
	struct prlfs_fd *pfd;

	inode->i_mode = S_IFDIR | S_IRUGO | S_IXUGO | S_IWUSR;
	inode->i_ctime = prlfs_current_time(inode);
	inode->i_atime = inode->i_mtime = inode->i_ctime;
	if (PRLFS_SB(sb)->share) {
		inode->i_uid = current->cred->uid;
		inode->i_gid = current->cred->gid;
	} else {
		inode->i_uid = PRLFS_SB(sb)->uid;
		inode->i_gid = PRLFS_SB(sb)->gid;
	}

	if (!inode_get_pfd(inode)) {
		pfd = kmalloc(sizeof(struct prlfs_fd), GFP_KERNEL);
		if (pfd != NULL)
			memset(pfd, 0, sizeof(struct prlfs_fd));
		else
			pfd = ERR_PTR(-ENOMEM);
		inode_set_pfd(inode, pfd);
	}

	if (ino == PRLFS_ROOT_INO) {
		inode->i_op = &prlfs_dir_iops;
		inode->i_fop = &prlfs_dir_fops;
	}
}
