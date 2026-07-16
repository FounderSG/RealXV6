/*
 * tunable variables
 */

#define NBUF    15      /* size of buffer cache */
#define NINODE  100     /* number of in core inodes */
#define NFILE   100     /* number of in core file structures */
#define NMOUNT  5       /* number of mountable file systems */
#define NEXEC   3       /* number of simultaneous exec's */
#define MAXMEM  (64*32) /* max core per process - first # is Kw */
#define SSIZE   20      /* initial stack size (*64 bytes) */
#define SINCR   20      /* increment of stack (*64 bytes) */
#define NOFILE  15      /* max open files per process */
#define CANBSIZ 256     /* max size of typewriter line */
#define CMAPSIZ 100     /* size of core allocation area */
#define SMAPSIZ 100     /* size of swap allocation area */
#define NCALL   20      /* max simultaneous time callouts */
#define NPROC   50      /* max number of processes */
#define NTEXT   40      /* max number of pure texts */
#define NCLIST  100     /* max total clist size */
#define HZ      60      /* Ticks/second of the clock */

/*
 * priorities
 * probably should not be
 * altered too much
 */

#define PSWP    -100
#define PINOD   -90
#define PRIBIO  -50
#define PPIPE   1
#define PWAIT   40
#define PSLEP   90
#define PUSER   100

/*
 * signals
 * dont change
 */

#define NSIG    20
#define SIGHUP  1       /* hangup */
#define SIGINT  2       /* interrupt (rubout) */
#define SIGQIT  3       /* quit (FS) */
#define SIGINS  4       /* illegal instruction */
#define SIGTRC  5       /* trace or breakpoint */
#define SIGIOT  6       /* iot */
#define SIGEMT  7       /* emt */
#define SIGFPT  8       /* floating exception */
#define SIGKIL  9       /* kill */
#define SIGBUS  10      /* bus error */
#define SIGSEG  11      /* segmentation violation */
#define SIGSYS  12      /* sys */
#define SIGPIPE 13      /* end of pipe */

/*
 * fundamental constants
 * cannot be changed
 */

#define USIZE   16      /* page count of user block */
#define PAGESIZ 4096    /* page size */
#define USPACE  0x20    /* start of user space page (windows live in the adapter holes above 640K) */
#define WINSEG  0xA000  /* EXE code-window selector (VMM WIN_TEXT @ linear 0xA0000, VGA-graphics hole) */
#define WDSEG   0xD000  /* EXE data-window selector (VMM WIN_DATA @ linear 0xD0000, option-ROM hole;
                         * unrelated to the kernel's u window at DS:0xD000 = linear 0x1D000) */
#define USTACK  0xF000  /* top of user stack */
#define KSSIZE  380     /* size of kernel stack in words (grew by 15 when the
                         * VMM u_fault save area was removed in stage 5; keeps
                         * sizeof(struct user) == 0x400 so the kernel stack top
                         * stays at U_AREA+1024 = 0xD400) */
#define NULL    0
#define NODEV   (-1)
#define ROOTINO 1       /* i number of all roots */
#define DIRSIZ  14      /* max characters per directory */

/*
 * structure to access an
 * integer in bytes
 */
struct hilo_t
{
    char    lobyte;
    char    hibyte;
};

/*
 * structure to access an integer
 */
struct integ_t
{
    int integ;
};

/*
 * Certain processor registers
 */
#define PS  0177776
#define KL  0177560
#define SW  0177570
