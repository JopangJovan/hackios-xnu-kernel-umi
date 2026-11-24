#!/system/bin/sh
set -eu
caseid=$1
token=$2
root=/dev/umi-native-$token
raw=/dev/block/sde61
where=$root/mnt
full=65011712
limit=56623104
maptool=$root/lun4-map
image=$root/original.raw
manifest=$root/files.sha256
bound=$root/check-window.sh
"$bound"
IFS= read -r metadata
IFS= read -r filekey
case "$caseid" in exercise|verify) ;; *) exit 2;; esac
mkdir "$where"
active=0
mounted=0
cleanup() {
    if [ "$mounted" = 1 ]; then umount "$where" || return; fi
    if [ "$active" = 1 ]; then "$maptool" remove "$token" || return; fi
    rm -f "$image" "$root/shrunk-fsck.log" "$root/final-fsck.log"
    rmdir "$where"
}
trap cleanup EXIT
trap 'exit 120' HUP INT TERM
attach() {
    "$bound"
    mapped=$(printf '%s\n' "$metadata" | "$maptool" "$1" "$token")
    active=1
    case "$mapped" in /dev/block/dm-[0-9]*) ;; *) exit 2;; esac
    test "$(ls "/sys/class/block/${mapped##*/}/slaves")" = sde61
    if [ "$1" = rw ]; then test "$(blockdev --getro "$mapped")" = 0
    else test "$(blockdev --getro "$mapped")" = 1; fi
}
detach() {
    "$maptool" remove "$token"
    active=0
}
mount_test() {
    mount -t f2fs -o "$1",inlinecrypt,heap,active_logs=2,mode=lfs,nodiscard,background_gc=off "$mapped" "$where"
    mounted=1
    test "$(awk -v p="$where" '$2==p { print $1; exit }' /proc/mounts)" = "$mapped"
}
unmount_test() {
    umount "$where"
    mounted=0
}
read_le() { od -An -tu"$2" -j "$1" -N "$2" "$3" | tr -d ' \n'; }
magic() { od -An -tx4 -j 1024 -N 4 "$1" | tr -d ' \n'; }
checksum() { sha256sum "$1" | cut -d' ' -f1; }
install_key() {
    printf '%s\n' "$filekey" | "$root/wrapped-fbe" "$1" "$2"
}
check_files() {
    if cat "$where/protected/tail.bin" >/dev/null 2>&1; then
        echo 'Protected file accessible before installing its wrapped key' >&2
        exit 4
    fi
    install_key add "$where"
    sha256sum -c "$manifest"
}
check_fs() {
    name=$1
    logfile=$root/$name-fsck.log
    fsck.f2fs --dry-run --no-kernel-check -f "$mapped" > "$logfile" 2>&1 || { cat "$logfile"; exit 3; }
    if grep -iE 'Error:|\[Fail|corruptions found' "$logfile"; then cat "$logfile"; exit 3; fi
    cat "$logfile"
    echo "Read-only fsck PASSED for $name"
}
if [ "$caseid" = exercise ]; then
    attach rw
    make_f2fs -a 1 -f -t 0 -O encrypt,extra_attr,quota,project_quota -l UMI_ICE_62_TEST "$mapped"
    test "$(magic "$mapped")" = f2f52010
    test "$(magic "$raw")" != f2f52010
    echo 'F2FS superblock found through hardware-wrapped dm-default-key only'
    mount_test rw
    mkdir "$where/protected"
    install_key policy "$where/protected"
    dd if=/dev/urandom of="$where/plain.bin" bs=524288 count=1 conv=fsync
    dd if=/dev/urandom of="$where/protected/sample.bin" bs=524288 count=1 conv=fsync
    dd if=/dev/urandom of="$where/protected/tail.bin" bs=524288 count=3 conv=fsync
    extent_before=$("$root/extent-test" "$where/protected/tail.bin")
    echo "Wrapped-file extent end before shrink: $extent_before, boundary: $limit"
    test "$extent_before" -gt "$limit"
    sha256sum "$where/plain.bin" "$where/protected/sample.bin" "$where/protected/tail.bin" > "$manifest"
    unmount_test
    detach
    "$bound"
    dd if="$raw" of="$image" bs=1048576 count=62
    test "$(stat -c %s "$image")" = "$full"
    original=$(checksum "$image")
    test "$(checksum "$raw")" = "$original"
    echo "Raw hardware-encrypted backup verified $original"
    attach rw
    logseg=$(read_le 1044 4 "$mapped")
    blocks=$(read_le 1060 8 "$mapped")
    segments=$(read_le 1092 4 "$mapped")
    main=$(read_le 1116 4 "$mapped")
    test "$blocks" = 15872
    target=$((main+segments*(1<<logseg)-2048))
    test "$target" = 13824
    mount_test rw
    check_files
    "$root/f2fs-resize" "$where" "$target"
    unmount_test
    detach
    "$bound"
    changed=$(checksum "$raw")
    test "$changed" != "$original"
    echo "Shrunk on-disk ciphertext differs $changed"
    attach ro-small
    test "$(blockdev --getsize64 "$mapped")" = "$limit"
    test "$(read_le 1060 8 "$mapped")" = 13824
    check_fs shrunk
    mount_test ro,norecovery
    check_files
    extent_after=$("$root/extent-test" "$where/protected/tail.bin")
    echo "Wrapped-file extent end after shrink: $extent_after, boundary: $limit"
    test "$extent_after" -le "$limit"
    unmount_test
    detach
    "$bound"
    dd if="$image" of="$raw" bs=1048576 count=62 conv=fsync,notrunc
    test "$(checksum "$raw")" = "$original"
    echo "Exact original raw ciphertext restored $original"
    test "$(magic "$raw")" != f2f52010
    printf 'NATIVE_ORIGINAL_HASH %s\n' "$original"
    echo NATIVE_SHRINK_RESTORE_PASSED
else
    IFS= read -r wanted
    test "$(checksum "$raw")" = "$wanted"
    echo "Restored ciphertext unchanged across reboot $wanted"
    attach ro
    test "$(blockdev --getro "$raw")" = 1
    test "$(read_le 1060 8 "$mapped")" = 15872
    test "$(magic "$raw")" != f2f52010
    check_fs final
    mount_test ro,norecovery
    check_files
    unmount_test
    detach
    test "$(checksum "$raw")" = "$wanted"
    echo NATIVE_REBOOT_REMOUNT_FILE_HASHES_PASSED
fi
unset metadata filekey
