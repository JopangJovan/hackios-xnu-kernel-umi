# Lock-screen passcode and data protection

## 中文进度（2026-11-18）

用户选择先完成密钥袋与数据加密，再接锁屏密码。**目前锁屏密码仍未修好，
现有 iOS 用户数据也尚未加密**。USB 配对和 AFC 照片导出相关代码已单独
提交为 `585eba0`；本页的调查和测试与该提交分开。

已定位到三个直接缺口：原厂 `keybagd` 启动任务被排除；内核
`MiniAppleKeyStore` 固定返回“没有密码”，密钥包装只是可逆 XOR；
当前可写 HFS 数据卷关闭了内容保护。恢复 `keybagd` 后，原厂
`aks_change_secret_se` 调用的 `0x0f` 接口仍被内核拒绝，不能靠打开服务解决。

独立加密测试已经完成一次 iOS → 安卓 → iOS 的重启验证：同一份测试
密钥袋可以解密原数据，错误密码、损坏的密钥袋和被改动的密文均被拒绝。
这些只使用公开的测试口令和生成的数据，不会读取用户密码或更换系统密钥袋。

继续审查时发现，加载器的初始随机数种子来自计时器，尚不适合生成真实
保护密钥。已改为使用临时 UEFI 的 RAW RNG 协议，失败时不再退回计时器；
`native-20` 实机成功取种子并启动了 iOS，四组换密码测试文件在跨重启后
全部解密通过，USB 配对仍然成功。还需完善 XNU 加密后端和文件系统保护类。
完成这些之前，不迁移
现有备忘录、照片、钥匙串或 USB 配对记录。

本轮又将创建、保存、加载、改密，以及锁定、解锁和文件密钥包装接口
接入了内核的独立 AppleKeyStore 实验路径。`native-22` 创建测试密钥袋，
改密后保存；正常回安卓再启动 `native-23`，读取同一份密钥袋和密文，
新密码解密成功、哈希一致，旧密码、篡改数据和未授权访问均被拒绝。
实验需要专用启动参数和 root 客户端主动启用，不能访问系统密钥袋 0。
文件内容由测试程序加密，内核负责保护文件密钥；**尚未接入 HFS 的
透明加密读写，也不能据此开启设置中的锁屏密码**。当前内核 AES 后端
还不是常数时间实现，所以本轮只使用公开测试口令和生成的数据。
两轮均复用了原有 USB 配对并成功读取系统日志，数据卷同步成功后
返回原厂安卓；没有修改安卓分区、GPT 或启动分区。

## Current status

The system passcode is **not functional**. The new independent-bag laboratory
below does not enable Settings' passcode flow or migrate the system keybag.
On 2026-11-17 the user reproduced a
failed passcode setup on the paired native-USB image. In test `native-15`,
`profiled` logged "Passcode has been changed", but launchd recorded a
failed lookup for `com.apple.mobile.keybagd.xpc` at the same time. The
user confirmed that setting and using the passcode still failed.

The chosen scope is real keybag persistence and encrypted data, before
enabling the lock-screen passcode. A UI-only password gate is not the goal.
The USB checkpoint is commit `585eba0`; the passcode investigation is
separate from that commit.

## Confirmed gaps

1. The helper's job-generation deny list excludes `keybagd`. The stock
   `com.apple.mobile.keybagd` job is absent from the normal helper. A
   successful-looking Settings/profiled message therefore does not prove
   that a password was installed.
2. `MiniAppleKeyStore.cpp` explicitly represents a device with no passcode.
   Lock flags always return `0x6`, device lock state is always disabled,
   and lock/unlock calls return success without changing state. The
   existing item-key wrapping is a fixed reversible XOR, not encryption.
   It cannot provide password protection for the persistent keychain.
3. The prepared HFS data-volume driver overrides `CONFIG_PROTECT` to `0`
   in `hfs.h`. `/private/var` persistence on `ufs_data` does not establish
   content protection. Turning on the setting alone would be insufficient.
4. The current kernel crypto provider supplies ECB/CBC/CTR but leaves
   AES-XTS unimplemented. Its small AES implementation explicitly warns
   that it is not constant-time. The UFS path does not currently provide
   the complete encrypted data I/O backend for this keybag workflow.
5. The loader originally filled `/chosen/random-seed` using `CNTVCT_EL0`
   counter samples XORed with a fixed sequence. XNU hashes that seed into
   its HMAC-DRBG. A functional CommonCrypto round trip does not qualify
   this seed as cryptographic entropy. The new UMI loader helper requests
   two blocks from `EFI_RNG_PROTOCOL` using the RAW algorithm, rejects provider errors, uniform
   or identical blocks, and clears temporary buffers. There is no UMI
   counter fallback. These are failure checks, not an entropy certification;
   they do not certify the firmware provider's entropy quality.

The earlier Android FBE_ICE/F2FS test exercised Android's Keymaster, Linux
dm-default-key, fscrypt, and UFS ICE. It did not install those services in
XNU and does not make Apple's keybag APIs work automatically.

## Restoring the daemon in isolation

Test `native-16` added the original keybagd launch job to a copy of the
USB helper, with the paired kernel and lockdownd configuration unchanged.
The daemon started. A read-only probe queried AppleKeyStore 18 times and
always received `0x6`. The kernel recorded these unsupported calls:

| Caller | Selector | Observed inputs |
|---|---|---|
| keybagd | `0x0f` | 11 scalar inputs, 1 scalar output |
| lockdownd | `0x02` | 6 scalar inputs, 1 scalar output |

Offline inspection of the local iOS 16.5 AppleKeyStore framework maps
`0x0f` to `aks_change_secret_se` (11 scalar inputs) and `0x02` to the
create-bag path. The scalar inputs include user pointers; probes must
never dump their pointed-to buffers or values.

Only interface IDs, counts, status, and public lock flags were recorded;
the probe does not read or print passwords or key material. These errors
confirm that restoring the daemon alone is not a complete fix. The
diagnostic job is not enabled in the normal image. Native USB pairing and
syslog still worked, and the data flush succeeded before stock Android
booted again.

## Independent encrypted fixture

`tools/ios-keybag-test/crypto-selftest.c` is a capability test using a public
synthetic passphrase and generated data. It is **not** Apple's keybag
format, the system passcode implementation, a mounted encrypted volume,
or evidence of hardware-bound keys.

It uses the existing CommonCrypto library to generate random encryption
and authentication keys, derive a wrapping/authentication pair with
PBKDF2-HMAC-SHA256, wrap keys with RFC 3394 AES key wrap, and protect a
16 KiB fixture with AES-256-CBC plus HMAC-SHA256. The MAC is verified
before data is decrypted or released. The test checks an RFC 3394 known
answer, wrong-password rejection, modified-keybag rejection, and modified
ciphertext rejection with cleared output. It only writes two new,
owner-only files in a fresh test directory; it has no block-device code.

The macOS create/verify checks, iOS creation (`native-17`), and read-only
verification after a complete Android reboot (`native-18`) passed.
The decrypted test-data SHA-256 was
`600fdcd555529198ed7e9307828f1435b96d4efa943fc07528263b9d1f294ae8`.
`native-18` also established a paired USB session, read the expected version
and model, flushed the data volume successfully, and returned to stock Android.

The fixture now also exercises a **single-file** keybag replacement: write
a pending encrypted keybag, sync it, rename it over the active keybag, then
sync the directory. Test child processes stop at a partial pending write,
after the pending-file sync, and after rename. Before rename the old active
bag remains authoritative; a pending file is never promoted automatically.
After recovery/replacement, the new public test passphrase opens the bag,
the previous one fails against that active bag, and the data ciphertext is
unchanged. This matrix passed on macOS and in `native-19` on iOS.
`native-20` verified all four updated bags again after a complete Android
reboot, with the corrected RNG seed path. The paired lockdown session
still returned success and the expected version/model. Framebuffer capture
showed the Settings UI, so this test also passed the earlier black-screen
failure point.

All four sessions (`native-17` through `native-20`) completed a synchronous
data-volume flush with `errno=0` and returned to stock Android. After the
last session, ADB reported `sys.boot_completed=1` and MIUI build
`V14.0.4.0.TJBCNXM`.

These are **process-interruption** tests, not physical power-loss tests.
Visibility after rename is not proof of power-loss durability before the
directory sync. Restoring an older keybag can restore acceptance of its old
passphrase: there is no anti-rollback enforcement here. No hardware rollback
protection has been enabled. The fixture format and public passphrases must
never be used to protect real user data.

API references: [CommonCrypto key wrapping](https://github.com/apple-oss-distributions/CommonCrypto/blob/main/include/CommonSymmetricKeywrap.h),
[CommonCrypto key derivation](https://github.com/apple-oss-distributions/CommonCrypto/blob/main/include/CommonKeyDerivation.h),
[EFI RNG protocol](https://github.com/tianocore/edk2/blob/master/MdePkg/Include/Protocol/Rng.h).

Apple's [keybag overview](https://support.apple.com/nl-nl/guide/security/sec6483d5760/web)
describes protected class keys and Secure Enclave-managed state on supported
Apple hardware. The independent fixture above is not an implementation of
that hardware security boundary on Qualcomm.

## Loader entropy correction

The UMI UEFI already includes its original `RngDxe.efi`. The tested binary's
SHA-256 is
`213fe5cb11ad6543f8ed02f2d31db2aa1b58f0a182286769fd8fd95102c010bd`.
Local inspection found the standard EFI RNG protocol and RAW algorithm;
its `GetRNG` routine at RVA `0x143c` reaches the PRNG reader at `0x22e0`,
which polls `0x00793004` and reads `0x00793000`. Those addresses fall in
UMI's existing `PRNG_CFG_PRNG` device mapping. This identifies a peripheral
read path, unlike the previous counter-derived seed; it is not a statistical
or hardware security certification.

`loader/umi_rng.h` requests two 64-byte blocks, uses the first as XNU's
seed, clears the check block, and the caller clears its seed copy after
placing it in the device tree. XNU's `PE_get_random_seed` clears the tree's
copy when consuming it. Logs contain only source/status, never seed bytes.
Missing interfaces, provider errors, uniform output, or repeated output
abort UMI kernel startup rather than use a predictable fallback. The
firmware reader itself contains a readiness polling loop; a provider that
never returns is not covered by the helper's returned-error checks.

The loader failure-injection test and both actual loader translation units
compiled successfully. The normal loader compilation reported two unused
function warnings (`find_log_partition` and `ramfb_phone_screen`); the RNG
helper test passed with `-Wall -Wextra -Werror`.
The existing `prepare.py verify` command still fails at
`xnu/files/osfmk/arm64/loader_return.c`: its working bytes equal `HEAD`, but
the older template manifest expects a different hash. This investigation
did not change that shared file or rewrite the manifest to hide the mismatch.

## Integration gates

1. Preserve the RAW RNG boot, cross-reboot fixture, and fault-injection tests
   as regression checks. Audit readiness/timeout handling and entropy health;
   these functional tests do not establish hardware key binding.
2. Implement an authenticated, versioned persistent keybag backend with
   atomic updates, recovery after an interrupted update, and migration
   tests. Separate password-wrapped class keys from device/hardware key
   binding. Qualify the native XNU entropy and cipher backend; a test
   using CommonCrypto in a process does not qualify the kernel backend.
3. Connect the real AKS create/change/load/save/lock/unlock semantics,
   caller authorization, state notifications, and key removal. Failed
   authentication or missing services must not return success. Preserve
   the existing keychain and USB escrow/pairing records during migration.
4. Connect HFS/VFS protection classes to actual encrypted reads and writes
   on a disposable filesystem image. Verify raw backing bytes are
   ciphertext, wrong keys fail, and lock/unlock enforces class policy.
   Do not simply remove the `CONFIG_PROTECT` override on the live volume.
5. Prepare and verify a recoverable migration of the current iOS data.
   Only after that succeeds, test the native Settings and lock-screen
   sequence, reboot unlock, USB trust, photos, calls, and messages.

The first native integration target is a separate disposable bag handle,
not system handle 0. Its experimental implementation and cross-reboot device
test are described below. System integration still requires durable keybag
service semantics, protection-class notifications, and a qualified cipher
backend before existing data can be migrated.
Existing XOR-wrapped keychain items and already-paired USB credentials
must be handled explicitly; they were not rewritten or rekeyed in these tests.

## Native independent-bag interfaces, 2026-11-18

The [independent AppleKeyStore experiment](experimental/keybag/README.md)
implements create/save/load/change-secret plus lock/unlock, item-key
wrap/unwrap, and unload inside the prepared kernel's AppleKeyStore
user client. Both `umi_aks_lab=1` and an explicit root-client opt-in are
required. System handle 0 cannot be selected, and ordinary clients retain
their existing behavior. This is an experimental subset of the observed
IOConnect ABI, not complete compatibility with native keybagd or Apple's
serialized keybag format.

The host test compared the actual key-wrap/KDF implementation with
CommonCrypto for every supported wrapped-key length and multiple PBKDF2
counts. The kernel and iOS probe compiled successfully. Patch reconstruction
and repeated staging into independent copies passed. Shared repository
templates and the normal build configuration were not changed.

The first temporary boot (`native-21`) was rejected by the loader's argument
whitelist before XNU started, and returned to stock Android. The isolated
loader now accepts exactly `umi_aks_lab=1`; a host test confirms that `0`,
`2`, `10`, and trailing junk remain rejected.

In `native-22`, the phone booted with the RAW RNG seed path and ran these
checks through native `IOConnectCallMethod` requests:

| Check | Observed result |
|---|---|
| Root opt-in / non-root opt-in | Root test enabled; non-root explicitly denied |
| Normal client / system handle | Normal create remained unsupported; opted-in client refused system handle 0 |
| User pointers and secret bounds | Bad pointer and 129-byte secret rejected |
| Create / save | Nonzero independent handle and encrypted 136-byte envelope |
| Incorrect old password during change | Rejected; saved envelope byte-for-byte unchanged |
| Correct change / save / reload | New envelope loaded locked; old password rejected; new password unlocked |
| Per-file key wrapping | 64-byte generated file keys wrapped under the selected class; wrong class rejected |
| Encrypted file | 4 KiB synthetic payload decrypted and matched the independent expected SHA-256 |
| Lock / tamper / handle isolation | Locked unwrap, tampered bag, malformed bag, foreign and stale handles rejected |
| Normal system flags | Still `0x6` before and after the laboratory test |
| USB pairing | Existing paired encrypted lockdown session returned success; version/model remained readable |

`native-22` also captured 262,144 bytes / 1,407 lines through the native
`com.apple.syslog_relay` service in a 20-second check. The phone completed
a synchronous data-volume flush (`errno=0`) and booted stock Android before
the second iOS session. Framebuffer capture showed the iOS home screen.

`native-23` used the same kernel and a read-only verification helper. It
loaded the three files created in `native-22`, rejected the previous secret,
unlocked with the replacement secret, and recovered the same plaintext hash.
The repeated locked/tamper/class/foreign-handle checks and the separate
non-root rejection check also passed. No fixture was regenerated during
this verification.
The existing encrypted USB pairing was reused without another trust prompt;
version/model queries succeeded and a 20-second native syslog check read
204,800 bytes / 1,085 lines. Both sessions used the same native USB driver
objects and configuration. Calls, SMS, cellular data, and AFC photo export
were not repeated in this isolated modem-disabled test.
The final `native-23` log reported synchronous data flush `errno=0`.
ADB then confirmed `sys.boot_completed=1`, product `umi`, and the stock
MIUI build `V14.0.4.0.TJBCNXM`. Both independent-bag sessions therefore
ended in stock Android, rather than leaving iOS running after the tests.

The generated plaintext SHA-256 was
`0d7fd71ee60a33433b8be0a5d664b379ec8580f8a64a84c77bd4068fca386137`.
Only `keybag.bin`, `wrapped-file-keys.bin`, and `ciphertext.bin` were created
inside a fresh root-owned test directory; the program does not accept personal
passphrases and logs no secret values. Its per-file data encryption uses
CommonCrypto; key protection runs through the kernel's laboratory AKS path.
This does not establish HFS encrypted block I/O, hardware binding, anti-rollback,
or production security for the current non-constant-time kernel AES provider.

No Android, GPT, or boot partition changes are part of this work. The
system keybag and existing user files have not been converted to a new
format. The phone returns to stock Android after each temporary test.
