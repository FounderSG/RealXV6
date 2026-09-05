#include "os.h"

/*
 * the fstat system call.
 */
void fstat(void)
{
    register struct file *fp;

    fp = getf(u.u_ar0[R0]);
    if(fp == NULL)
        return;
    stat1(fp->f_inode, u.u_arg[0]);
}

/*
 * the stat system call.
 */
void stat(void)
{
    register struct inode *ip;

    ip = namei(&uchar, 0);
    if(ip == NULL)
        return;
    stat1(ip, u.u_arg[1]);
    iput(ip);
}

/*
 * The basic routine for fstat and stat:
 * get the inode and pass appropriate parts back.
 */
void stat1(struct inode *ip, int ub)
{
    register int i;
    register struct buf *bp;
    register int *cp;

    iupdat(ip, time);
    bp = bread(ip->i_dev, ldiv(ip->i_number+31, 16));
    cp = &(ip->i_dev);
    for(i=0; i<14; i++) {
        suword(ub, *cp++);
        ub += 2;
    }
    cp = (uint)bp->b_addr + 32*lrem(ip->i_number+31, 16) + 24;
    for(i=0; i<4; i++) {
        suword(ub, *cp++);
        ub += 2;
    }
    brelse(bp);
}

/*
 * the dup system call.
 */
void dup(void)
{
    register int i;
    register struct file *fp;

    fp = getf(u.u_ar0[R0]);
    if(fp == NULL)
        return;
    if ((i = ufalloc()) < 0)
        return;
    u.u_ofile[i] = fp;
    fp->f_count++;
}

/*
 * the mount system call.
 */
void smount(void)
{
    int d;
    struct inode *ip;
    struct mount *mp, *smp;
    struct buf *bp;
    struct filsys *fsp;

    d = getmdev();
    if(u.u_error)
        return;
    u.u_dirp = u.u_arg[1];
    ip = namei(&uchar, 0);
    if(ip == NULL)
        return;
    if(ip->i_count!=1 || (ip->i_mode&(IFBLK&IFCHR))!=0)
        goto out;
    smp = NULL;
    for(mp = &mount[0]; mp < &mount[NMOUNT]; mp++) {
        if(mp->m_bufp != NULL) {
            if(d == mp->m_dev)
                goto out;
        } else
        if(smp == NULL)
            smp = mp;
    }
    if(smp == NULL)
        goto out;
    (*bdevsw[major(d)].d_open)(d, !u.u_arg[2]);
    if(u.u_error)
        goto out;
    bp = bread(d, 1);
    if(u.u_error) {
        brelse(bp);
        goto out1;
    }
    smp->m_inodp = ip;
    smp->m_dev = d;
    smp->m_bufp = getblk(NODEV, 0);
    bcopy(bp->b_addr, smp->m_bufp->b_addr, 256);
    fsp = smp->m_bufp->b_addr;
    fsp->s_ilock = 0;
    fsp->s_flock = 0;
    fsp->s_ronly = u.u_arg[2] & 1;
    brelse(bp);
    ip->i_flag |= IMOUNT;
    prele(ip);
    return;

out:
    u.u_error = EBUSY;
out1:
    iput(ip);
}

/*
 * the umount system call.
 */
void sumount(void)
{
    int d;
    register struct inode *ip;
    register struct mount *mp;
    register struct buf *bp;

    update();
    d = getmdev();
    if(u.u_error)
        return;
    for(mp = &mount[0]; mp < &mount[NMOUNT]; mp++)
        if(mp->m_bufp!=NULL && d==mp->m_dev)
            goto found;
    u.u_error = EINVAL;
    return;

found:
    for(ip = &inode[0]; ip < &inode[NINODE]; ip++)
        if(ip->i_number!=0 && d==ip->i_dev) {
            u.u_error = EBUSY;
            return;
        }
    (*bdevsw[major(d)].d_close)(d, 0);
    ip = mp->m_inodp;
    ip->i_flag &= ~IMOUNT;
    iput(ip);
    bp = mp->m_bufp;
    mp->m_bufp = NULL;
    brelse(bp);
}

/*
 * Common code for mount and umount.
 * Check that the user's argument is a reasonable
 * thing on which to mount, and return the device number if so.
 */
int getmdev(void)
{
    register int d;
    register struct inode *ip;

    ip = namei(&uchar, 0);
    if(ip == NULL)
        return -1;
    if((ip->i_mode&IFMT) != IFBLK)
        u.u_error = ENOTBLK;
    d = ip->i_addr[0];
    if(major(ip->i_addr[0]) >= nblkdev)
        u.u_error = ENXIO;
    iput(ip);
    return(d);
}
