#include "os.h"

struct buf buf[NBUF];
struct buf bfreelist;
int rablock;
int nblkdev;

/*
 * This is the set of buffers proper, whose heads
 * were declared in buf.h.  There can exist buffer
 * headers not pointing here that are used purely
 * as arguments to the I/O routines to describe
 * I/O to be done-- e.g. swbuf, just below, for
 * swapping.
 */
char buffers[NBUF][514];
struct buf swbuf;

/*
 * Declarations of the tables for the magtape devices;
 * see bdwrite.
 */
int tmtab;
int httab;

/*
 * The following several routines allocate and free
 * buffers with various side effects.  In general the
 * arguments to an allocate routine are a device and
 * a block number, and the value is a pointer to
 * to the buffer header; the buffer is marked "busy"
 * so that no on else can touch it.  If the block was
 * already in core, no I/O need be done; if it is
 * already busy, the process waits until it becomes free.
 * The following routines allocate a buffer:
 *  getblk
 *  bread
 *  breada
 * Eventually the buffer must be released, possibly with the
 * side effect of writing it out, by using one of
 *  bwrite
 *  bdwrite
 *  bawrite
 *  brelse
 */

/*
 * Read in (if necessary) the block and return a buffer pointer.
 */
struct buf* bread(int dev, int blkno)
{
    register struct buf *rbp;

    rbp = getblk(dev, blkno);
    if (rbp->b_flags&B_DONE)
        return(rbp);
    rbp->b_flags |= B_READ;
    rbp->b_wcount = -256;
    (*bdevsw[major(dev)].d_strategy)(rbp);
    iowait(rbp);
    return(rbp);
}

/*
 * Read in the block, like bread, but also start I/O on the
 * read-ahead block (which is not allocated to the caller)
 */
struct buf* breada(int adev, int blkno, int rablkno)
{
    register struct buf *rbp, *rabp;
    register int dev;

    dev = adev;
    rbp = 0;
    if (!incore(dev, blkno)) {
        rbp = getblk(dev, blkno);
        if ((rbp->b_flags&B_DONE) == 0) {
            rbp->b_flags |= B_READ;
            rbp->b_wcount = -256;
            (*bdevsw[major(dev)].d_strategy)(rbp);
        }
    }
    if (rablkno && !incore(dev, rablkno)) {
        rabp = getblk(dev, rablkno);
        if (rabp->b_flags & B_DONE)
            brelse(rabp);
        else {
            rabp->b_flags |= B_READ|B_ASYNC;
            rabp->b_wcount = -256;
            (*bdevsw[major(dev)].d_strategy)(rabp);
        }
    }
    if (rbp==0)
        return(bread(dev, blkno));
    iowait(rbp);
    return(rbp);
}

/*
 * Write the buffer, waiting for completion.
 * Then release the buffer.
 */
void bwrite(struct buf *bp)
{
    register struct buf *rbp;
    register int flag;

    rbp = bp;
    flag = rbp->b_flags;
    rbp->b_flags &= ~(B_READ | B_DONE | B_ERROR | B_DELWRI);
    rbp->b_wcount = -256;
    (*bdevsw[major(rbp->b_dev)].d_strategy)(rbp);
    if ((flag&B_ASYNC) == 0) {
        iowait(rbp);
        brelse(rbp);
    } else if ((flag&B_DELWRI)==0)
        geterror(rbp);
}

/*
 * Release the buffer, marking it so that if it is grabbed
 * for another purpose it will be written out before being
 * given up (e.g. when writing a partial block where it is
 * assumed that another write for the same block will soon follow).
 * This can't be done for magtape, since writes must be done
 * in the same order as requested.
 */
void bdwrite(struct buf *bp)
{
    register struct buf *rbp;
    register struct devtab *dp;

    rbp = bp;
    dp = bdevsw[major(rbp->b_dev)].d_tab;
    if (dp == &tmtab || dp == &httab)
        bawrite(rbp);
    else {
        rbp->b_flags |= B_DELWRI | B_DONE;
        brelse(rbp);
    }
}

/*
 * Release the buffer, start I/O on it, but don't wait for completion.
 */
void bawrite(struct buf *bp)
{
    register struct buf *rbp;

    rbp = bp;
    rbp->b_flags |= B_ASYNC;
    bwrite(rbp);
}

/*
 * release the buffer, with no I/O implied.
 */
void brelse(struct buf *bp)
{
    register struct buf *rbp, **backp;
    int sps;

    rbp = bp;
    if (rbp->b_flags&B_WANTED)
        wakeup(rbp);
    if (bfreelist.b_flags&B_WANTED) {
        bfreelist.b_flags &= ~B_WANTED;
        wakeup(&bfreelist);
    }
    if (rbp->b_flags&B_ERROR)
        rbp->b_dev |= 0xff;   /* no assoc. on error */
    backp = &bfreelist.av_back;
    sps = getps();
    spl6();
    rbp->b_flags &= ~(B_WANTED|B_BUSY|B_ASYNC);
    (*backp)->av_forw = rbp;
    rbp->av_back = *backp;
    *backp = rbp;
    rbp->av_forw = &bfreelist;
    setps(sps);
}

/*
 * See if the block is associated with some buffer
 * (mainly to avoid getting hung up on a wait in breada)
 */
struct buf* incore(int adev, int blkno)
{
    register int dev;
    register struct buf *bp;
    register struct devtab *dp;

    dev = adev;
    dp = bdevsw[major(adev)].d_tab;
    for (bp=dp->b_forw; bp != dp; bp = bp->b_forw)
        if (bp->b_blkno==blkno && bp->b_dev==dev)
            return(bp);
    return(0);
}

/*
 * Assign a buffer for the given block.  If the appropriate
 * block is already associated, return it; otherwise search
 * for the oldest non-busy buffer and reassign it.
 * When a 512-byte area is wanted for some random reason
 * (e.g. during exec, for the user arglist) getblk can be called
 * with device NODEV to avoid unwanted associativity.
 */
struct buf* getblk(int dev, int blkno)
{
    register struct buf *bp;
    register struct devtab *dp;
    extern lbolt;

    if(major(dev)>=nblkdev)
        panic("blkdev");
    loop:
    if (dev < 0)
        dp = &bfreelist;
    else {
        dp = bdevsw[major(dev)].d_tab;
        if(dp == NULL)
            panic("devtab");
        for (bp=dp->b_forw; bp != dp; bp = bp->b_forw) {
            if (bp->b_blkno!=blkno || bp->b_dev!=dev)
                continue;
            spl6();
            if (bp->b_flags&B_BUSY) {
                bp->b_flags |= B_WANTED;
                sleep(bp, PRIBIO);
                spl0();
                goto loop;
            }
            spl0();
            notavail(bp);
            return(bp);
        }
    }
    spl6();
    if (bfreelist.av_forw == &bfreelist) {
        bfreelist.b_flags |= B_WANTED;
        sleep(&bfreelist, PRIBIO);
        spl0();
        goto loop;
    }
    spl0();
    notavail(bp = bfreelist.av_forw);
    if (bp->b_flags & B_DELWRI) {
        bp->b_flags |= B_ASYNC;
        bwrite(bp);
        goto loop;
    }
    bp->b_flags = B_BUSY | B_RELOC;
    bp->b_back->b_forw = bp->b_forw;
    bp->b_forw->b_back = bp->b_back;
    bp->b_forw = dp->b_forw;
    bp->b_back = dp;
    dp->b_forw->b_back = bp;
    dp->b_forw = bp;
    bp->b_dev = dev;
    bp->b_blkno = blkno;
    return(bp);
}

/*
 * Wait for I/O completion on the buffer; return errors
 * to the user.
 */
void iowait(struct buf *bp)
{
    register struct buf *rbp;

    rbp = bp;
    spl6();
    while ((rbp->b_flags&B_DONE)==0)
        sleep(rbp, PRIBIO);
    spl0();
    geterror(rbp);
}

/*
 * Unlink a buffer from the available list and mark it busy.
 * (internal interface)
 */
void notavail(struct buf *bp)
{
    register struct buf *rbp;
    int sps;

    rbp = bp;
    sps = getps();
    spl6();
    rbp->av_back->av_forw = rbp->av_forw;
    rbp->av_forw->av_back = rbp->av_back;
    rbp->b_flags |= B_BUSY;
    setps(sps);
}

/*
 * Mark I/O complete on a buffer, release it if I/O is asynchronous,
 * and wake up anyone waiting for it.
 */
void iodone(struct buf *bp)
{
    register struct buf *rbp;

    rbp = bp;
    if(rbp->b_flags&B_MAP)
        mapfree(rbp);
    rbp->b_flags |= B_DONE;
    if (rbp->b_flags&B_ASYNC)
        brelse(rbp);
    else {
        rbp->b_flags &= ~B_WANTED;
        wakeup(rbp);
    }
}

/*
 * Zero the core associated with a buffer.
 */
void clrbuf(struct buf *bp)
{
    register int *p;
    register int c;

    p = bp->b_addr;
    c = 256;
    do
        *p++ = 0;
    while (--c);
}

/*
 * Initialize the buffer I/O system by freeing
 * all buffers and setting all device buffer lists to empty.
 */
void binit()
{
    register struct buf *bp;
    register struct devtab *dp;
    register int i;
    struct bdevsw *bdp;

    bfreelist.b_forw = bfreelist.b_back =
        bfreelist.av_forw = bfreelist.av_back = &bfreelist;
    for (i=0; i<NBUF; i++) {
        bp = &buf[i];
        bp->b_dev = -1;
        bp->b_addr = buffers[i];
        bp->b_back = &bfreelist;
        bp->b_forw = bfreelist.b_forw;
        bfreelist.b_forw->b_back = bp;
        bfreelist.b_forw = bp;
        bp->b_flags = B_BUSY;
        brelse(bp);
    }
    i = 0;
    for (bdp = bdevsw; bdp->d_open; bdp++) {
        dp = bdp->d_tab;
        if(dp) {
            dp->b_forw = dp;
            dp->b_back = dp;
        }
        i++;
    }
    nblkdev = i;
}

/* Device start routine for disks
 * and other devices that have the register
 * layout of the older DEC controllers (RF, RK, RP, TM)
 */
void devstart_rk(struct buf *bp, int devloc, int devblk, int hbcom)
{
    /* DEC controllers specific */
}

void mapalloc(struct buf *bp)
{
    /* PDP 11/70 specific */
    (void)bp;
}

void mapfree(struct buf *bp)
{
    /* PDP 11/70 specific */
    (void)bp;
}

/*
 * swap I/O
 */
int swap(int blkno, int coreaddr, int count, int rdflg)
{
    register int *fp;

    fp = &swbuf.b_flags;
    spl6();
    while (*fp&B_BUSY) {
        *fp |= B_WANTED;
        sleep(fp, PSWP);
    }
    *fp = B_BUSY | B_PHYS | rdflg;
    swbuf.b_dev = swapdev;
    swbuf.b_wcount = count;
    swbuf.b_blkno = blkno;
    swbuf.b_addr = 0;
    swbuf.b_xmem = coreaddr;    /* page number */
    (*bdevsw[swapdev>>8].d_strategy)(&swbuf);
    spl6();
    while((*fp&B_DONE)==0)
        sleep(fp, PSWP);
    if (*fp&B_WANTED)
        wakeup(fp);
    spl0();
    *fp &= ~(B_BUSY|B_WANTED);
    return(*fp&B_ERROR);
}

/*
 * make sure all write-behind blocks
 * on dev (or NODEV for all)
 * are flushed out.
 * (from umount and update)
 */
void bflush(int dev)
{
    register struct buf *bp;

loop:
    spl6();
    for (bp = bfreelist.av_forw; bp != &bfreelist; bp = bp->av_forw) {
        if (bp->b_flags&B_DELWRI && (dev == NODEV||dev==bp->b_dev)) {
            bp->b_flags |= B_ASYNC;
            notavail(bp);
            bwrite(bp);
            goto loop;
        }
    }
    spl0();
}

/*
 * Raw I/O. The arguments are
 *      The strategy routine for the device
 *      A buffer, which will always be a special buffer
 *        header owned exclusively by the device for this purpose
 *      The device number
 *      Read/write flag
 * Essentially all the work is computing physical addresses and
 * validating them.
 */
void physio(void (*strat)(struct buf *), struct buf *abp, int dev, int rw)
{
    register struct buf *bp;
    register struct proc *pp;
    uint base, off, seg;
    int page, nsect;

    bp = abp;
    base = (uint)u.u_base;
    /*
     * The IDE backend moves whole 512-byte sectors, so transfer as many full
     * sectors as u_count allows; a sub-sector tail is left in u_count and
     * reported to the caller (via rdwr) as "not transferred".
     */
    nsect = u.u_count >> 9;
    if(nsect == 0)
        return;
    /*
     * Check odd base, odd count, and address wraparound.  Word PIO needs an
     * even base and count; V6 rejected the same cases.
     */
    if((base&01) || (u.u_count&01) || base >= base + u.u_count)
        goto bad;
    pp = u.u_procp;
    /*
     * Locate the user buffer's ABSOLUTE (identity-mapped) physical page.  V6
     * simulated the PDP-11 segmentation registers here; the port instead reads
     * the compact block layout [u][data][stack] at p_addr.  The address must
     * be identity-mapped, NOT reached through the per-process WIN_DATA window,
     * because the transfer finishes at interrupt time when another process may
     * own that window -- the same reason swap() addresses core by page number.
     */
    /* separated I&D: data in the low window slots, stack in the high ones */
    if(base + u.u_count <= (uint)pp->p_dsize << 12)
        page = pp->p_addr + 1 + (base >> 12);
    else if(base >= (uint)(UDPAGES - pp->p_ssize) << 12)
        page = pp->p_addr + 1 + pp->p_dsize +
               ((base >> 12) - (UDPAGES - pp->p_ssize));
    else
        goto bad;       /* not wholly in the data nor wholly in the stack */
    off = base & (PAGESIZ-1);
    seg = (uint)page * (PAGESIZ/16) + (off >> 4);
    off &= 017;

    spl6();
    while (bp->b_flags&B_BUSY) {
        bp->b_flags |= B_WANTED;
        sleep(bp, PRIBIO);
    }
    bp->b_flags = B_BUSY | B_PHYS | rw;
    bp->b_dev = dev;
    bp->b_xmem = (void *)seg;
    bp->b_addr = (void *)off;
    bp->b_blkno = lshift(u.u_offset, -9);
    bp->b_wcount = nsect;
    bp->b_error = 0;
    pp->p_flag |= SLOCK;
    (*strat)(bp);
    spl6();
    while ((bp->b_flags&B_DONE) == 0)
        sleep(bp, PRIBIO);
    pp->p_flag &= ~SLOCK;
    if (bp->b_flags&B_WANTED)
        wakeup(bp);
    spl0();
    bp->b_flags &= ~(B_BUSY|B_WANTED);
    if ((bp->b_flags&B_ERROR) == 0)
        u.u_count -= (uint)nsect << 9;
    geterror(bp);
    return;

bad:
    u.u_error = EFAULT;
}

/*
 * Pick up the device's error number and pass it to the user;
 * if there is an error but the number is 0 set a generalized
 * code.  Actually the latter is always true because devices
 * don't yet return specific errors.
 */
void geterror(struct buf *abp)
{
    register struct buf *bp;

    bp = abp;
    if (bp->b_flags&B_ERROR)
        if ((u.u_error = bp->b_error)==0)
            u.u_error = EIO;
}
