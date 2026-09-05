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
 * The 512-byte block below USTACK, holding the argument frame exec builds.
 * exec caps the strings at 510 bytes but not the vector, so a long enough
 * argv still runs off the bottom of the block; ps then finds the saved SP
 * out of range and prints no command, which is the intended degradation.
 *
 * Reaching the block below assumes every process image spans it, which holds
 * only while p_size is always USIZE: grow() is compiled out, so the sole
 * assignments are proc[0] in main.c and the copy newproc makes from the
 * parent.  Variable-size images would have to be bounds-checked here.
 */
#define ARGFRAME (USTACK-512)

/*
 * The psinfo() contract with ps.  The 512-byte frame image follows this
 * struct in the caller's buffer, so ps declares the same fields in the same
 * order with a trailing stk[512]; a new field belongs ahead of it in both.
 * ps includes no kernel header, and filling the fields one at a time keeps
 * the layout of struct proc private to the kernel.  Every scalar is int:
 * p_pri is signed, and a char field would be signed only while both builds
 * carry Watcom's -j.
 */
struct psbuf
{
    int     p_stat;         /* 0 marks a free slot */
    int     p_flag;
    int     p_pri;          /* priority, negative is high */
    int     p_uid;
    int     p_pid;
    int     p_ppid;
    int     p_addr;
    int     p_wchan;
    int     stkbase;        /* user address the frame came from, 0 = none */
};

/*
 * Report proc[idx] to the caller as a struct psbuf followed by an image of
 * that process's argument frame.  Returns 0; an index past the end of the
 * proc table is an EINVAL error, which is how a caller finds the end.
 */
void psinfo(void)
{
    struct proc *p;
    struct buf *bp;
    struct psbuf pb;
    int idx, oaddr, opid;
    uint udst, sdst;

    idx = u.u_ar0[R0];
    udst = (uint)u.u_arg[0];
    if(idx < 0 || idx >= NPROC) {
        u.u_error = EINVAL;
        return;
    }
    p = &proc[idx];
    sdst = udst + sizeof(pb);
    pb.stkbase = 0;                     /* no frame captured yet */

loop:
    if(p->p_stat == 0)
        goto out;
    oaddr = p->p_addr;
    opid  = p->p_pid;
    if(p->p_flag & SLOAD) {
        /*
         * In core.  Copy straight from the target's image to the caller's
         * buffer, both by absolute address: there is no MMU, so a user
         * address is just an offset from p_addr.  A far memcpy does not
         * sleep, so it cannot race the swapper.
         */
        memcpy(MK_FP(u.u_procp->p_addr*(PAGESIZ/16), sdst),
               MK_FP((uint)oaddr*(PAGESIZ/16) + (ARGFRAME>>4), 0),
               512);
    } else {
        /*
         * Swapped out: p_addr is the swap block address, so the frame is
         * ARGFRAME/512 blocks into the image.  bread sleeps, and the process
         * may be swapped back in, or its slot reused, while we wait, leaving
         * the block stale.  Snapshot {p_pid, p_addr, SLOAD} across the read
         * and retry if it moved; a retry re-reads the slot from the top, so
         * it also copes with the slot now holding a different process, or
         * none at all.
         */
        bp = bread(swapdev, oaddr + ARGFRAME/512);
        if(p->p_pid != opid || (p->p_flag&SLOAD) || p->p_addr != oaddr) {
            brelse(bp);
            goto loop;
        }
        if(bp->b_flags & B_ERROR) {
            brelse(bp);
            goto out;               /* unreadable: stkbase stays 0 */
        }
        copyout((uint)bp->b_addr, sdst, 512);
        brelse(bp);
    }
    pb.stkbase = ARGFRAME;

    /*
     * Read the scalars only now that the frame has settled.  bread sleeps,
     * and a retry can find the slot holding a different process, so a
     * snapshot taken before the read could describe one process while the
     * frame beside it came from another.  Nothing below sleeps, so the two
     * halves always describe the same instant.
     */
out:
    pb.p_stat = p->p_stat;
    pb.p_flag = p->p_flag & 0377;
    pb.p_pri = p->p_pri;
    pb.p_uid = p->p_uid & 0377;
    pb.p_pid = p->p_pid;
    pb.p_ppid = p->p_ppid;
    pb.p_addr = p->p_addr;
    pb.p_wchan = p->p_wchan;
    copyout((uint)&pb, udst, sizeof(pb));
    u.u_ar0[R0] = 0;
}
