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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*	Copyright (c) 1983, 1984, 1985, 1986, 1987, 1988, 1989 AT&T	*/
/*	  All Rights Reserved  	*/

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */

#include <sys/vnode.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/debug.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/rwstlock.h>
#include <sys/vfs.h>

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <umem.h>

#include <sys/ioctl.h>
/* LINUX BLKGETSIZE64 */
#include <sys/mount.h>

/*
 * Convert stat(2) formats to vnode types and vice versa.  (Knows about
 * numerical order of S_IFMT and vnode types.)
 */
enum vtype iftovt_tab[] = {
	VNON, VFIFO, VCHR, VNON, VDIR, VNON, VBLK, VNON,
	VREG, VNON, VLNK, VNON, VSOCK, VNON, VNON, VNON
};

ushort_t vttoif_tab[] = {
	0, S_IFREG, S_IFDIR, S_IFBLK, S_IFCHR, S_IFLNK, S_IFIFO,
	0, 0, S_IFSOCK, 0, 0
};

/*
 * vn_vfswlock is used to implement a lock which is logically a writers lock
 * protecting the v_vfsmountedhere field.
 */
int
vn_vfswlock(vnode_t *vp)
{
	vn_vfslocks_entry_t *vpvfsentry;

	/*
	 * If vp is NULL then somebody is trying to lock the covered vnode
	 * of /.  (vfs_vnodecovered is NULL for /).  This situation will
	 * only happen when unmounting /.  Since that operation will fail
	 * anyway, return EBUSY here instead of in VFS_UNMOUNT.
	 */
	if (vp == NULL)
		return (EBUSY);

	vpvfsentry = vn_vfslocks_getlock_vnode(vp);

	if (rwst_tryenter(&vpvfsentry->ve_lock, RW_WRITER))
		return (0);

	vn_vfslocks_rele(vpvfsentry);
	return (EBUSY);
}

void
vn_vfsunlock(vnode_t *vp)
{
	vn_vfslocks_entry_t *vpvfsentry;

	/*
	 * ve_refcnt needs to be decremented twice.
	 * 1. To release refernce after a call to vn_vfslocks_getlock()
	 * 2. To release the reference from the locking routines like
	 *    vn_vfsrlock/vn_vfswlock etc,.
	 */
	vpvfsentry = vn_vfslocks_getlock(vp);
	vn_vfslocks_rele(vpvfsentry);

	rwst_exit(&vpvfsentry->ve_lock);
	vn_vfslocks_rele(vpvfsentry);
}

vnode_t *vn_alloc(int kmflag)
{
	ASSERT(kmflag == 0 || kmflag == UMEM_NOFAIL);

	vnode_t *vp;

	vp = umem_alloc(sizeof(vnode_t), kmflag);

	/* taken from vn_cache_constructor */
	mutex_init(&vp->v_lock, NULL, MUTEX_DEFAULT, NULL);
	rwst_init(&vp->v_vfsmhlock.ve_lock, NULL, RW_DEFAULT, NULL);

	if(vp != NULL) {
		vp->v_path = NULL;
		vn_reinit(vp);
	}

	return vp;
}

void vn_reinit(vnode_t *vp)
{
	vp->v_vfsp = NULL;
	vp->v_fd = -1;
	vp->v_size = 0;
	vp->v_data = NULL;
	vp->v_count = 1;

	vn_recycle(vp);
}

void vn_recycle(vnode_t *vp)
{
	if(vp->v_path != NULL) {
		free(vp->v_path);
		vp->v_path = NULL;
	}
}

void vn_free(vnode_t *vp)
{
	ASSERT(vp->v_count == 0 || vp->v_count == 1);

	vn_close(vp);
}

/*
 * Note: for the xxxat() versions of these functions, we assume that the
 * starting vp is always rootdir (which is true for spa_directory.c, the only
 * ZFS consumer of these interfaces).  We assert this is true, and then emulate
 * them by adding '/' in front of the path.
 */

/*ARGSUSED*/
int
vn_open(char *path, enum uio_seg x1, int flags, int mode, vnode_t **vpp, enum create x2, mode_t x3)
{
	int fd;
	vnode_t *vp;
	int old_umask = 0;
	char realpath[MAXPATHLEN];
	struct stat64 st;

	/*
	 * If we're accessing a real disk from userland, we need to use
	 * the character interface to avoid caching.  This is particularly
	 * important if we're trying to look at a real in-kernel storage
	 * pool from userland, e.g. via zdb, because otherwise we won't
	 * see the changes occurring under the segmap cache.
	 * On the other hand, the stupid character device returns zero
	 * for its size.  So -- gag -- we open the block device to get
	 * its size, and remember it for subsequent VOP_GETATTR().
	 */
	/* FIXME: Clean this up */
	if (strncmp(path, "/dev/", 5) == 0) {
		char *dsk;
		fd = open64(path, O_RDONLY);
		if (fd == -1)
			return (errno);
		if (fstat64(fd, &st) == -1) {
			close(fd);
			return (errno);
		}
		close(fd);
		(void) sprintf(realpath, "%s", path);
		dsk = strstr(path, "/dsk/");
		if (dsk != NULL)
			(void) sprintf(realpath + (dsk - path) + 1, "r%s",
			    dsk + 1);
	} else {
		(void) sprintf(realpath, "%s", path);
		if (!(flags & FCREAT) && stat64(realpath, &st) == -1)
			return (errno);
	}

	if (flags & FCREAT)
		old_umask = umask(0);

	/*
	 * The construct 'flags - FREAD' conveniently maps combinations of
	 * FREAD and FWRITE to the corresponding O_RDONLY, O_WRONLY, and O_RDWR.
	 */
	fd = open64(realpath, flags - FREAD, mode);

	if (flags & FCREAT)
		(void) umask(old_umask);

	if (fd == -1)
		return (errno);

	if (fstat64(fd, &st) == -1) {
		close(fd);
		return (errno);
	}

	(void) fcntl(fd, F_SETFD, FD_CLOEXEC);

	*vpp = vp = umem_zalloc(sizeof (vnode_t), UMEM_NOFAIL);

	vp->v_fd = fd;
	if(S_ISBLK(st.st_mode)) {
		/* LINUX */
		if(ioctl(fd, BLKGETSIZE64, &vp->v_size) != 0)
			return errno;
	} else
		vp->v_size = st.st_size;
	vp->v_path = strdup(path);

	vp->v_type = VNON;

	if(S_ISREG(st.st_mode))
		vp->v_type = VREG;
	else if(S_ISDIR(st.st_mode))
		vp->v_type = VDIR;
	else if(S_ISCHR(st.st_mode))
		vp->v_type = VCHR;
	else if(S_ISBLK(st.st_mode))
		vp->v_type = VBLK;
	else if(S_ISFIFO(st.st_mode))
		vp->v_type = VFIFO;
	else if(S_ISLNK(st.st_mode))
		vp->v_type = VLNK;
	else if(S_ISSOCK(st.st_mode))
		vp->v_type = VSOCK;

	VERIFY(vp->v_type != VNON);

	zmutex_init(&vp->v_lock);
	rwst_init(&vp->v_vfsmhlock.ve_lock, NULL, RW_DEFAULT, NULL);

	vp->v_count = 1;

	return (0);
}

int
vn_openat(char *path, enum uio_seg x1, int flags, int mode, vnode_t **vpp, enum create x2,
    mode_t x3, vnode_t *startvp)
{
	char *realpath = umem_alloc(strlen(path) + 2, UMEM_NOFAIL);
	int ret;

	ASSERT(startvp == rootdir);
	(void) sprintf(realpath, "/%s", path);

	ret = vn_open(realpath, x1, flags, mode, vpp, x2, x3);

	umem_free(realpath, strlen(path) + 2);

	return (ret);
}

/*ARGSUSED*/
int
vn_rdwr(enum uio_rw uio, vnode_t *vp, caddr_t addr, ssize_t len, offset_t offset,
	enum uio_seg x1, int x2, rlim64_t x3, cred_t *x4, ssize_t *residp)
{
	ssize_t iolen;

	if (uio == UIO_READ)
		iolen = pread64(vp->v_fd, addr, len, offset);
	else
		iolen = pwrite64(vp->v_fd, addr, len, offset);

	if(iolen < len)
		fprintf(stderr, "%s: len: %lli iolen: %lli offset: %lli file: %s\n", uio == UIO_READ ? "UIO_READ" : "UIO_WRITE", (longlong_t) len, (longlong_t) iolen, (longlong_t) offset, vp->v_path);

	if (iolen == -1)
		return (errno);
	if (residp)
		*residp = len - iolen;
	else if (iolen != len)
		return (EIO);
	return (0);
}

void vn_rele(vnode_t *vp)
{
	ASSERT(vp->v_count > 0);

	mutex_enter(&vp->v_lock);
	if(vp->v_count == 1) {
		mutex_exit(&vp->v_lock);
		/* ZFSFUSE: FIXME FIXME */
// 		zfs_inactive(
	} else {
		vp->v_count--;
		mutex_exit(&vp->v_lock);
	}
}

void
vn_close(vnode_t *vp)
{
	rwst_destroy(&vp->v_vfsmhlock.ve_lock);
	zmutex_destroy(&vp->v_lock);
	if(vp->v_fd != -1)
		close(vp->v_fd);
	if(vp->v_path != NULL)
		free(vp->v_path);
	umem_free(vp, sizeof (vnode_t));
}
