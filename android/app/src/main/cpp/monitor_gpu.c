#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const volatile uint32_t *rings;
    void *mapping;
    size_t size;
    int fd;
} dm_kgsl_monitor;

#define NS_PER_SECOND 1000000000LL

// KGSL's DEVICE_SHADOW property exposes the kernel's read-only timestamp map.
// The five slots before the final entry cover both ring layouts used by KGSL
// (four priorities, with or without the extra LPAC slot).
static int dm_kgsl_monitor_open(dm_kgsl_monitor *monitor) {
    struct { unsigned long gpuaddr; size_t size; unsigned flags; } shadow = {0};
    struct { unsigned type; void *value; size_t length; } prop = {2, &shadow, sizeof shadow};
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -errno;
    if (ioctl(fd, _IOWR(0x09, 0x02, prop), &prop) < 0) {
        int error = errno;
        close(fd);
        return -error;
    }
    const size_t entry_bytes = 10 * sizeof(uint32_t);
    size_t entries = shadow.size / entry_bytes;
    if (entries < 8) {
        close(fd);
        return -ERANGE;
    }
    void *memory = mmap(NULL, shadow.size, PROT_READ, MAP_SHARED, fd, (off_t)shadow.gpuaddr);
    if (memory == MAP_FAILED) {
        int error = errno;
        close(fd);
        return -error;
    }
    *monitor = (dm_kgsl_monitor){
        .rings = (const volatile uint32_t *)((const char *)memory + (entries - 6) * entry_bytes),
        .mapping = memory, .size = shadow.size, .fd = fd,
    };
    return 0;
}

static void dm_kgsl_monitor_close(dm_kgsl_monitor *monitor) {
    munmap(monitor->mapping, monitor->size);
    close(monitor->fd);
}

static int dm_kgsl_monitor_read(const dm_kgsl_monitor *monitor, uint32_t starts[5]) {
    int busy = 0;
    for (int ring = 0; ring < 5; ++ring) {
        const volatile uint32_t *entry = monitor->rings + 10 * ring;
        starts[ring] = entry[0];
        if (starts[ring] != entry[2]) busy = 1;
    }
    return busy;
}

static _Atomic int64_t g_hud_poll_ns;
static _Atomic int g_gpu_percent = -1;
static _Atomic int g_worker_on;

static int64_t monitor_now(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * NS_PER_SECOND + now.tv_nsec;
}

static int gpu_poll_fresh(void) {
    int64_t poll = atomic_load(&g_hud_poll_ns);
    return poll && monitor_now() - poll < 2500000000LL;
}

static void *sample_gpu(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "dm-gpu-hud");
    dm_kgsl_monitor monitor;
    int error = dm_kgsl_monitor_open(&monitor);
    if (error < 0) {
        atomic_store(&g_gpu_percent, -1);
        atomic_store(&g_worker_on, -1);
        return NULL;
    }
    const struct timespec interval = {0, 1000000};
    uint32_t previous[5] = {0}, starts[5];
    int64_t started = monitor_now(), last = started, busy_ns = 0, counted_ns = 0;
    int moving = 0, verified = 0;
    while (gpu_poll_fresh()) {
        int busy = dm_kgsl_monitor_read(&monitor, starts);
        int64_t now = monitor_now(), span = now - last;
        for (int ring = 0; ring < 5; ++ring) {
            moving |= starts[ring] != previous[ring];
            previous[ring] = starts[ring];
        }
        // A long delay leaves an unobserved interval, so avoid assuming that
        // the endpoint describes everything since the previous sample.
        if (span <= 10000000) {
            counted_ns += span;
            if (busy) busy_ns += span;
        }
        if (now - started >= NS_PER_SECOND) {
            int percent = -1;
            verified |= moving;
            if (verified && (moving || busy_ns == 0) && counted_ns >= NS_PER_SECOND / 2)
                percent = (int)((100 * busy_ns + counted_ns / 2) / counted_ns);
            atomic_store(&g_gpu_percent, percent);
            started = now;
            busy_ns = counted_ns = 0;
            moving = 0;
        }
        last = now;
        nanosleep(&interval, NULL);
    }
    dm_kgsl_monitor_close(&monitor);
    atomic_store(&g_gpu_percent, -1);
    atomic_store(&g_worker_on, 0);
    return NULL;
}

static void gpu_poll(void) {
    atomic_store(&g_hud_poll_ns, monitor_now());
    int idle = 0;
    if (!atomic_compare_exchange_strong(&g_worker_on, &idle, 1)) return;
    pthread_t worker;
    if (pthread_create(&worker, NULL, sample_gpu, NULL) == 0)
        pthread_detach(worker);
    else
        atomic_store(&g_worker_on, -1);
}

JNIEXPORT jint JNICALL
Java_com_nx_android_game_hud_HudSampler_nativeGpuSample(JNIEnv *env, jclass cls, jboolean enabled) {
    (void)env; (void)cls;
    if (!enabled) {
        atomic_store(&g_hud_poll_ns, 0);
        return -1;
    }
    gpu_poll();
    return atomic_load(&g_gpu_percent);
}
