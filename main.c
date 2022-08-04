#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <stdbool.h>

#define DP_MNT_SHM_NAME "/dp_mnt.shm"
#define MAX_DP_THREADS 4

static uint32_t g_dp_last_hb[MAX_DP_THREADS], g_dp_miss_hb[MAX_DP_THREADS];

typedef struct dp_mnt_shm_ {
    uint32_t dp_hb[MAX_DP_THREADS];
    bool dp_active[MAX_DP_THREADS];
} dp_mnt_shm_t;

static volatile g_exit_signal = 0;
static dp_mnt_shm_t *g_shm;

static void *create_shm(size_t size) {
    int fd;
    void *ptr;

    fd = shm_open(DP_MNT_SHM_NAME, O_CREAT | O_RDWR | O_TRUNC, S_IRWXU | S_IRWXG);
    if (fd < 0) {
        return NULL;
    }

    if (ftruncate(fd, size) != 0) {
        close(fd);
        return NULL;
    }

    ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED || ptr == NULL) {
        close(fd);
        return NULL;
    }

    close(fd);

    return ptr;
}

int main() {
    int i, ret;
    //创建共享内存
    g_shm = create_shm(sizeof(dp_mnt_shm_t));
    if (g_shm == NULL) {
        printf("Unable to create shared memory. Exit!\n");
        return -1;
    }

    printf("pid=%d", getpid());

    for (i = 0; i < MAX_DP_THREADS; i ++) {
        g_dp_last_hb[i] = g_dp_miss_hb[i] = g_shm->dp_hb[i] = 0;
    }

    ret = 0;

    while (1) {
        if (g_exit_signal == 1) {
            //            ret = exit_monitor();
            printf("monitor exit[%d]", ret);
            sleep(3);       // wait for consul exit
            exit(0);
        }
    }
}
