#include "os.h"

/*
 * Swap out process p.
 * The ff flag causes its core to be freed--
 * it may be off when called to create an image for a
 * child process in newproc.
 * Os is the old size of the data area of the process,
 * and is supplied during core expansion swaps.
 *
 * panic: out of swap space
 * panic: swap error -- IO error
 */
void xswap(struct proc *p, int ff, uint a)
{
    register struct proc *rp;
    int os;

    rp = p;
    os = rp->p_size;

    xccdec(rp->p_textp);
    rp->p_flag |= SLOCK;
    if(swap(a, rp->p_addr, os, 0))
        panic("swap error");
    if(ff)
        mfree(coremap, os, rp->p_addr);
    rp->p_addr = a;
    rp->p_flag &= ~(SLOAD|SLOCK);
    rp->p_time = 0;
    if(runout) {
        runout = 0;
        wakeup(&runout);
    }
}

/*
 * relinquish use of the shared text segment
 * of a process.
 */
void xfree(void)
{
    struct text *xp;
    struct inode *ip;

    if((xp=u.u_procp->p_textp) != NULL) {
        u.u_procp->p_textp = NULL;
        xccdec(xp);
        if(--xp->x_count == 0) {
            ip = xp->x_iptr;
            if((ip->i_mode&ISVTX) == 0) {
                xp->x_iptr = NULL;
                mfree(swapmap, xp->x_size*(PAGESIZ/512), xp->x_daddr);
                                /* unit fix vs V6's (x_size+7)/8: x_size is
                                 * pages here, one page = 8 swap sectors */
                ip->i_flag &= ~ITEXT;
                iput(ip);
            }
        }
    }
}

/*
 * Attach to a shared text segment.  If the file's text is already in the
 * table, share it; otherwise claim a free slot, allocate core and read the
 * image in.  tsize is the text byte count (hdr.a_text): V6 takes it from
 * u.u_arg[1], where its exec keeps the header; this exec keeps the header
 * in a local, so it is passed explicitly.
 *
 * V6 reads the text into the caller's own space and bounces it out to the
 * swap device, because PDP-11 text must sit contiguously below data.  Here
 * the block is position independent behind WIN_TEXT and the kernel reaches
 * any page flat, so the image is read straight into the shared block (the
 * shortcut V6's own comment suggests) and then written once to the swap
 * image (x_daddr), from which it is reloaded after the in-core copy is
 * released (xccdec) with sharers still swapped out.
 *
 * On any u.u_error return nothing is attached and the caller's old image
 * is intact (exec relies on this to fail back cleanly).
 *
 * panic: out of text
 * panic: out of swap space
 * panic: swap error
 */
void xalloc(struct inode *ip, uint tsize)
{
    register struct text *xp;
    register struct text *rp;
    int ts, c;
    uint ta;

    if(tsize == 0)
        return;
    rp = NULL;
    for(xp = &text[0]; xp < &text[NTEXT]; xp++)
        if(xp->x_iptr == NULL) {
            if(rp == NULL)
                rp = xp;
        } else
            if(xp->x_iptr == ip) {
                xp->x_count++;
                u.u_procp->p_textp = xp;    /* V6: attach before out:, so
                                             * the no-core coroutine below
                                             * lets sched reload this text */
                goto out;
            }
    if((xp=rp) == NULL)
        panic("out of text");
    ts = tsize / PAGESIZ;
    if(tsize % PAGESIZ)
        ts++;
    if((ta = malloc(coremap, ts)) == NULL) {
        ta = swgrow(ts);            /* V6 reaches this point through expand(),
                                     * whose no-core path is this same
                                     * swap-self-out trick */
        /*
         * The free slot may have been claimed while we were out.  Nobody
         * can have created THIS text meanwhile (ip is locked by the
         * caller), so only a fresh free slot is needed.
         */
        if(xp->x_iptr != NULL) {
            for(xp = &text[0]; xp < &text[NTEXT]; xp++)
                if(xp->x_iptr == NULL)
                    break;
            if(xp >= &text[NTEXT])
                panic("out of text");
        }
    }
    if((xp->x_daddr = malloc(swapmap, ts*(PAGESIZ/512))) == NULL)
        panic("out of swap space");
    xp->x_caddr = ta;
    xp->x_size = ts;
    xp->x_iptr = ip;
    xp->x_count = 1;
    for(c = 0; c < ts; c++)
        clearseg(ta + c);           /* zero the tail of the last page */
    u.u_procp->p_taddr = ta;        /* I-space io (suibyte) targets the block */
    u.u_base = (char *)0;
    u.u_offset[0] = 0;
    u.u_offset[1] = sizeof(struct exec);
    u.u_count = tsize;
    u.u_segflg = 2;
    readi(ip);
    u.u_segflg = 0;
    if(u.u_error) {
        /* I/O error loading the image: tear the entry down rather than
         * leave garbage where later execs would share it (V6 runs the
         * garbage; a shared cache must not). */
        xp->x_iptr = NULL;
        xp->x_count = 0;
        mfree(coremap, ts, ta);
        mfree(swapmap, ts*(PAGESIZ/512), xp->x_daddr);
        u.u_procp->p_taddr = u.u_procp->p_textp != NULL ?
            u.u_procp->p_textp->x_caddr : 0;
        return;
    }
    /* Write the prototype image to the swap device (V6).  SLOCK as V6
     * does: the block itself cannot move, but the process must not be
     * swapped out in the middle of the write. */
    u.u_procp->p_flag |= SLOCK;
    if(swap(xp->x_daddr, xp->x_caddr, xp->x_size, B_WRITE))
        panic("swap error");
    u.u_procp->p_flag &= ~SLOCK;
    ip->i_flag |= ITEXT;
    ip->i_count++;
    xp->x_ccount = 1;               /* creator is in core */
    u.u_procp->p_textp = xp;
    return;

out:
    if(xp->x_ccount == 0) {
        /*
         * Text not in core.  V6 swaps the caller out and lets sched page
         * the text in next to it (PDP-11 contiguity); behind WIN_TEXT the
         * block is position independent, so read it back in place when
         * core is available.  With no core, fall back to the V6
         * coroutine: swap self out with SSWAP; sched reloads the text and
         * takes the in-core reference at swap-in.  (The caller's OLD text
         * keeps its in-core reference while we are out -- conservative,
         * and rebalanced by exec's xfree at the commit point.)
         */
        if((ta = malloc(coremap, xp->x_size)) != NULL) {
            if(swap(xp->x_daddr, ta, xp->x_size, B_READ))
                panic("swap error");
            xp->x_caddr = ta;
        } else {
            if(save(u.u_ssav) == 0) {
                while((ta = malloc(swapmap,
                        u.u_procp->p_size*(PAGESIZ/512))) == NULL)
                    sleep(&swapmap, PSWP);
                xswap(u.u_procp, 1, ta);
                u.u_procp->p_flag |= SSWAP;
                swtch();
                /* no return */
            }
            /* swapped back in: sched loaded the text and took the
             * in-core reference for us */
            u.u_procp->p_taddr = xp->x_caddr;
            return;
        }
    }
    xp->x_ccount++;
    u.u_procp->p_taddr = xp->x_caddr;
}

/*
 * Decrement the in-core usage count of a shared text segment.
 * When it drops to zero, free the core space.
 */
void xccdec(struct text *xp)
{
    register struct text *rp;

    if((rp=xp)!=NULL && rp->x_ccount!=0)
        if(--rp->x_ccount == 0)
            mfree(coremap, rp->x_size, rp->x_caddr);
}
