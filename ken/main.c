#include "os.h"

struct user u;
struct proc proc[NPROC];
struct inode *rootdir;

int core_cs;
int core_spl;

int mpid;
char runin;
char runout;
char runrun;
char curpri;
int rootdev = 0;
int swapdev = 0;
int swplo = 4000;
int nswap = 872;
int updlock = 0;

int execnt;
int lbolt;
int time[2];
int tout[2];
int nchrdev;

struct mount mount[NMOUNT];
struct inode inode[NINODE];
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

    mfree(coremap, 128, USPACE);
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

    printk("Unix Ready.\r\n");

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
