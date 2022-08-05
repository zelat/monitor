#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define SCRIPT_SYSCTL   "sysctl -p"
#define DP_MNT_SHM_NAME "/dp_mnt.shm"
#define MAX_DP_THREADS 4
#define DP_MISS_HB_MAX 60

enum {
    PROC_CTRL = 0,
    PROC_SCANNER,
    PROC_DP,
    PROC_AGENT,
    PROC_SCANNER_STANDALONE,
    PROC_MAX,
};

typedef struct proc_info_ {
    char name[32];                // 进程名
    char path[64];                // 进程路径
    int active  : 1,              // 进程状态
    running : 1;                  // 进程是否在运行状态
    pid_t pid;                    // pid号
    int short_live_count;         // 进程存活时间
    struct timeval start;
    int exit_status;              // 退出状态
} proc_info_t;

typedef struct dp_mnt_shm_ {
    uint32_t dp_hb[MAX_DP_THREADS];    // dp心跳计数器
    bool dp_active[MAX_DP_THREADS];    // dp状态
} dp_mnt_shm_t;

static uint32_t g_dp_last_hb[MAX_DP_THREADS], g_dp_miss_hb[MAX_DP_THREADS];
static volatile sig_atomic_t g_exit_signal = 0;
static dp_mnt_shm_t *g_shm;

static proc_info_t g_procs[PROC_MAX] = {
        [PROC_CTRL] = {"ctrl", "/usr/local/bin/controller", },
        [PROC_SCANNER] = {"scanner", "/usr/local/bin/scanner", },
        [PROC_DP] = {"ardp", "/root/ardp/cmake-build-debug-dev/ardp",},
        [PROC_AGENT] = {"agent", "/usr/local/bin/agent", },
        [PROC_SCANNER_STANDALONE] = {"scanner", "/usr/local/bin/scanner", },
};

// 创建进程间共享文件，用于heartbeat
template<typename T>
T *create_shm(size_t size) {
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

    return static_cast<dp_mnt_shm_t *>(ptr);
}

static void exit_handler(int sig){
    g_exit_signal = 1;
}

static void proc_exit_handler(int signal)
{
    int i, status, exit_status;
    pid_t pid;

    /* Wait for a child process to exit */
    while (1) {
        // waitpid() can be called in signal handler
        pid = waitpid(WAIT_ANY, &status, WNOHANG);
        if (pid <= 0) {
            return;
        }

        if (WIFEXITED(status)) {
            exit_status = WEXITSTATUS(status);
        } else {
            exit_status = -1;
        }

        for (i = 0; i < PROC_MAX; i ++) {
            if (pid != g_procs[i].pid) {
                continue;
            }

            g_procs[i].exit_status = exit_status;
            g_procs[i].running = false;
        }
    }
}

static void dp_stop_handler(int signal)
{
    g_procs[PROC_DP].active = false;
}

static void dp_start_handler(int signal)
{
    g_procs[PROC_DP].active = true;
}

static int exit_monitor(void)
{
    int ret = 0;

    g_procs[PROC_CTRL].active = false;
    g_procs[PROC_SCANNER].active = false;
    g_procs[PROC_DP].active = false;
    g_procs[PROC_AGENT].active = false;
    g_procs[PROC_SCANNER_STANDALONE].active = false;

    signal(SIGCHLD, SIG_DFL);

    printf("Clean up.\n");

    munmap(g_shm, sizeof(dp_mnt_shm_t));
    return ret;
}


static void stop_proc(int i, int sig, int wait)
{
    if (g_procs[i].pid > 0) {
        printf("Kill %s with signal %d, pid=%d\n", g_procs[i].name, sig, g_procs[i].pid);
        kill(g_procs[i].pid, sig);

        int pid, status;
        while (wait) {
            pid = waitpid(WAIT_ANY, &status, WNOHANG);
            if (pid == g_procs[i].pid) {
                g_procs[i].running = false;
                g_procs[i].pid = 0;
                printf("%s stopped.\n", g_procs[i].name);
                break;
            }
        }
    }
}

static void check_hearbeat(){
    int i;
    if (!g_procs[PROC_DP].active){
        return;
    }
    for (i = 0; i < MAX_DP_THREADS; ++i) {
        printf("dp_hb = %u\n", g_shm->dp_hb[i]);
        if (!g_shm->dp_active[i]) {
            continue;
        }

        if (g_shm->dp_hb[i] != g_dp_last_hb[i]) {
            g_dp_last_hb[i] = g_shm->dp_hb[i];
            g_dp_miss_hb[i] = 0;
            continue;
        }

        g_dp_miss_hb[i] ++;
        // Suppress log for timer drifting. Only print when count is large than 1.
        if (g_dp_miss_hb[i] > 1) {
            printf("dp%d heartbeat miss count=%u hb=%u\n", i, g_dp_miss_hb[i], g_dp_last_hb[i]);
        }
        if (g_dp_miss_hb[i] > DP_MISS_HB_MAX) {
            printf("kill dp for heartbeat miss.\n");
            stop_proc(PROC_DP, SIGSEGV, false);

            g_dp_miss_hb[i] = 0;
        }
    }
}

int main() {
    int i, ret;
    struct timeval tmo;
    fd_set read_fds;

    signal(SIGTERM, exit_handler);
    signal(SIGBUS, exit_handler);
    signal(SIGINT, exit_handler);
    signal(SIGQUIT, exit_handler);
    signal(SIGCHLD, proc_exit_handler);
    signal(40, dp_stop_handler);
    signal(41, dp_start_handler);
    //创建共享内存
    g_shm = create_shm<dp_mnt_shm_t>(sizeof(dp_mnt_shm_t));
    if (g_shm == NULL) {
        printf("Unable to create shared memory. Exit!\n");
        return -1;
    }

    printf("pid = %d", getpid());

    for (i = 0; i < MAX_DP_THREADS; i ++) {
        g_dp_last_hb[i] = g_dp_miss_hb[i] = g_shm->dp_hb[i] = 0;
    }

    ret = system(SCRIPT_SYSCTL);
    g_procs[PROC_DP].active = true;

    while (1) {
        if (g_exit_signal == 1) {
            ret = exit_monitor();
            printf("monitor exit[%d]", ret);
            sleep(3);       // wait for consul exit
            exit(0);
        }

        //stop/start process
        tmo.tv_sec = 1;
        tmo.tv_usec = 0;

        FD_ZERO(&read_fds);
        ret = select(0, &read_fds, NULL, NULL, &tmo);

        if (ret == 0) {
            check_hearbeat();
        }
    }
    return 0;
}
