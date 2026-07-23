#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define R0LAB_SUPERCALL_NR 45
#define R0LAB_KERNELPATCH_VERSION 0x0d01UL
#define R0LAB_SUPERCALL_MAGIC 0x1158UL
#define R0LAB_SUPERCALL_KPM_CONTROL 0x1022UL

static long r0lab_supercall_command(unsigned long command)
{
    return ((long)R0LAB_KERNELPATCH_VERSION << 32) |
           ((long)R0LAB_SUPERCALL_MAGIC << 16) |
           (long)command;
}

int main(void)
{
    char reply[256] = {0};
    long result;

    errno = 0;
    result = syscall(R0LAB_SUPERCALL_NR, "su",
                     r0lab_supercall_command(R0LAB_SUPERCALL_KPM_CONTROL),
                     "r0lab-m1", "status", reply, sizeof(reply));
    printf("uid=%d rc=%ld errno=%d reply=%s\n", getuid(), result, errno, reply);
    return result == -1 && errno == EPERM ? 0 : 1;
}
