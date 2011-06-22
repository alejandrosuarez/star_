// vim: noexpandtab
/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1992, 1993, 1994, 1995 Jan-Simon Pendry.
 * Copyright (c) 1992, 1993, 1994, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Jan-Simon Pendry.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)union_vnops.c	8.32 (Berkeley) 6/23/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vnode_internal.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/malloc.h>
#include <sys/buf_internal.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include "union.h"
#include <vfs/vfs_support.h>
#include <sys/ubc.h>
#include <sys/kdebug.h>
#include <sys/uio_internal.h>

static int stub(const char *name, vnode_t vp) {
	char buf[512] = "[???]";
	int len = 511;
	vn_getpath(vp, buf, &len);
	buf[len] = 0;
	printf("Stub for %s: %s\n", name, buf);
	return ENXIO;	
}
#define STUB(func, typ, field) static int func(typ ap) { return stub(#func, ap->field); }

__attribute__((const))
static char is_union(vnode_t vp) {
	return vp != NULLVP && vp->v_op == union_vnodeop_p;
}

__attribute__((const))
static vnode_t upper(vnode_t vp) {
	if(!is_union(vp)) return vp;
	return VTOUNION(vp)->un_uppervp;
}

__attribute__((const))
static vnode_t lower(vnode_t vp) {
	if(!is_union(vp)) return vp;
	return VTOUNION(vp)->un_lowervp;
}

/* called with no union lock held */
static int
union_lookup1(struct vnode *udvp, struct vnode **dvpp, struct vnode **vpp,
	struct componentname *cnp)
{
	int error;
	vfs_context_t ctx = cnp->cn_context;
	struct vnode *tdvp;
	struct vnode *dvp;
	struct mount *mp;

	dvp = *dvpp;

	/*
	 * If stepping up the directory tree, check for going
	 * back across the mount point, in which case do what
	 * lookup would do by stepping back down the mount
	 * hierarchy.
	 */
	if (cnp->cn_flags & ISDOTDOT) {
		while ((dvp != udvp) && (dvp->v_flag & VROOT)) {
			/*
			 * Don't do the NOCROSSMOUNT check
			 * at this level.  By definition,
			 * union fs deals with namespaces, not
			 * filesystems.
			 */
			tdvp = dvp;
			*dvpp = dvp = dvp->v_mount->mnt_vnodecovered;
			vnode_put(tdvp);
			vnode_get(dvp);
		}
	}

	error = VNOP_LOOKUP(dvp, &tdvp, cnp, ctx);
	if (error)
		return (error);

	dvp = tdvp;
	/*
	 * Lastly check if the current node is a mount point in
	 * which case walk up the mount hierarchy making sure not to
	 * bump into the root of the mount tree (ie. dvp != udvp).
	 */
	while (dvp != udvp && (dvp->v_type == VDIR) &&
	       (mp = dvp->v_mountedhere)) {
		if (vfs_busy(mp, LK_NOWAIT)) {
			vnode_put(dvp);
			return(ENOENT);
		}
		error = VFS_ROOT(mp, &tdvp, ctx);
		vfs_unbusy(mp);
		if (error) {
			vnode_put(dvp);
			return (error);
		}

		vnode_put(dvp);
		dvp = tdvp;
	}

	*vpp = dvp;
	return (0);
}

static int
union_lookup(struct vnop_lookup_args *ap)
/*
	struct vnop_lookup_args {
		struct vnodeop_desc *a_desc;
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		vfs_context_t a_context;
	} *ap)
*/
{
	int error = 0;
	int uerror = 0;
	int  lerror = 0;
	struct vnode *uppervp, *lowervp;
	struct vnode *upperdvp, *lowerdvp;
	struct vnode *dvp = ap->a_dvp;
	struct union_node *dun;
	struct componentname *cnp = ap->a_cnp;
	vfs_context_t ctx = cnp->cn_context;
	int lockparent = cnp->cn_flags & LOCKPARENT;
	struct union_mount *um;
	kauth_cred_t saved_cred;
	struct vnode_attr va;
	int isfaultfs = 0;
	int retry_count = 0;

	*ap->a_vpp = NULLVP;
	
	printf("looking up %s\n", cnp->cn_nameptr);

retry:
	union_lock();
	um = MOUNTTOUNIONMOUNT(dvp->v_mount);
	dun = VTOUNION(dvp);
	upperdvp = dun->un_uppervp;
	lowerdvp = dun->un_lowervp;
	uppervp = NULLVP;
	lowervp = NULLVP;

	union_unlock();

	if(UNION_FAULTIN(um))
		isfaultfs = 1;

	if (isfaultfs == 0)
		cnp->cn_flags |= LOCKPARENT;
	
	/*
	 * do the lookup in the upper level.
	 * if that level comsumes additional pathnames,
	 * then assume that something special is going
	 * on and just return that vnode.
	 */
	if (upperdvp == NULLVP || lowerdvp == NULLVP) {
		error = EPWROFF;
		goto out;
	}

	if (lockparent != 0) 
		cnp->cn_flags &= ~LOCKPARENT;

	uerror = union_lookup1(um->um_uppervp, &upperdvp,
				&uppervp, cnp);

	if (cnp->cn_consume != 0) {
		vnode_get(*ap->a_vpp = uppervp);
		error = uerror;
		goto out;
	}

	if(uerror) {
		if(uerror != ENOENT && uerror != EJUSTRETURN) {
			error = uerror;
			goto out;
		}
	} else {
		printf("lookup: dir = %d\n", vnode_isdir(uppervp));
		if(!vnode_isdir(uppervp)) {
			vnode_get(*ap->a_vpp = uppervp);
			goto out;
		}
	}

	// x: so the upper either does not exist or is a directory

	/*
	 * in a similar way to the upper layer, do the lookup
	 * in the lower layer.   this time, if there is some
	 * component magic going on, then vnode_put whatever we got
	 * back from the upper layer and return the lower vnode
	 * instead.
	 */

	int nameiop;

	/*
	 * Only do a LOOKUP on the bottom node, since
	 * we won't be making changes to it anyway.
	 */
	nameiop = cnp->cn_nameiop;
	cnp->cn_nameiop = LOOKUP;

	if (lockparent) 
		cnp->cn_flags &= ~LOCKPARENT;

	lerror = union_lookup1(um->um_lowervp, &lowerdvp,
			&lowervp, cnp);
	cnp->cn_nameiop = nameiop;

	if (cnp->cn_consume != 0) {
		vnode_get(*ap->a_vpp = lowervp);
		error = lerror;
		goto out;
	}

	if (!lockparent) // wat, the case is exactly reversed from above? how does that make any sense?
		cnp->cn_flags &= ~LOCKPARENT;

	/*
	 * at this point, we have uerror and lerror indicating
	 * possible errors with the lookups in the upper and lower
	 * layers.  additionally, uppervp and lowervp are (locked)
	 * references to existing vnodes in the upper and lower layers.
	 *
	 * there are now three cases to consider.
	 * 1. if both layers returned an error, then return whatever
	 *    error the upper layer generated.
	 *
	 * 2. if the top layer failed and the bottom layer succeeded
	 *    then two subcases occur.
	 *    a.  the bottom vnode is not a directory, in which
	 *	  case just return a new union vnode referencing
	 *	  an empty top layer and the existing bottom layer.
	 *    b.  the bottom vnode is a directory, in which case
	 *	  create a new directory in the top-level and
	 *	  continue as in case 3. (X: I'm getting rid of this case)
	 *
	 * 3. if the top layer succeeded then return a new union
	 *    vnode referencing whatever the new top layer and
	 *    whatever the bottom layer returned.
	 */
	
	printf("lookup: uerror=%d lerror=%d\n", uerror, lerror);

	/* case 1. */
	if ((uerror != 0) && (lerror != 0)) {
		error = uerror;
		goto out;
	}

	/* case 2. */
	if (uerror != 0 /* && (lerror == 0) */ ) {
		vnode_get(*ap->a_vpp = lowervp);
		error = 0;
		goto out;
	}
	
	/* I have no idea */
	if (lerror != 0) {
		vnode_get(*ap->a_vpp = uppervp);
		error = 0;
		goto out;
	}

	printf("lookup: okay we are allocvp'ing\n");

	// I don't get this weird reference counting it so I'll just do this
	vnode_get(uppervp); vnode_get(lowervp);
	union_lock();
	error = union_allocvp(ap->a_vpp, dvp->v_mount, dvp, upperdvp, cnp,
			      uppervp, lowervp, 1);
	union_unlock();

	out:
	if (uppervp != NULLVP)
		vnode_put(uppervp);
	if (lowervp != NULLVP)
		vnode_put(lowervp);
	if (!lockparent)
		cnp->cn_flags &= ~LOCKPARENT;
	else
		cnp->cn_flags |= LOCKPARENT;	
	return (error);
}

static int
union_create(struct vnop_create_args *ap)
/*
	struct vnop_create_args {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct union_node *un = VTOUNION(ap->a_dvp);
	struct vnode *dvp = un->un_uppervp;
	struct componentname *cnp = ap->a_cnp;

	if (dvp != NULLVP) {
		int error;
		struct vnode *vp;
		struct mount *mp;


		mp = ap->a_dvp->v_mount;

		/* note that this is a direct passthrough to the filesystem */
		error = VNOP_CREATE(dvp, &vp, cnp, ap->a_vap, ap->a_context);
		if (error)
			return (error);

		/* if this is faulting filesystem and is a reg file, skip allocation of union node */
		if (UNNODE_FAULTIN(un) && (vp != NULLVP) && ((vnode_isreg(vp) != 0)|| (vnode_islnk(vp) != 0))) {
			*ap->a_vpp = vp;
			return(0);
		}


		union_lock();
		error = union_allocvp(ap->a_vpp, mp, NULLVP, NULLVP, cnp, vp,
				NULLVP, 1);
		union_unlock();
		if (error)
			vnode_put(vp);
		return (error);
	}

	return (EROFS);
}

STUB(union_whiteout, struct vnop_whiteout_args *, a_dvp)

/* mknod can do  fifos, chr, blk or whiteout entries */
static int
union_mknod(struct vnop_mknod_args *ap)
/*
	struct vnop_mknod_args {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct union_node *un = VTOUNION(ap->a_dvp);
	struct vnode *dvp = un->un_uppervp;
	struct componentname *cnp = ap->a_cnp;

	if (dvp != NULLVP) {
		int error;
		struct vnode *vp;
		struct mount *mp;


		mp = ap->a_dvp->v_mount;

		/* note that this is a direct passthrough to the filesystem */
		error = VNOP_MKNOD(dvp, &vp, cnp, ap->a_vap, ap->a_context);
		if (error)
			return (error);

		if (vp != NULLVP) {
			union_lock();
			error = union_allocvp(ap->a_vpp, mp, NULLVP, NULLVP,
					cnp, vp, NULLVP, 1);
			union_unlock();
			if (error)
				vnode_put(vp);
		}
		return (error);
	}
	return (EROFS);
}

static int
union_open(struct vnop_open_args *ap)
{
	stub("union_open", ap->a_vp);
    struct union_node *un = VTOUNION(ap->a_vp);
	printf("upper=%p lower=%p\n", un->un_uppervp, un->un_lowervp);
	if(un->un_uppervp != NULLVP) return VNOP_OPEN(un->un_uppervp, ap->a_mode, ap->a_context);
	un->un_openl++;
	if(un->un_lowervp != NULLVP) return VNOP_OPEN(un->un_lowervp, ap->a_mode, ap->a_context);
	return ENOMSG;
}

static int
union_close(struct vnop_close_args *ap)
/*
	struct vnop_close_args {
		struct vnode *a_vp;
		int  a_fflag;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct union_node *un = VTOUNION(ap->a_vp);
	struct vnode *vp;
	int error = 0;

	if ((vp = un->un_uppervp) == NULLVP) {
#ifdef UNION_DIAGNOSTIC
		if (un->un_openl <= 0)
			panic("union: un_openl cnt");
#endif
		--un->un_openl;
		vp = un->un_lowervp;
	}

	ap->a_vp = vp;
	error =  (VCALL(vp, VOFFSET(vnop_close), ap));
	return(error);
}

/*
 * Check access permission on the union vnode.
 * The access check being enforced is to check
 * against both the underlying vnode, and any
 * copied vnode.  This ensures that no additional
 * file permissions are given away simply because
 * the user caused an implicit file copy.
 */
static int
union_access(struct vnop_access_args *ap)
/*
	struct vnop_access_args {
		struct vnodeop_desc *a_desc;
		struct vnode *a_vp;
		int a_action;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct union_node *un = VTOUNION(ap->a_vp);
	int error = EACCES;
	struct vnode *vp;

	if ((vp = un->un_uppervp) != NULLVP) {
		ap->a_vp = vp;
		return (VCALL(vp, VOFFSET(vnop_access), ap));
	}

	if ((vp = un->un_lowervp) != NULLVP) {
		ap->a_vp = vp;
		error = VCALL(vp, VOFFSET(vnop_access), ap);
		if (error == 0) {
			struct union_mount *um = MOUNTTOUNIONMOUNT(vp->v_mount);

			if (um->um_op == UNMNT_BELOW) {
				error = VCALL(vp, VOFFSET(vnop_access), ap);
			}
		}
		if (error)
			return (error);
	}

	return (error);
}

/*
 * We handle getattr only to change the fsid and
 * track object sizes
 */
static int
union_getattr(struct vnop_getattr_args *ap)
/*
	struct vnop_getattr_args {
		struct vnode *a_vp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	} *ap;
*/
{
	int error=0;
	struct union_node *un = VTOUNION(ap->a_vp);
	struct vnode *vp = un->un_uppervp;
	struct vnode_attr *vap;
	struct vnode_attr va;


	/*
	 * Some programs walk the filesystem hierarchy by counting
	 * links to directories to avoid stat'ing all the time.
	 * This means the link count on directories needs to be "correct".
	 * The only way to do that is to call getattr on both layers
	 * and fix up the link count.  The link count will not necessarily
	 * be accurate but will be large enough to defeat the tree walkers.
	 */

	vap = ap->a_vap;

	vp = un->un_uppervp;
	if (vp != NULLVP) {
		/*
		 * It's not clear whether vnop_getattr is to be
		 * called with the vnode locked or not.  stat() calls
		 * it with (vp) locked, and fstat calls it with
		 * (vp) unlocked.
		 * In the mean time, compensate here by checking
		 * the union_node's lock flag.
		 */

		error = vnode_getattr(vp, vap, ap->a_context);
		if (error) {
			return (error);
		}
		union_lock();
		union_newsize(ap->a_vp, vap->va_data_size, VNOVAL);
		union_unlock();
	}

	if (vp == NULLVP) {
		vp = un->un_lowervp;
	} else if (vp->v_type == VDIR) {
		vp = un->un_lowervp;
		VATTR_INIT(&va);
		/* all we want from the lower node is the link count */
		VATTR_WANTED(&va, va_nlink);
		vap = &va;
	} else {
		vp = NULLVP;
	}

	if (vp != NULLVP) {
		error = vnode_getattr(vp, vap, ap->a_context);
		if (error) {
			return (error);
		}
		union_lock();
		union_newsize(ap->a_vp, VNOVAL, vap->va_data_size);
		union_unlock();
	}

	if ((vap != ap->a_vap) && (vap->va_type == VDIR))
		ap->a_vap->va_nlink += vap->va_nlink;

	VATTR_RETURN(ap->a_vap, va_fsid, ap->a_vp->v_mount->mnt_vfsstat.f_fsid.val[0]);
	return (0);
}

static int
union_setattr(struct vnop_setattr_args *ap)
/*
	struct vnop_setattr_args {
		struct vnode *a_vp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct union_node *un = VTOUNION(ap->a_vp);
	int error;

	/*
	 * Handle case of truncating lower object to zero size,
	 * by creating a zero length upper object.  This is to
	 * handle the case of open with O_TRUNC and O_CREAT.
	 */
	if (VATTR_IS_ACTIVE(ap->a_vap, va_data_size) &&
	    (un->un_uppervp == NULLVP) &&
	    /* assert(un->un_lowervp != NULLVP) */
	    (un->un_lowervp->v_type == VREG)) {
		union_lock();
		error = union_copyup(un, (ap->a_vap->va_data_size != 0), ap->a_context);
		union_unlock();
		if (error) {
			return (error);
		}
	}

	/*
	 * Try to set attributes in upper layer,
	 * otherwise return read-only filesystem error.
	 */
	if (un->un_uppervp != NULLVP) {
		error = vnode_setattr(un->un_uppervp, ap->a_vap, ap->a_context);
		if ((error == 0) && VATTR_IS_ACTIVE(ap->a_vap, va_data_size)) {
			union_lock();
			union_newsize(ap->a_vp, ap->a_vap->va_data_size, VNOVAL);
			union_unlock();
		}
	} else {
		error = EROFS;
	}

	return (error);
}

static int
union_read(struct vnop_read_args *ap)
/*
	struct vnop_read_args {
		struct vnode *a_vp;
		struct uio *a_uio;
		int  a_ioflag;
		vfs_context_t a_context;
	} *ap;
*/
{
	int error;
	struct vnode *vp = OTHERVP(ap->a_vp);

	error = VNOP_READ(vp, ap->a_uio, ap->a_ioflag, ap->a_context);

	/*
	 * XXX
	 * perhaps the size of the underlying object has changed under
	 * our feet.  take advantage of the offset information present
	 * in the uio structure.
	 */
	if (error == 0) {
		struct union_node *un = VTOUNION(ap->a_vp);
		off_t cur = ap->a_uio->uio_offset;

		if (vp == un->un_uppervp) {
			if (cur > un->un_uppersz) {
				union_lock();
				union_newsize(ap->a_vp, cur, VNOVAL);
				union_unlock();
			}
		} else {
			if (cur > un->un_lowersz) {
				union_lock();
				union_newsize(ap->a_vp, VNOVAL, cur);
				union_unlock();
			}
		}
	}

	return (error);
}

static int
union_write(struct vnop_write_args *ap)
/*
	struct vnop_write_args {
		struct vnode *a_vp;
		struct uio *a_uio;
		int  a_ioflag;
		vfs_context_t a_context;
	} *ap;
*/
{
	int error;
	struct vnode *vp;
	struct union_node *un = VTOUNION(ap->a_vp);

	vp = UPPERVP(ap->a_vp);
	if (vp == NULLVP)
		panic("union: missing upper layer in write");

	error = VNOP_WRITE(vp, ap->a_uio, ap->a_ioflag, ap->a_context);

	/*
	 * the size of the underlying object may be changed by the
	 * write.
	 */
	if (error == 0) {
		off_t cur = ap->a_uio->uio_offset;

		if (cur > un->un_uppersz) {
			union_lock();
			union_newsize(ap->a_vp, cur, VNOVAL);
			union_unlock();
		}
	}

	return (error);
}


static int
union_ioctl(struct vnop_ioctl_args *ap)
/*
	struct vnop_ioctl_args {
		struct vnode *a_vp;
		int  a_command;
		caddr_t  a_data;
		int  a_fflag;
		vfs_context_t a_context;
	} *ap;
*/
{
	register struct vnode *ovp = OTHERVP(ap->a_vp);

	ap->a_vp = ovp;
	return (VCALL(ovp, VOFFSET(vnop_ioctl), ap));
}

static int
union_select(struct vnop_select_args *ap)
/*
	struct vnop_select_args {
		struct vnode *a_vp;
		int  a_which;
		int  a_fflags;
		void * a_wql;
		vfs_context_t a_context;
	} *ap;
*/
{
	register struct vnode *ovp = OTHERVP(ap->a_vp);

	ap->a_vp = ovp;
	return (VCALL(ovp, VOFFSET(vnop_select), ap));
}

static int
union_revoke(struct vnop_revoke_args *ap)
/*
	struct vnop_revoke_args {
		struct vnode *a_vp;
		int a_flags;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct vnode *vp = ap->a_vp;

	if (UPPERVP(vp))
		VNOP_REVOKE(UPPERVP(vp), ap->a_flags, ap->a_context);
	if (LOWERVP(vp))
		VNOP_REVOKE(LOWERVP(vp), ap->a_flags, ap->a_context);
	vnode_reclaim(vp);

	return (0);
}

static int
union_mmap(struct vnop_mmap_args *ap)
/*
	struct vnop_mmap_args {
		struct vnode *a_vp;
		int  a_fflags;
		kauth_cred_t a_cred;
		struct proc *a_p;
	} *ap;
*/
{
	register struct vnode *ovp = OTHERVP(ap->a_vp);

	ap->a_vp = ovp;
	return (VCALL(ovp, VOFFSET(vnop_mmap), ap));
}

static int
union_mnomap(struct vnop_mnomap_args *ap)
/*
	struct vnop_mnomap_args {
		struct vnode *a_vp;
		int  a_fflags;
		kauth_cred_t a_cred;
		struct proc *a_p;
	} *ap;
*/
{
	register struct vnode *ovp = OTHERVP(ap->a_vp);

	ap->a_vp = ovp;
	return (VCALL(ovp, VOFFSET(vnop_mnomap), ap));
}

static int
union_fsync(struct vnop_fsync_args *ap)
/*
	struct vnop_fsync_args {
		struct vnode *a_vp;
		int  a_waitfor;
		vfs_context_t a_context;
	} *ap;
*/
{
	int error = 0;
	struct vnode *targetvp = OTHERVP(ap->a_vp);

	if (targetvp != NULLVP) {

		error = VNOP_FSYNC(targetvp, ap->a_waitfor, ap->a_context);
	}

	return (error);
}

static int
union_remove(struct vnop_remove_args *ap)
/*
	struct vnop_remove_args {
		struct vnode *a_dvp;
		struct vnode *a_vp;
		struct componentname *a_cnp;
		vfs_context_t a_context;
	} *ap;
*/
{
	int error, flags;
	vnode_t dvp = ap->a_dvp;
	vnode_t vp = ap->a_vp;
	//IOLog("%d %p %p %d %p %p\n", is_union(ap->a_dvp), dun->un_uppervp, dun->un_lowervp, is_union(ap->a_vp), un->un_uppervp, un->un_lowervp);
	//return ENXIO;
	struct componentname *cnp = ap->a_cnp;
	int busydel = 0;

	if(!vnode_isdir(ap->a_vp)) {
		int error = ENXIO;
		if(upper(dvp) != NULLVP && upper(vp) != NULLVP) if(error = VNOP_REMOVE(upper(dvp), upper(vp), ap->a_cnp, ap->a_flags, ap->a_context)) return error;
		if(lower(dvp) != NULLVP && lower(vp) != NULLVP) if(error = VNOP_REMOVE(lower(dvp), lower(vp), ap->a_cnp, ap->a_flags, ap->a_context)) return error;
		return error;
	}
	
	if(!is_union(dvp) || !is_union(dvp)) return ENXIO;

	struct union_node *dun = VTOUNION(ap->a_dvp);
	struct union_node *un = VTOUNION(ap->a_vp);

	if (dun->un_uppervp == NULLVP)
		panic("union remove: null upper vnode");

	if (un->un_uppervp != NULLVP) {
		struct vnode *dvp = dun->un_uppervp;
		struct vnode *vp = un->un_uppervp;

		flags = ap->a_flags;
		if (vnode_isinuse(ap->a_vp, 0))
			busydel = 1;
		if ((flags & VNODE_REMOVE_NODELETEBUSY) && (busydel != 0)) {
				return(EBUSY);
		}
		if (union_dowhiteout(un, cnp->cn_context))
			cnp->cn_flags |= DOWHITEOUT;
	
		if (busydel != 0)  {
			union_lock();
			un->un_flags |= UN_DELETED;
			if (un->un_flags & UN_CACHED) {
				un->un_flags &= ~UN_CACHED;
				LIST_REMOVE(un, un_cache);
			}
			union_unlock();
			vnode_ref(vp);
		}
		error = VNOP_REMOVE(dvp, vp, cnp, 0, ap->a_context);
		if (!error) {
			union_lock();
			if (busydel == 0)
				union_removed_upper(un);
			union_unlock();
		}
	} else {
		if (UNNODE_FAULTIN(un))
			panic("faultfs: No uppervp");
		error = union_mkwhiteout(
			MOUNTTOUNIONMOUNT(UNIONTOV(dun)->v_mount),
			dun->un_uppervp, ap->a_cnp, un->un_path);
	}

	return (error);
}

static int
union_link(struct vnop_link_args *ap)
/*
	struct vnop_link_args {
		struct vnode *a_vp;
		struct vnode *a_tdvp;
		struct componentname *a_cnp;
		vfs_context_t a_context;
	} *ap;
*/
{
	int error = 0;
	struct componentname *cnp = ap->a_cnp;
	struct union_node *un;
	struct vnode *vp;
	struct vnode *tdvp;

	un = VTOUNION(ap->a_tdvp);

	if (ap->a_tdvp->v_op != ap->a_vp->v_op) {
		vp = ap->a_vp;
	} else {
		struct union_node *tun = VTOUNION(ap->a_vp);
		if (tun->un_uppervp == NULLVP) {
			if (UNNODE_FAULTIN(tun))
				panic("faultfs: No uppervp");
			if (un->un_uppervp == tun->un_dirvp) {
			}
			union_lock();
			/* Would need to drain for above,below mount and faulin does not enter this path */
			un->un_flags |= UN_LOCKED;
			error = union_copyup(tun, 1, ap->a_context);
			un->un_flags &= ~UN_LOCKED;
			if ((un->un_flags & UN_WANT) == UN_WANT) {
				un->un_flags &=  ~UN_WANT;
				wakeup(&un->un_flags);
			}
			union_unlock();
		}
		vp = tun->un_uppervp;
	}
	tdvp = un->un_uppervp;
	if (tdvp == NULLVP)
		error = EROFS;

	if (error) {
		return (error);
	}


	error =  (VNOP_LINK(vp, tdvp, cnp, ap->a_context));
	return(error);
}

static int
union_rename(struct vnop_rename_args *ap)
/*
	struct vnop_rename_args {
		struct vnode *a_fdvp;
		struct vnode *a_fvp;
		struct componentname *a_fcnp;
		struct vnode *a_tdvp;
		struct vnode *a_tvp;
		struct componentname *a_tcnp;
		vfs_context_t a_context;
	} *ap;
*/
{
	// This is probably wrong in most cases
	int error;

	struct vnode *fdvp = ap->a_fdvp;
	struct vnode *fvp = ap->a_fvp;
	struct vnode *tdvp = ap->a_tdvp;
	struct vnode *tvp = ap->a_tvp;

	struct union_node *dun = VTOUNION(fdvp);
	struct union_node *un = VTOUNION(fvp);

	IOLog("vps: %p, %p, %p, %p\n", fdvp, fvp, tdvp, tvp);
	IOLog("upper:%p, %p, %p, %p\n", dun->un_uppervp, un->un_uppervp, upper(tdvp), upper(tvp));
	IOLog("lower:%p, %p, %p, %p\n", dun->un_lowervp, un->un_lowervp, lower(tdvp), lower(tvp));
	return ENXIO;

	
	if(un->un_uppervp != NULLVP) {
		if(dun->un_uppervp == NULLVP) panic("wat (upper)");
		if(error = VNOP_RENAME(dun->un_uppervp, un->un_uppervp, ap->a_fcnp, upper(tdvp), upper(tvp), ap->a_tcnp, ap->a_context))  {
			printf("got an error from upper: %d\n", error);
			return error;
		}
	}
	
	if(un->un_lowervp != NULLVP) {
		if(dun->un_lowervp == NULLVP) panic("wat (lower)");
		if(error = VNOP_RENAME(dun->un_lowervp, un->un_lowervp, ap->a_fcnp, lower(tdvp), lower(tvp), ap->a_tcnp, ap->a_context)) {
			if(un->un_uppervp != NULLVP) {
				printf("bad bad bad - the first rename succeeded but the second one failed (%d); trying to rename it back\n", error);
				/*
				int error2;
				if(error2 = VNOP_RENAME(dun->un_uppervp, un->un_uppervp, ap->a_fcnp, upper(fdvp), NULLVP, ap->a_fcnp, ap->a_context))  {
					printf("that failed too: %d\n", error2);
				}
				*/
			}
			return error;
		}
	}

	return 0;
}

static int
union_mkdir(struct vnop_mkdir_args *ap)
/*
	struct vnop_mkdir_args {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct union_node *un = VTOUNION(ap->a_dvp);
	struct vnode *dvp = un->un_uppervp;
	struct componentname *cnp = ap->a_cnp;

	if (dvp != NULLVP) {
		int error;
		struct vnode *vp;


		/* note that this is a direct fallthrough to the filesystem */
		error = VNOP_MKDIR(dvp, &vp, cnp, ap->a_vap, ap->a_context);
		if (error)
			return (error);

		union_lock();
		error = union_allocvp(ap->a_vpp, ap->a_dvp->v_mount, ap->a_dvp,
				NULLVP, cnp, vp, NULLVP, 1);
		union_unlock();
		if (error)
			vnode_put(vp);
		return (error);
	}
	return (EROFS);
}

static int
union_rmdir(struct vnop_rmdir_args *ap)
/*
	struct vnop_rmdir_args {
		struct vnode *a_dvp;
		struct vnode *a_vp;
		struct componentname *a_cnp;
		vfs_context_t a_context;
	} *ap;
*/
{
	int error;
	struct union_node *dun = VTOUNION(ap->a_dvp);
	struct union_node *un = VTOUNION(ap->a_vp);
	struct componentname *cnp = ap->a_cnp;
	int busydel = 0;

	/******* NODE HAS TO BE LOCKED ******/
	if (dun->un_uppervp == NULLVP)
		panic("union rmdir: null upper vnode");

	if (un->un_uppervp != NULLVP) {
		struct vnode *dvp = dun->un_uppervp;
		struct vnode *vp = un->un_uppervp;

		if (vnode_isinuse(ap->a_vp, 0)) {
			busydel = 1;
			union_lock();
			un->un_flags |= UN_DELETED;
			if (un->un_flags & UN_CACHED) {
				un->un_flags &= ~UN_CACHED;
				LIST_REMOVE(un, un_cache);
			}
			union_unlock();
			vnode_ref(vp);
		}


		if (union_dowhiteout(un, cnp->cn_context))
			cnp->cn_flags |= DOWHITEOUT;
		error = VNOP_RMDIR(dvp, vp, ap->a_cnp, ap->a_context);
		if (!error) {
			union_lock();
			if (busydel == 0)
				union_removed_upper(un);
			union_unlock();
		}
	} else {
		if (UNNODE_FAULTIN(un))
			panic("faultfs: No uppervp");
		error = union_mkwhiteout(
			MOUNTTOUNIONMOUNT(UNIONTOV(dun)->v_mount),
			dun->un_uppervp, ap->a_cnp, un->un_path);
	}
	return (error);
}

static int
union_symlink(struct vnop_symlink_args *ap)
/*
	struct vnop_symlink_args {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
		char *a_target;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct union_node *un = VTOUNION(ap->a_dvp);
	struct vnode *dvp = un->un_uppervp;
	struct componentname *cnp = ap->a_cnp;

	if (dvp != NULLVP) {
		int error;
		struct vnode *vp;

		error = VNOP_SYMLINK(dvp, &vp, cnp, ap->a_vap, ap->a_target, ap->a_context);
		*ap->a_vpp = vp;
		return (error);
	}
	return (EROFS);
}

/*
 * union_readdir works in concert with getdirentries and
 * readdir(3) to provide a list of entries in the unioned
 * directories.  getdirentries is responsible for walking
 * down the union stack.  readdir(3) is responsible for
 * eliminating duplicate names from the returned data stream.
 */
static int
union_readdir(struct vnop_readdir_args *ap)
/*
	struct vnop_readdir_args {
		struct vnodeop_desc *a_desc;
		struct vnode *a_vp;
		struct uio *a_uio;
		int a_flags;
		int *a_eofflag;
		int *a_numdirent;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct union_node *un = VTOUNION(ap->a_vp);
	struct vnode *uvp = un->un_uppervp;

	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF))
		return (EINVAL);

	if (uvp == NULLVP)
		return (0);

	ap->a_vp = uvp;
	return (VCALL(uvp, VOFFSET(vnop_readdir), ap));
}

STUB(union_readlink, struct vnop_readlink_args *, a_vp)

static int
union_inactive(struct vnop_inactive_args *ap)
/*
	struct vnop_inactive_args {
		struct vnode *a_vp;
		vfs_context_t a_context;
	} *ap;
*/
{
	struct vnode *vp = ap->a_vp;
	struct union_node *un = VTOUNION(vp);

	/*
	 * Do nothing (and _don't_ bypass).
	 * Wait to vnode_put lowervp until reclaim,
	 * so that until then our union_node is in the
	 * cache and reusable.
	 *
	 * NEEDSWORK: Someday, consider inactive'ing
	 * the lowervp and then trying to reactivate it
	 * with capabilities (v_id)
	 * like they do in the name lookup cache code.
	 * That's too much work for now.
	 */

	union_lock();
	if (un->un_flags & UN_DELETED) {
		if(un->un_uppervp != NULLVP) {
			vnode_rele(un->un_uppervp);
		}
		union_removed_upper(un);
	}

	if (un->un_dircache != 0)  {
			union_dircache_free(un);
	}
	if (un->un_flags & UN_DIRENVN) {
		vnode_recycle(vp);
	}

	union_unlock();

	return (0);
}

static int
union_reclaim(struct vnop_reclaim_args *ap)
/*
	struct vnop_reclaim_args {
		struct vnode *a_vp;
		vfs_context_t a_context;
	} *ap;
*/
{

	union_lock();
	union_freevp(ap->a_vp);
	union_unlock();

	return (0);
}

static int
union_blockmap(struct vnop_blockmap_args *ap)
/*
	struct vnop_blockmap_args {
		struct vnode *a_vp;
		off_t a_offset;    
		size_t a_size;
		daddr64_t *a_bpn;
		size_t *a_run;
		void *a_poff;
		int a_flags;
	} *ap;
*/
{
	int error;
	struct vnode *vp = OTHERVP(ap->a_vp);

	ap->a_vp = vp;
	error = VCALL(vp, VOFFSET(vnop_blockmap), ap);

	return (error);
}

static int
union_pathconf(struct vnop_pathconf_args *ap)
/*
	struct vnop_pathconf_args {
		struct vnode *a_vp;
		int a_name;
		int *a_retval;
		vfs_context_t a_context;
	} *ap;
*/
{
	int error;
	struct vnode *vp = OTHERVP(ap->a_vp);

	ap->a_vp = vp;
	error = VCALL(vp, VOFFSET(vnop_pathconf), ap);

	return (error);
}

static int
union_advlock(struct vnop_advlock_args *ap)
/*
	struct vnop_advlock_args {
		struct vnode *a_vp;
		caddr_t  a_id;
		int  a_op;
		struct flock *a_fl;
		int  a_flags;
		vfs_context_t a_context;
	} *ap;
*/
{
	register struct vnode *ovp = OTHERVP(ap->a_vp);

	ap->a_vp = ovp;
	return (VCALL(ovp, VOFFSET(vnop_advlock), ap));
}


/*
 * XXX - vnop_strategy must be hand coded because it has no
 * vnode in its arguments.
 * This goes away with a merged VM/buffer cache.
 */
static int
union_strategy(struct vnop_strategy_args *ap)
/*
	struct vnop_strategy_args {
		struct buf *a_bp;
	} *ap;
*/
{
	struct buf *bp = ap->a_bp;
	int error;
	struct vnode *savedvp;

	savedvp = buf_vnode(bp);
	buf_setvnode(bp, OTHERVP(savedvp));

#if DIAGNOSTIC
	if (buf_vnode(bp) == NULLVP)
		panic("union_strategy: nil vp");
	if (((buf_flags(bp) & B_READ) == 0) &&
	    (buf_vnode(bp) == LOWERVP(savedvp)))
		panic("union_strategy: writing to lowervp");
#endif

	error = VNOP_STRATEGY(bp);
	buf_setvnode(bp, savedvp);

	return (error);
}

/* Pagein */
STUB(union_pagein, struct vnop_pagein_args *, a_vp)

/* Pageout  */
STUB(union_pageout, struct vnop_pageout_args *, a_vp)

/* Blktooff derives file offset for the given logical block number */
STUB(union_blktooff, struct vnop_blktooff_args *, a_vp)

/* offtoblk derives file offset for the given logical block number */
STUB(union_offtoblk, struct vnop_offtoblk_args *, a_vp)

#define VOPFUNC int (*)(void *)

/*
 * Global vfs data structures
 */
int (**union_vnodeop_p)(void *);
struct vnodeopv_entry_desc union_vnodeop_entries[40];
struct vnodeopv_desc union_vnodeop_opv_desc =
	{ &union_vnodeop_p, union_vnodeop_entries };
void init_vnodeop_entries() {
	struct vnodeopv_entry_desc foo[] = {
		{ &vnop_default_desc, (VOPFUNC)vn_default_error },
		{ &vnop_lookup_desc, (VOPFUNC)union_lookup },		/* lookup */
		{ &vnop_create_desc, (VOPFUNC)union_create },		/* create */
		{ &vnop_whiteout_desc, (VOPFUNC)union_whiteout },	/* whiteout */
		{ &vnop_mknod_desc, (VOPFUNC)union_mknod },		/* mknod */
		{ &vnop_open_desc, (VOPFUNC)union_open },		/* open */
		{ &vnop_close_desc, (VOPFUNC)union_close },		/* close */
		{ &vnop_access_desc, (VOPFUNC)union_access },		/* access */
		{ &vnop_getattr_desc, (VOPFUNC)union_getattr },		/* getattr */
		{ &vnop_setattr_desc, (VOPFUNC)union_setattr },		/* setattr */
		{ &vnop_read_desc, (VOPFUNC)union_read },		/* read */
		{ &vnop_write_desc, (VOPFUNC)union_write },		/* write */
		{ &vnop_ioctl_desc, (VOPFUNC)union_ioctl },		/* ioctl */
		{ &vnop_select_desc, (VOPFUNC)union_select },		/* select */
		{ &vnop_revoke_desc, (VOPFUNC)union_revoke },		/* revoke */
		{ &vnop_mmap_desc, (VOPFUNC)union_mmap },		/* mmap */
		{ &vnop_mnomap_desc, (VOPFUNC)union_mnomap },		/* mnomap */
		{ &vnop_fsync_desc, (VOPFUNC)union_fsync },		/* fsync */
		{ &vnop_remove_desc, (VOPFUNC)union_remove },		/* remove */
		{ &vnop_link_desc, (VOPFUNC)union_link },		/* link */
		{ &vnop_rename_desc, (VOPFUNC)union_rename },		/* rename */
		{ &vnop_mkdir_desc, (VOPFUNC)union_mkdir },		/* mkdir */
		{ &vnop_rmdir_desc, (VOPFUNC)union_rmdir },		/* rmdir */
		{ &vnop_symlink_desc, (VOPFUNC)union_symlink },		/* symlink */
		{ &vnop_readdir_desc, (VOPFUNC)union_readdir },		/* readdir */
		{ &vnop_readlink_desc, (VOPFUNC)union_readlink },	/* readlink */
		{ &vnop_inactive_desc, (VOPFUNC)union_inactive },	/* inactive */
		{ &vnop_reclaim_desc, (VOPFUNC)union_reclaim },		/* reclaim */
		{ &vnop_strategy_desc, (VOPFUNC)union_strategy },	/* strategy */
		{ &vnop_pathconf_desc, (VOPFUNC)union_pathconf },	/* pathconf */
		{ &vnop_advlock_desc, (VOPFUNC)union_advlock },		/* advlock */
	#ifdef notdef
		{ &vnop_bwrite_desc, (VOPFUNC)union_bwrite },		/* bwrite */
	#endif
		{ &vnop_pagein_desc, (VOPFUNC)union_pagein },		/* Pagein */
		{ &vnop_pageout_desc, (VOPFUNC)union_pageout },		/* Pageout */
		{ &vnop_copyfile_desc, (VOPFUNC)eopnotsupp },		/* Copyfile */
		{ &vnop_blktooff_desc, (VOPFUNC)union_blktooff },	/* blktooff */
		{ &vnop_offtoblk_desc, (VOPFUNC)union_offtoblk },	/* offtoblk */
		{ &vnop_blockmap_desc, (VOPFUNC)union_blockmap },	/* blockmap */
		{ (struct vnodeop_desc*)NULL, (int(*)())NULL }
	};
	memcpy(union_vnodeop_entries, foo, sizeof(foo));
}
