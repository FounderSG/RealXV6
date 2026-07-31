#include "os.h"
/*
 * RK disk driver
 */

#define NRKBLK  4872

struct  devtab  rktab;
struct  buf rrkbuf;

void rkstart(void);
void devstart(struct buf *bp);

void rkstrategy(struct buf *abp)
{
    register struct buf *bp;
    int d;

    bp = abp;
    if(bp->b_flags&B_PHYS)
        mapalloc(bp);
    d = minor(bp->b_dev)-7;
    if(d <= 0)
        d = 1;
    if (bp->b_blkno >= NRKBLK*d) {
        bp->b_flags |= B_ERROR;
        iodone(bp);
        return;
    }
    bp->av_forw = 0;
    spl5();
    if (rktab.d_actf==0)
        rktab.d_actf = bp;
    else
        rktab.d_actl->av_forw = bp;
    rktab.d_actl = bp;
    if (rktab.d_active==0)
        rkstart();
    spl0();
}

/*
 * RK05 cylinder/sector geometry.  Not used in this port: the rk driver is
 * remapped onto IDE, which devstart addresses linearly through ideio, so
 * this V6 disk-address computation is never called.  Kept for reference.
 */
int rkaddr(struct buf *bp)
{
    register struct buf *p;
    register int b;
    int d, m;

    p = bp;
    b = p->b_blkno;
    m = minor(p->b_dev) - 7;
    if(m <= 0)
        d = minor(p->b_dev);
    else {
        d = lrem(b, m);
        b = ldiv(b, m);
    }
    return(d<<13 | (b/12)<<4 | b%12);
}

void rkstart()
{
    register struct buf *bp;

    if ((bp = rktab.d_actf) == 0)
        return;
    rktab.d_active++;
    devstart(bp);
}

void rkintr(void)
{
    register struct buf *bp;

    if (rktab.d_active == 0)
        return;
    bp = rktab.d_actf;
    rktab.d_active = 0;
    rktab.d_errcnt = 0;
    rktab.d_actf = bp->av_forw;
    iodone(bp);
    rkstart();
}

void devstart(struct buf *bp)
{
    unsigned int n;
    void far *p;

    if(bp->b_flags&B_PHYS) {
        if(bp == &rrkbuf) {
            /* raw char I/O: physio put the buffer's absolute (identity) far
             * address in b_xmem:b_addr and a sector count in b_wcount. */
            p = MK_FP((uint)bp->b_xmem, (uint)bp->b_addr);
            n = bp->b_wcount;
        } else {
            /* swap: b_wcount whole pages from page b_xmem, offset 0. */
            p = MK_FP((uint)(bp->b_xmem)*(PAGESIZ/16), 0);
            n = (PAGESIZ/512) * bp->b_wcount;
        }
    } else {
        p = MK_FP(core_cs, bp->b_addr);
        n = 1;
    }

    ideio(bp->b_blkno + NRKBLK * minor(bp->b_dev), n, p, bp->b_flags&B_READ);
}

/*
 * Raw (unbuffered) disk access through the character device.  Both hand a
 * device-owned buffer header (rrkbuf) to physio, which validates the user
 * buffer and drives rkstrategy directly, bypassing the buffer cache.
 */
int rkread(int dev)
{
    physio(rkstrategy, &rrkbuf, dev, B_READ);
    return 0;
}

int rkwrite(int dev)
{
    physio(rkstrategy, &rrkbuf, dev, B_WRITE);
    return 0;
}
