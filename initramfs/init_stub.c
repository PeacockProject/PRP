// Minimal PID1 wrapper for aarch64 PRP initramfs.
// Keep /init as a native ELF and delegate to init shell script.

#include <fcntl.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void kmsg(const char *msg) {
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if(fd >= 0) {
        (void)write(fd, msg, strlen(msg));
        close(fd);
    }
}

int main(void) {
    (void)mkdir("/proc", 0755);
    (void)mkdir("/sys", 0755);
    (void)mkdir("/dev", 0755);
    (void)mount("proc", "/proc", "proc", 0, "");
    (void)mount("sysfs", "/sys", "sysfs", 0, "");
    (void)mount("devtmpfs", "/dev", "devtmpfs", 0, "");

    kmsg("<6>PRP-STUB: entered /init\n");
    // Diagnostic: avoid touching fb from the native PID1 stub.
    // If panel still shows blue-decay, fault is earlier than initramfs userspace.

    if(access("/init.sh", X_OK) == 0 && access("/sbin/busybox", X_OK) == 0) {
        char *argv[] = {"/sbin/busybox", "sh", "/init.sh", NULL};
        execv(argv[0], argv);
        kmsg("<3>PRP-STUB: exec /sbin/busybox sh /init.sh failed\n");
    } else {
        kmsg("<3>PRP-STUB: /init.sh or /sbin/busybox missing\n");
    }

    // Keep PID1 alive if delegation fails.
    for(;;) {
        sleep(1);
    }
    return 0;
}
