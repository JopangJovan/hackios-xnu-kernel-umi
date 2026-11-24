/* Shared guard for the original-service integration test. /private/var
 * stays volatile; a separately mounted device may hold the test journal. */
#ifndef UMI_SYSTEM_FIXTURE_H
#define UMI_SYSTEM_FIXTURE_H

#include <ctype.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

#define UMI_SYSTEM_FIXTURE_ROOT "/var/tmp/k06"
#define UMI_SYSTEM_FIXTURE_MOUNT "/private/var/tmp/k06"
#define UMI_SYSTEM_FIXTURE_FLAG "umi_aks_service_txn=1"
#define UMI_SYSTEM_FIXTURE_DEVICE "/dev/ufs_retry"
#define UMI_SYSTEM_FIXTURE_RAW "/dev/rufs_retry"
#define UMI_SYSTEM_FIXTURE_BYTES (8u * 1024u * 1024u)

static inline int umi_system_fixture_flag(const char *flag)
{
    char args[4096] = {0};
    size_t length = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &length, NULL, 0)) return 0;
    char *save = NULL;
    for (char *word = strtok_r(args, " \t", &save); word; word = strtok_r(NULL, " \t", &save)) {
        if (!strcmp(word, flag)) return 1;
    }
    return 0;
}

static inline int umi_system_fixture_retry_mode(void)
{
    if (umi_system_fixture_flag("umi_aks_shared_retry=1")) return 1;
    return umi_system_fixture_flag("umi_aks_shared_retry=2") ? 2 : 0;
}

static inline int umi_system_fixture_volatile(void)
{
    struct statfs volume;
    if (statfs("/private/var", &volume) || strcmp(volume.f_mntonname, "/private/var")) return 0;
    const char *device = volume.f_mntfromname;
    int volatile_volume = !strcmp(volume.f_fstypename, "tmpfs") || !strcmp(volume.f_fstypename, "ramfs");
    if (!volatile_volume && !strncmp(device, "/dev/md", 7) && isdigit((unsigned char)device[7])) {
        device += 7;
        while (isdigit((unsigned char)*device)) ++device;
        volatile_volume = !*device;
    }
    return volatile_volume && umi_system_fixture_flag(UMI_SYSTEM_FIXTURE_FLAG);
}

static inline int umi_system_fixture_allowed(void)
{
    if (!umi_system_fixture_volatile()) return 0;
    if (!umi_system_fixture_retry_mode()) return 1;
    struct statfs mount;
    return !statfs(UMI_SYSTEM_FIXTURE_MOUNT, &mount) &&
        !strcmp(mount.f_mntonname, UMI_SYSTEM_FIXTURE_MOUNT) &&
        !strcmp(mount.f_mntfromname, UMI_SYSTEM_FIXTURE_DEVICE) &&
        !strcmp(mount.f_fstypename, "hfs") && !(mount.f_flags & MNT_RDONLY) &&
        (mount.f_flags & MNT_JOURNALED) &&
        mount.f_bsize && mount.f_blocks && mount.f_blocks <= UMI_SYSTEM_FIXTURE_BYTES / mount.f_bsize;
}

#endif
