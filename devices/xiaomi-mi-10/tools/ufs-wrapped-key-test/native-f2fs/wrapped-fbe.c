#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/fscrypt.h>
#include <linux/magic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#define RAW_MAX 128
#define WRAPPED_FLAG 1

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    int setting = !strcmp(argv[1], "policy");
    if (!setting && strcmp(argv[1], "add")) return 2;
    if (strncmp(argv[2], "/dev/umi-native-", 16)) return 2;
    int fd = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct statfs fs;
    if (fd < 0 || fstatfs(fd, &fs) || fs.f_type != F2FS_SUPER_MAGIC) return 2;
    char text[RAW_MAX * 2 + 2];
    if (!fgets(text, sizeof(text), stdin)) return 2;
    text[strcspn(text, "\n")] = '\0';
    size_t digits = strlen(text);
    if (digits < 128 || digits > 256 || digits % 2 || strspn(text, "0123456789abcdef") != digits) return 2;
    union {
        struct fscrypt_add_key_arg arg;
        unsigned char bytes[sizeof(struct fscrypt_add_key_arg) + RAW_MAX];
    } u = {0};
    _Static_assert(sizeof(struct fscrypt_add_key_arg) == 80, "unexpected fscrypt ABI");
    u.arg.key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
    u.arg.raw_size = digits / 2;
    uint32_t flag = WRAPPED_FLAG;
    memcpy(u.bytes + sizeof(u.arg) - sizeof(flag), &flag, sizeof(flag));
    for (size_t n = 0; n < digits; n += 2) {
        unsigned byte;
        if (sscanf(text + n, "%2x", &byte) != 1) return 2;
        u.arg.raw[n / 2] = byte;
    }
    explicit_bzero(text, sizeof(text));
    int result = ioctl(fd, FS_IOC_ADD_ENCRYPTION_KEY, &u.arg);
    struct fscrypt_policy_v2 policy = {
        .version = FSCRYPT_POLICY_V2,
        .contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS,
        .filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS,
        .flags = FSCRYPT_POLICY_FLAGS_PAD_16 | FSCRYPT_POLICY_FLAG_IV_INO_LBLK_64,
    };
    memcpy(policy.master_key_identifier, u.arg.key_spec.u.identifier, sizeof(policy.master_key_identifier));
    explicit_bzero(&u, sizeof(u));
    if (result) { perror("install hardware-wrapped fscrypt key"); return 1; }
    if (setting && ioctl(fd, FS_IOC_SET_ENCRYPTION_POLICY, &policy)) {
        perror("set wrapped-key fscrypt v2 policy");
        return 1;
    }
    puts(setting ? "Hardware-wrapped fscrypt key installed, IV_INO_LBLK_64 policy set" : "Hardware-wrapped fscrypt key reinstalled");
    close(fd);
    return 0;
}
