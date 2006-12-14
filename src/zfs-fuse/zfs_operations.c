/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Ricardo Correia.
 * Use is subject to license terms.
 */

#include "fuse.h"

#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/debug.h>
#include <sys/vnode.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_znode.h>
#include <sys/mode.h>

#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "util.h"

#define ZFS_MAGIC 0x2f52f5

static const char *hello_str = "Hello World!\n";

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
                             off_t off, size_t maxsize)
{
    if (off < bufsize)
        return fuse_reply_buf(req, buf + off, min(bufsize - off, maxsize));
    else
        return fuse_reply_buf(req, NULL, 0);
}

static void hello_ll_open(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
	fprintf(stderr, "hello_ll_open: %li\n", (long) ino);

    if (ino != 2)
        fuse_reply_err(req, EISDIR);
    else if ((fi->flags & 3) != O_RDONLY)
        fuse_reply_err(req, EACCES);
    else
        fuse_reply_open(req, fi);
}

static void hello_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                         off_t off, struct fuse_file_info *fi)
{
	fprintf(stderr, "hello_ll_read: %li\n", (long) ino);

    (void) fi;

    assert(ino == 2);
    reply_buf_limited(req, hello_str, strlen(hello_str), off, size);
}

static void zfsfuse_destroy(void *userdata)
{
	vfs_t *vfs = (vfs_t *) userdata;

	VERIFY(do_umount(vfs) == 0);
}

static void zfsfuse_statfs(fuse_req_t req)
{
	vfs_t *vfs = (vfs_t *) fuse_req_userdata(req);

	struct statvfs64 zfs_stat;

	int ret = VFS_STATVFS(vfs, &zfs_stat);
	if(ret != 0) {
		fuse_reply_err(req, ret);
		return;
	}

	struct statvfs stat = { 0 };

	/* There's a bug somewhere in FUSE, in the kernel or in df(1) where
	   f_bsize is being used to calculate filesystem size instead of
	   f_frsize, so we must use that instead */
	stat.f_bsize = zfs_stat.f_frsize;
	stat.f_frsize = zfs_stat.f_frsize;
	stat.f_blocks = zfs_stat.f_blocks;
	stat.f_bfree = zfs_stat.f_bfree;
	stat.f_bavail = zfs_stat.f_bavail;
	stat.f_files = zfs_stat.f_files;
	stat.f_ffree = zfs_stat.f_ffree;
	stat.f_favail = zfs_stat.f_favail;
	stat.f_fsid = zfs_stat.f_fsid;
	stat.f_flag = zfs_stat.f_flag;
	stat.f_namemax = zfs_stat.f_namemax;

	int error = -fuse_reply_statfs(req, &stat);
	if(error)
		fuse_reply_err(req, error);
}

static int zfsfuse_stat(vnode_t *vp, struct stat *stbuf)
{
	ASSERT(vp != NULL);
	ASSERT(stbuf != NULL);

	vattr_t vattr;
	vattr.va_mask = AT_STAT | AT_NBLOCKS | AT_BLKSIZE | AT_SIZE;

	int error = VOP_GETATTR(vp, &vattr, 0, NULL);
	if(error)
		return error;

	memset(stbuf, 0, sizeof(struct stat));

	stbuf->st_dev = vattr.va_fsid;
	stbuf->st_ino = vattr.va_nodeid == 3 ? 1 : vattr.va_nodeid;
	stbuf->st_mode = VTTOIF(vattr.va_type) | vattr.va_mode;
	stbuf->st_nlink = vattr.va_nlink;
	stbuf->st_uid = vattr.va_uid;
	stbuf->st_gid = vattr.va_gid;
	stbuf->st_rdev = vattr.va_rdev;
	stbuf->st_size = vattr.va_size;
	stbuf->st_blksize = vattr.va_blksize;
	stbuf->st_blocks = vattr.va_nblocks;
	stbuf->st_atime = TIMESTRUC_TO_TIME(vattr.va_atime);
	stbuf->st_mtime = TIMESTRUC_TO_TIME(vattr.va_mtime);
	stbuf->st_ctime = TIMESTRUC_TO_TIME(vattr.va_ctime);

	return 0;
}

static int zfsfuse_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	vfs_t *vfs = (vfs_t *) fuse_req_userdata(req);
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *znode;

	int error = zfs_zget(zfsvfs, ino, &znode);
	if(error) {
		ZFS_EXIT(zfsvfs);
		return error;
	}

	ASSERT(znode != NULL);
	vnode_t *vp = ZTOV(znode);
	ASSERT(vp != NULL);

	struct stat stbuf;
	error = zfsfuse_stat(vp, &stbuf);

	VN_RELE(vp);
	ZFS_EXIT(zfsvfs);

	if(!error)
		error = -fuse_reply_attr(req, &stbuf, 0.0);

	return error;
}

static void zfsfuse_getattr_helper(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	fuse_ino_t real_ino = ino == 1 ? 3 : ino;

	int error = zfsfuse_getattr(req, real_ino, fi);
	if(error)
		fuse_reply_err(req, error);
}

static int zfsfuse_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	vfs_t *vfs = (vfs_t *) fuse_req_userdata(req);
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *znode;

	int error = zfs_zget(zfsvfs, parent, &znode);
	if(error) {
		ZFS_EXIT(zfsvfs);
		return error;
	}

	ASSERT(znode != NULL);
	vnode_t *dvp = ZTOV(znode);
	ASSERT(dvp != NULL);

	vnode_t *vp = NULL;

	error = VOP_LOOKUP(dvp, (char *) name, &vp, NULL, 0, NULL, NULL);
	if(error)
		goto out;

	struct fuse_entry_param e = { 0 };

	e.attr_timeout = 0.0;
	e.entry_timeout = 0.0;

	if(vp == NULL)
		goto out;

	e.ino = VTOZ(vp)->z_id;
	if(e.ino == 3)
		e.ino = 1;

	e.generation = VTOZ(vp)->z_phys->zp_gen;

	error = zfsfuse_stat(vp, &e.attr);

out:
	if(vp != NULL)
		VN_RELE(vp);
	VN_RELE(dvp);
	ZFS_EXIT(zfsvfs);

	if(!error)
		error = -fuse_reply_entry(req, &e);

	return error;
}

static void zfsfuse_lookup_helper(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	fuse_ino_t real_parent = parent == 1 ? 3 : parent;

	int error = zfsfuse_lookup(req, real_parent, name);
	if(error)
		fuse_reply_err(req, error);
}

static int zfsfuse_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	vfs_t *vfs = (vfs_t *) fuse_req_userdata(req);
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	znode_t *znode;

	int error = zfs_zget(zfsvfs, ino, &znode);
	if(error) {
		ZFS_EXIT(zfsvfs);
		return error;
	}

	ASSERT(znode != NULL);
	vnode_t *vp = ZTOV(znode);
	ASSERT(vp != NULL);

	if(vp->v_type != VDIR) {
		error = ENOTDIR;
		goto out;
	}

	vnode_t *old_vp = vp;

	/* XXX: not sure about flags */
	error = VOP_OPEN(&vp, FREAD | FWRITE, NULL);

	ASSERT(old_vp == vp);

	if(!error)
		fi->fh = (uint64_t) (uintptr_t) vp;

out:
	if(error)
		VN_RELE(vp);
	ZFS_EXIT(zfsvfs);

	if(!error)
		error = -fuse_reply_open(req, fi);

	return error;
}

static void zfsfuse_opendir_helper(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	fuse_ino_t real_ino = ino == 1 ? 3 : ino;

	int error = zfsfuse_opendir(req, real_ino, fi);
	if(error)
		fuse_reply_err(req, error);
}

static int zfsfuse_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	vfs_t *vfs = (vfs_t *) fuse_req_userdata(req);
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	ZFS_ENTER(zfsvfs);

	vnode_t *vp = (vnode_t *)(uintptr_t) fi->fh;
	ASSERT(vp != NULL);

	/* XXX: not sure about flags */
	int error = VOP_CLOSE(vp, FREAD | FWRITE, 1, (offset_t) 0, NULL);

	/* XXX: errors are ignored by FUSE, so we release it anyway? (??) */
	VN_RELE(vp);
	ZFS_EXIT(zfsvfs);

	return error;
}

static void zfsfuse_release_helper(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	fuse_ino_t real_ino = ino == 1 ? 3 : ino;

	int error = zfsfuse_release(req, real_ino, fi);
	/* Release events always reply_err */
	fuse_reply_err(req, error);
}

static int zfsfuse_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
	vnode_t *vp = (vnode_t *)(uintptr_t) fi->fh;
	ASSERT(vp != NULL);

	if(vp->v_type != VDIR)
		return ENOTDIR;

	vfs_t *vfs = (vfs_t *) fuse_req_userdata(req);
	zfsvfs_t *zfsvfs = vfs->vfs_data;

	char *outbuf = malloc(size);
	if(outbuf == NULL)
		return ENOMEM;

	ZFS_ENTER(zfsvfs);

	union {
		char buf[DIRENT64_RECLEN(MAXNAMELEN)];
		struct dirent64 dirent;
	} entry;

	struct stat fstat = { 0 };

	iovec_t iovec;
	uio_t uio;
	uio.uio_iov = &iovec;
	uio.uio_iovcnt = 1;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_fmode = 0;

	int eofp = 0;

	int outbuf_off = 0;
	int outbuf_resid = size;

	off_t next = off;

	int error;

	for(;;) {
		iovec.iov_base = entry.buf;
		iovec.iov_len = sizeof(entry.buf);
		uio.uio_resid = iovec.iov_len;
		uio.uio_loffset = next;

		error = VOP_READDIR(vp, &uio, NULL, &eofp);
		if(error)
			goto out;

		/* No more directory entries */
		if(iovec.iov_base == entry.buf)
			break;

		fstat.st_ino = entry.dirent.d_ino;
		fstat.st_mode = 0;

		int dsize = fuse_dirent_size(strlen(entry.dirent.d_name));
		if(dsize > outbuf_resid)
			break;

		fuse_add_dirent(outbuf + outbuf_off, entry.dirent.d_name, &fstat, entry.dirent.d_off);

		outbuf_off += dsize;
		outbuf_resid -= dsize;
		next = entry.dirent.d_off;
	}

out:
	ZFS_EXIT(zfsvfs);

	if(!error)
		error = -fuse_reply_buf(req, outbuf, outbuf_off);

	free(outbuf);

	return error;
}

static void zfsfuse_readdir_helper(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
	fuse_ino_t real_ino = ino == 1 ? 3 : ino;

	int error = zfsfuse_readdir(req, real_ino, size, off, fi);
	if(error)
		fuse_reply_err(req, error);
}

struct fuse_lowlevel_ops zfs_operations =
{
	.open       = hello_ll_open,
	.read       = hello_ll_read,
	.opendir    = zfsfuse_opendir_helper,
	.readdir    = zfsfuse_readdir_helper,
	.releasedir = zfsfuse_release_helper,
	.lookup     = zfsfuse_lookup_helper,
	.getattr    = zfsfuse_getattr_helper,
	.statfs     = zfsfuse_statfs,
	.destroy    = zfsfuse_destroy,
};
