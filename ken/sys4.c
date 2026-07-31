/*
 * Everything in this file is a routine implementing a system call.
 */

#include "os.h"

void getswit(void)
{
    u.u_ar0[R0] = getps();
}

void gtime(void)
{
    u.u_ar0[R0] = time[0];
    u.u_ar0[R1] = time[1];
}

void stime(void)
{
    if(suser()) {
        time[0] = u.u_ar0[R0];
        time[1] = u.u_ar0[R1];
        wakeup(tout);
    }
}

void setuid(void)
{
    char uid;

    uid = u.u_ar0[R0] & 0xff;
    if(u.u_ruid == uid || suser()) {
        u.u_uid = uid;
        u.u_procp->p_uid = uid;
        u.u_ruid = uid;
    }
}

void getuid(void)
{
    u.u_ar0[R0] = (u.u_uid << 8) | u.u_ruid;
}

void setgid(void)
{
    char gid;

    gid = u.u_ar0[R0] & 0xff;
    if(u.u_rgid == gid || suser()) {
        u.u_gid = gid;
        u.u_rgid = gid;
    }
}

void getgid(void)
{
    u.u_ar0[R0] = (u.u_gid<<8) + u.u_rgid;
}

void getpid(void)
{
    u.u_ar0[R0] = u.u_procp->p_pid;
}

void sync(void)
{
    update();
}

void nice(void)
{
    int n;

    n = u.u_ar0[R0];
    if(n > 20)
        n = 20;
    if(n < 0 && !suser())
        n = 0;
    u.u_procp->p_nice = n;
}

/*
 * Unlink system call.
 * panic: unlink -- "cannot happen"
 */
void unlink(void)
{
    struct inode *ip, *pp;

    pp = namei(&uchar, 2);
    if(pp == NULL)
        return;
    prele(pp);
    ip = iget(pp->i_dev, u.u_dent.u_ino);
    if(ip == NULL)
        panic("unlink -- iget");
    if((ip->i_mode&IFMT)==IFDIR && !suser())
        goto out;
    u.u_offset[1] -= DIRSIZ+2;
    u.u_base = (char *)&u.u_dent;
    u.u_count = DIRSIZ+2;
    u.u_dent.u_ino = 0;
    writei(pp);
    ip->i_nlink--;
    ip->i_flag |= IUPD;

out:
    iput(pp);
    iput(ip);
}

void chdir(void)
{
    struct inode *ip;

    ip = namei(&uchar, 0);
    if(ip == NULL)
        return;
    if((ip->i_mode&IFMT) != IFDIR) {
        u.u_error = ENOTDIR;
    bad:
        iput(ip);
        return;
    }
    if(access(ip, IEXEC))
        goto bad;
    iput(u.u_cdir);
    u.u_cdir = ip;
    prele(ip);
}

void chmod(void)
{
    struct inode *ip;

    if ((ip = owner()) == NULL)
        return;
    ip->i_mode &= ~07777;
    if (u.u_uid)
        u.u_arg[1] &= ~ISVTX;
    ip->i_mode |= u.u_arg[1]&07777;
    ip->i_flag |= IUPD;
    iput(ip);
}

void chown(void)
{
    struct inode *ip;

    if (!suser() || (ip = owner()) == NULL)
        return;
    ip->i_uid = u.u_arg[1] & 0xff;
    ip->i_gid = u.u_arg[1] >> 8;
    ip->i_flag |= IUPD;
    iput(ip);
}

/*
 * Change modified date of file:
 * time to r0-r1; sys smdate; file
 * This call has been withdrawn because it messes up
 * incremental dumps (pseudo-old files aren't dumped).
 * It works though and you can uncomment it if you like.

smdate()
{
    register struct inode *ip;
    register int *tp;
    int tbuf[2];

    if ((ip = owner()) == NULL)
        return;
    ip->i_flag =| IUPD;
    tp = &tbuf[2];
    *--tp = u.u_ar0[R1];
    *--tp = u.u_ar0[R0];
    iupdat(ip, tp);
    ip->i_flag =& ~IUPD;
    iput(ip);
}
*/

void ssig(void)
{
    int a;

    a = u.u_arg[0];
    if(a<=0 || a>=NSIG || a ==SIGKIL) {
        u.u_error = EINVAL;
        return;
    }
    u.u_ar0[R0] = u.u_signal[a];
    u.u_signal[a] = u.u_arg[1];
    if(u.u_procp->p_sig == a)
        u.u_procp->p_sig = 0;
}

void kill(void)
{
    register struct proc *p, *q;
    int a;
    int f;

    f = 0;
    a = u.u_ar0[R0];
    q = u.u_procp;
    for(p = &proc[0]; p < &proc[NPROC]; p++) {
        if(p == q)
            continue;
        if(a != 0 && p->p_pid != a)
            continue;
        if(a == 0 && (p->p_ttyp != q->p_ttyp || p <= &proc[1]))
            continue;
        if(u.u_uid != 0 && u.u_uid != p->p_uid)
            continue;
        f++;
        psignal(p, u.u_arg[0]);
    }
    if(f == 0)
        u.u_error = ESRCH;
}

void times(void)
{
    int *p;

    for(p = &u.u_utime; p  < &u.u_utime+6;) {
        suword(u.u_arg[0], *p++);
        u.u_arg[0] += 2;
    }
}

#if PDP11
profil()
{
    u.u_prof[0] = u.u_arg[0] & ~1;  /* base of sample buf */
    u.u_prof[1] = u.u_arg[1];   /* size of same */
    u.u_prof[2] = u.u_arg[2];   /* pc offset */
    u.u_prof[3] = (u.u_arg[3]>>1) & 077777; /* pc scale */
}
#endif

/*
 * psinfo - process status query (replaces the old getkaddr peek).
 *
 * Copy proc[index] out to the user buffer, followed by a 512-byte image of the
 * top of that process's user stack (the top 512-byte block), where exec leaves the
 * argument vector.  ps assembles the COMMAND column from that image, so it never
 * reads /dev/mem or /dev/kmem and never needs to know the physical layout: the
 * kernel, which owns the mapping, resolves it here.  This is also what lets the
 * data segment be mapped sparsely -- ps no longer assumes a flat p_addr image.
 *
 * The stack image is only meaningful for separated-I&D (EXE) processes; single-
 * segment images (icode, proc 0/1) carry p_tsize==0 and get only the struct.
 *
 * In core the copy is a non-blocking far memcpy, so it cannot race the swapper.
 * For a swapped-out process the stack block is read from the swap device, which
 * sleeps; the process may be swapped back in (or the slot reused) while we sleep,
 * leaving the block stale.  Snapshot {p_pid,p_addr,SLOAD} across the bread and
 * retry if it moved -- on the retry it is in core and the read is race-free.
 */
void psinfo(void)
{
    struct proc *p;
    struct buf *bp;
    int idx, oaddr, opid, osize;
    uint udst, sdst;

    idx = u.u_ar0[R0];
    udst = (uint)u.u_arg[0];
    if(idx < 0 || idx >= NPROC) {
        u.u_error = EINVAL;
        return;
    }
    p = &proc[idx];
    copyout((uint)p, udst, sizeof(struct proc));
    u.u_ar0[R0] = p->p_stat;
    if(p->p_stat == 0 || p->p_tsize == 0)
        return;                         /* no EXE argument frame */
    sdst = udst + sizeof(struct proc);

loop:
    oaddr = p->p_addr;
    osize = p->p_size;
    opid  = p->p_pid;
    /*
     * The user stack sits at the tail of the (possibly sparse) data block; exec
     * leaves the arg frame in the top 512-byte block of the top stack page,
     * which is the block's last physical page (p_addr + p_size - 1) at page
     * offset PAGESIZ-512 = 0xE00.  (USTACK's top 2 bytes are unused, so this
     * block is 512-aligned and a single swap read suffices.)  This reaches any
     * process (current or not) via the identity map, since core blocks are
     * allocated at physical pages >= USPACE, outside the windows.
     */
    if(p->p_flag & SLOAD) {
        /* In core: read the target's top page by identity map (no sleep, so it
         * cannot race the swapper).  Write into ps's own buffer through ps's
         * data window (user_dseg = WIN_DATA), not a raw p_addr identity offset:
         * the data block now starts at slot 1 (slot 0 = u), so a p_addr-based
         * offset would land one page low, in ps's u-area. */
        memcpy(MK_FP((unsigned)user_dseg, sdst),
               MK_FP((unsigned)(oaddr+osize-1)*(PAGESIZ/16), PAGESIZ-512),
               512);
    } else {
        bp = bread(swapdev, oaddr + (osize-1)*(PAGESIZ/512) + (PAGESIZ/512 - 1));
        if(p->p_pid != opid || (p->p_flag&SLOAD) || p->p_addr != oaddr) {
            brelse(bp);                 /* swapped in while we slept; reread */
            goto loop;
        }
        copyout((uint)bp->b_addr, sdst, 512);
        brelse(bp);
    }
}
