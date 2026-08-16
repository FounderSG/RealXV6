#include "os.h"

struct proc proc[NPROC];
struct inode *rootdir;

int core_cs;

int mpid;
char runin;
char runout;
char runrun;
char curpri;
int rootdev = 0;
int swapdev = 0;
int swplo = 4000;
int nswap = 872;
int maxmem;
int updlock = 0;

int execnt;
int lbolt;
int time[2];
int tout[2];
int nchrdev;

struct mount mount[NMOUNT];
struct inode inode[NINODE];
struct text text[NTEXT];
char canonb[CANBSIZ];
int coremap[CMAPSIZ];
int swapmap[SMAPSIZ];
struct callo callout[NCALL];

extern struct devtab rktab;

struct bdevsw bdevsw[] = {
    { nulldev, nulldev, rkstrategy, &rktab },
    { NULL, NULL, NULL, NULL},
};

struct cdevsw cdevsw[] = {
    { klopen, klclose, klread, klwrite, klsgtty },
    { nulldev, nulldev, mmread, mmwrite, mmsgtty },
    { nulldev, nulldev, rkread, rkwrite, (int (*)(int, int *))nulldev },
    { NULL, NULL, NULL, NULL, NULL }
};

/*
 * Icode is the octal bootstrap
 * program executed in user mode
 * to bring up the system.
 *
 * Disassembly:
 * 0100        B8 12 01         MOV AX, 0112h  ; argv
 * 0103        50               PUSH AX
 * 0104        B8 0D 01         MOV AX, 010Dh  ; prog
 * 0107        50               PUSH AX
 * 0108        BA 0B 00         MOV DX, 000Bh  ; sys_exec
 * 010B        CD 81            INT 81h        ; syscall exec(prog, argv)
 * 010D  prog: 69 6E 69 74 00   DB 'init\0'    ; program to exec
 * 0112  argv: 0D 01            DW av          ; argv[] array
 * 0114        00 00            DW 0           ; NULL
 */
char icode[] = {
    0xB8, 0x12, 0x01, 0x50, 0xB8, 0x0D, 0x01, 0x50,
    0xBA, 0x0B, 0x00, 0xCD, 0x81, 0x69, 0x6E, 0x69,
    0x74, 0x00, 0x0D, 0x01, 0x00, 0x00
};

void main()
{
    pc_init();
    segflt_setup();
    privflt_setup();
    nofault_setup();

    maxmem = 128;
    mfree(coremap, maxmem, USPACE);
    mfree(swapmap, nswap, swplo);

    /*
     * set up system process
     */
    proc[0].p_addr = core_cs/(PAGESIZ/16);
    proc[0].p_size = USIZE;
    proc[0].p_stat = SRUN;
    proc[0].p_flag |= SLOAD|SSYS;
    u.u_procp = &proc[0];

    cinit();
    binit();
    iinit();

    rootdir = iget(rootdev, ROOTINO);
    rootdir->i_flag &= ~ILOCK;
    u.u_cdir = iget(rootdev, ROOTINO);
    u.u_cdir->i_flag &= ~ILOCK;

    printf("Unix Ready.\r\n");

    /*
     * make init process
     * enter scheduling loop
     * with system process
     */

    if(newproc()) {
        copyout((uint)icode, 0x100, sizeof(icode));
        move_to_user_mode(u.u_procp->p_addr*(PAGESIZ/16));
        /*
         * Return goes to loc. 0 of user init
         * code just copied out.
         */
        return;
    }
    sched();
}

/*
 * Try out the 3 pseudo text,data,stack segment
 * sizes passed as arguments for possible exceed
 * of max sizes, then load the user segmentation.
 * The argument sep specifies if the text and
 * data+stack segments are to be separated (EXE).
 * Sizes are in pages: x86 paging stands in for
 * the PDP-11 APRs, so where V6 builds the
 * u_uisa/u_uisd prototypes here, this port keeps
 * the equivalent state (p_tsize/p_dsize/p_ssize) in
 * the proc entry and sureg rebuilds the windows from
 * it.  The +1 is the u page in slot 0 of the block.
 */
int estabur(int nt, int nd, int ns, int sep)
{
    if(sep)
        if(nd+ns > UDPAGES)
            goto err;
    if(nt+nd+ns+1 > maxmem)
        goto err;
    sureg(u.u_procp);
    return(0);

err:
    u.u_error = ENOMEM;
    return(-1);
}
