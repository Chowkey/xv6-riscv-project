// user/ps.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/getproc.h"

#define UNUSED    0
#define USED      1
#define SLEEPING  2
#define RUNNABLE  3
#define RUNNING   4
#define ZOMBIE    5

char *states[] = {
    [UNUSED]    "UNUSED  ",
    [USED]      "USED    ",
    [SLEEPING]  "SLEEPING",
    [RUNNABLE]  "RUNNABLE",
    [RUNNING]   "RUNNING ",
    [ZOMBIE]    "ZOMBIE  "
};

int
main(int argc, char *argv[])
{
    struct procinfo procs[64];
    int n = getprocs(procs, 64);

    if(n < 0){
        printf("ps: getprocs failed\n");
        exit(1);
    }

    printf("PID\tSTATE\t\tSIZE\tNAME\n");
    for(int i = 0; i < n; i++){
        printf("%d\t%s\t%ld\t%s\n",
            procs[i].pid,
            states[procs[i].state],
            procs[i].sz,
            procs[i].name);
    }
    exit(0);
}