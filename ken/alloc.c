#include "os.h"

/*
 * iinit is called once (from main)
 * very early in initialization.
 * It reads the root's super block
 * and initializes the current date
 * from the last modified date.
 *
 * panic: iinit -- cannot read the super
 * block. Usually because of an IO error.
 */
void iinit(void)
{
    struct buf *bp, *cp;
    struct filsys *fp;
    (*bdevsw[major(rootdev)].d_open)(rootdev, 1);
    bp = bread(rootdev, 1);
    cp = getblk(NODEV, 0);
    if (u.u_error)
        panic("iinit");
    bcopy(bp->b_addr, cp->b_addr, 256);
    brelse(bp);
    mount[0].m_bufp = cp;
    mount[0].m_dev = rootdev;
    fp = (struct filsys*)cp->b_addr;
    fp->s_flock = 0;
    fp->s_ilock = 0;
    fp->s_ronly = 0;
    time[0] = fp->s_time[0];
    time[1] = fp->s_time[1];
}

/*
 * alloc will obtain the next available
 * free disk block from the free list of
 * the specified device.
 * The super block has up to 100 remembered
 * free blocks; the last of these is read to
 * obtain 100 more . . .
 *
 * no space on dev x/y -- when
 * the free list is exhausted.
 */
struct buf *alloc(int dev)
{
    int bno;
    struct buf *bp;
    struct filsys *fp;
    int *ip;

    fp = getfs(dev);
    while(fp->s_flock)
        sleep(&fp->s_flock, PINOD);
    do {
        if(fp->s_nfree <= 0)
            goto nospace;
        bno = fp->s_free[--fp->s_nfree];
        if(bno == 0)
            goto nospace;
    } while (badblock(fp, bno, dev));
    if(fp->s_nfree <= 0) {
        fp->s_flock++;
        bp = bread(dev, bno);
        ip = bp->b_addr;
        fp->s_nfree = *ip++;
        bcopy(ip, fp->s_free, 100);
        brelse(bp);
        fp->s_flock = 0;
        wakeup(&fp->s_flock);
    }
    bp = getblk(dev, bno);
    clrbuf(bp);
    fp->s_fmod = 1;
    return(bp);

nospace:
    fp->s_nfree = 0;
    prdev("no space", dev);
    u.u_error = ENOSPC;
    return(NULL);
}

/*
 * place the specified disk block
 * back on the free list of the
 * specified device.
 */
void free(int dev, int bno)
{
    struct filsys *fp;
    struct buf *bp;
    int *ip;

    fp = getfs(dev);
    fp->s_fmod = 1;
    while(fp->s_flock)
        sleep(&fp->s_flock, PINOD);
    if(badblock(fp, bno, dev))
        return;
    if(fp->s_nfree <=0 ) {
        fp->s_nfree = 1;
        fp->s_free[0] = 0;
    }
    if(fp->s_nfree >= 100) {
        fp->s_flock++;
        bp = getblk(dev, bno);
        ip = bp->b_addr;
        *ip++ = fp->s_nfree;
        bcopy(fp->s_free, ip, 100);
        fp->s_nfree = 0;
        bwrite(bp);
        fp->s_flock = 0;
        wakeup(&fp->s_flock);
    }
    fp->s_free[fp->s_nfree++] = bno;
    fp->s_fmod = 1;
}

/*
 * Check that a block number is in the
 * range between the I list and the size
 * of the device.
 * This is used mainly to check that a
 * garbage file system has not been mounted.
 *
 * bad block on dev x/y -- not in range
 */
int badblock(struct filsys *fp, int abn, int dev)
{
    register uint bn = abn;

    if (bn < fp->s_isize+2 || bn >= fp->s_fsize) {
        prdev("bad block", dev);
        return(1);
    }
    return(0);
}

/*
 * Allocate an unused I node
 * on the specified device.
 * Used with file creation.
 * The algorithm keeps up to
 * 100 spare I nodes in the
 * super block. When this runs out,
 * a linear search through the
 * I list is instituted to pick
 * up 100 more.
 */
struct inode *ialloc(int dev)
{
    struct filsys *fp;
    struct buf *bp;
    struct inode *ip;
    int i, j, k, ino;
    int *wp;

    fp = getfs(dev);
    while(fp->s_ilock)
        sleep(&fp->s_ilock, PINOD);
loop:
    if(fp->s_ninode > 0) {
        ino = fp->s_inode[--fp->s_ninode];
        ip = iget(dev, ino);
        if (ip==NULL)
            return(NULL);
        if (ip->i_mode == 0) {
            for(wp = &ip->i_mode; wp < &ip->i_addr[8];)
                *wp++ = 0;
            fp->s_fmod = 1;
            return(ip);
        }
        /*
         * Inode was allocated after all.
         * Look some more.
         */
        iput(ip);
        goto loop;
    }
    fp->s_ilock++;
    ino = 0;
    for(i=0; i<fp->s_isize; i++) {
        bp = bread(dev, i+2);
        wp = bp->b_addr;
        for(j=0; j<256; j+=16) {
            ino++;
            if(wp[j] != 0)
                continue;
            for(k=0; k<NINODE; k++)
            if(dev==inode[k].i_dev && ino==inode[k].i_number)
                goto cont;
            fp->s_inode[fp->s_ninode++] = ino;
            if(fp->s_ninode >= 100)
                break;
        cont:;
        }
        brelse(bp);
        if(fp->s_ninode >= 100)
            break;
    }
    fp->s_ilock = 0;
    wakeup(&fp->s_ilock);
    if (fp->s_ninode > 0)
        goto loop;
    prdev("Out of inodes", dev);
    u.u_error = ENOSPC;
    return(NULL);
}

/*
 * Free the specified I node
 * on the specified device.
 * The algorithm stores up
 * to 100 I nodes in the super
 * block and throws away any more.
 */
void ifree(int dev, int ino)
{
    struct filsys *fp;

    fp = getfs(dev);
    if(fp->s_ilock)
        return;
    if(fp->s_ninode >= 100)
        return;
    fp->s_inode[fp->s_ninode++] = ino;
    fp->s_fmod = 1;
}

/*
 * getfs maps a device number into
 * a pointer to the incore super
 * block.
 * The algorithm is a linear
 * search through the mount table.
 * A consistency check of the
 * in core free-block and i-node
 * counts.
 *
 * bad count on dev x/y -- the count
 *  check failed. At this point, all
 *  the counts are zeroed which will
 *  almost certainly lead to "no space"
 *  diagnostic
 * panic: no fs -- the device is not mounted.
 *  this "cannot happen"
 */
struct filsys *getfs(int dev)
{
    struct mount *p;
    struct filsys *fp;
    uint n1, n2;

    for(p = &mount[0]; p < &mount[NMOUNT]; p++)
    if(p->m_bufp != NULL && p->m_dev == dev) {
        fp = p->m_bufp->b_addr;
        n1 = fp->s_nfree;
        n2 = fp->s_ninode;
        if(n1 > 100 || n2 > 100) {
            prdev("bad count", dev);
            fp->s_nfree = 0;
            fp->s_ninode = 0;
        }
        return fp;
    }
    panic("no fs");
    return 0;
}

/*
 * update is the internal name of
 * 'sync'. It goes through the disk
 * queues to initiate sandbagged IO;
 * goes through the I nodes to write
 * modified nodes; and it goes through
 * the mount table to initiate modified
 * super blocks.
 */
void update(void)
{
    struct inode *ip;
    struct mount *mp;
    struct buf *bp;
    struct filsys *fp;

    if(updlock)
        return;
    updlock++;
    for(mp = &mount[0]; mp < &mount[NMOUNT]; mp++)
        if(mp->m_bufp != NULL) {
            fp = mp->m_bufp->b_addr;
            if(fp->s_fmod==0 || fp->s_ilock!=0 ||
                    fp->s_flock!=0 || fp->s_ronly!=0)
                continue;
            bp = getblk(mp->m_dev, 1);
            fp->s_fmod = 0;
            fp->s_time[0] = time[0];
            fp->s_time[1] = time[1];
            bcopy(fp, bp->b_addr, 256);
            bwrite(bp);
        }
    for(ip = &inode[0]; ip < &inode[NINODE]; ip++)
        if((ip->i_flag&ILOCK) == 0) {
            ip->i_flag |= ILOCK;
            iupdat(ip, time);
            prele(ip);
        }
    updlock = 0;
    bflush(NODEV);
}
