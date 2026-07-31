typedef int label_t[14];

/*
 * The user structure.
 * One allocated per process.
 * Contains all per process data
 * that doesn't need to be referenced
 * while the process is swapped.
 * The user block is USIZE*64 bytes
 * long; resides at virtual kernel
 * loc 140000; contains the system
 * stack per user; is cross referenced
 * with the proc structure for the
 * same process.
 */
struct user
{
    label_t u_rsav;         /* save info when exchanging stacks */
    char    u_segflg;       /* flag for IO; user or kernel space */
    char    u_error;        /* return error code */
    char    u_uid;          /* effective user id */
    char    u_gid;          /* effective group id */
    char    u_ruid;         /* real user id */
    char    u_rgid;         /* real group id */
    struct proc *u_procp;   /* pointer to proc structure */
    char    *u_base;        /* IO base */
    uint    u_count;        /* bytes remaining for IO */
    int     u_offset[2];    /* offset in file for IO */
    struct inode *u_cdir;   /* cwd inode */
    char    u_dbuf[DIRSIZ]; /* path buffer */
    char    *u_dirp;        /* current dir entry */
    struct {
        int u_ino;
        char u_name[DIRSIZ];
    }u_dent;                /* dir entry for namei */
    struct inode *u_pdir;
    int u_ofile[NOFILE];    /* pointers to file structures of open files */
    int u_arg[5];           /* arguments to current system call */
    label_t u_qsav;         /* label variable for quits and interrupts */
    label_t u_ssav;         /* label variable for swapping */
    int u_signal[NSIG];     /* disposition of signals */
    int u_utime;            /* this process user time */
    int u_stime;            /* this process system time */
    int u_cutime[2];        /* sum of childs' utimes */
    int u_cstime[2];        /* sum of childs' stimes */
    int u_ar0[4];           /* users saved register R0 - R3 */
    char u_intflg;          /* catch intr from sys */
                            /* kernel stack per user: extends from
                             * u + PAGESIZ (0xE000) backward, not to reach here */
};

/* The u-area of the current process is a VMM paging WINDOW at a fixed near
 * aperture (offset UADDR=0xD000 = linear page 0x1D in the kernel segment), not
 * a linker-placed global.  The VMM remaps that page to the running process's
 * u page UPAGE(p) on each context switch (retu -> sureg -> HVC_SUREG); UPAGE is
 * block slot 0 (= p_addr) for an EXE process and p_addr+USIZE-1 for a single-
 * seg one; no bytes are copied.  The struct holds only the u data fields; the
 * kernel stack occupies the rest of the page, growing down from UADDR+0x1000
 * (0xE000).
 * STARTX maps the window to proc[0]'s u page, then zeroes it, at boot. */
#define UADDR 0xD000
#define u (*(struct user *)UADDR)

/* Top two words of the kernel stack = the entry (return) SS:SP for the current
 * kernel entry.  SwitchToKernelStack pushes them at the page top;
 * exec/segflt/psig rewrite them to redirect the trap return.  Fixed apertures,
 * independent of sizeof(u) (they replace the old u_stack[KSSIZE-1:-2]). */
#define uret_ss (*(int *)(UADDR + 0x1000 - 2))   /* 0xDFFE */
#define uret_sp (*(int *)(UADDR + 0x1000 - 4))   /* 0xDFFC */

/* u_error codes */
#define EFAULT  106
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENXIO   6
#define E2BIG   7
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define ENOTBLK 15
#define EBUSY   16
#define EEXIST  17
#define EXDEV   18
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define ENOTTY  25
#define ETXTBSY 26
#define EFBIG   27
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EMLINK  31
#define EPIPE   32

