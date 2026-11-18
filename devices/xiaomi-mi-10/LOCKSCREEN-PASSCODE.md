# Lock-screen passcode and data protection

## 中文进度（2026-11-18）

用户已选择接好原生锁屏密码后再加密现有 iOS，并允许抹除 iOS 测试数据。
安卓、GPT 和启动分区仍不在修改范围内。**目前锁屏密码仍未修好，
现有 iOS 用户数据也尚未加密**。USB 配对和 AFC 照片导出相关代码已单独
提交为 `585eba0`；本页的调查和测试与该提交分开。

最新的 `native-30/31` 已通过系统密钥袋管理接口的独立测试：改密先产生
候选袋，保存成功后才安装；保存接口缓冲区不足或安装认证失败时，原袋仍
可用（尚未对原厂落盘流程注入真实写盘故障）。回安卓再进入
iOS 后，同一份袋可以用新密码解密生成的测试文件，旧密码被拒绝，哈希
一致，USB 配对和系统日志仍可读取。`native-34` 又通过原厂 `keybagd`
的 XPC 改密、保存、安装和完成确认：仅将原厂程序的文件路径重定向到
独立测试目录，实际保存的加密袋与内核安装内容相同，错误密码被拒绝。
这仍不能代替原生锁屏、正式启动加载或全盘加密验证。

`native-35` 已从上一轮原厂保存的二进制 plist 读取同一份加密袋，通过
测试加载器安装为“首次解锁前锁定”，验证错误密码拒绝、正确密码解锁、
重锁和序列化内容不变。随后新建另一份测试袋，原厂 flag 4 改密方式、
完成确认的错误密码拒绝、失败安装保留原袋、加密文件解密及哈希比较
也全部通过。该加载器仍是诊断程序，不是正式的 `keybagd --init` 启动接入。
USB 继续复用原配对，30 秒读取 385,024 字节、2,288 行系统日志；截图确认
桌面正常。正常系统钥匙串及现有 `/private/var` 尚未迁移或重新加密。
最终数据卷同步返回 `errno=0`，手机已回到原厂安卓；没有修改安卓、GPT
或启动分区。

`native-37` 又通过了原厂 `keybagd --init` 的真实加载流程和原厂
MobileKeyBag 的锁定、解锁、事件回调。驱动此前把待加载状态返回为
`NotReady`，而原厂只在 `NotFound` 时读盘；`native-36` 因这个返回码
错误触发退出，已保存失败记录并修正。修正后原厂读取同一份测试袋、
安装为锁定状态，错误密码不解锁、不发成功通知，正确密码触发一次
首次解锁，重复锁定/解锁不多发通知。SpringBoard 和 UserEventAgent
自己的日志也看到了对应状态变化。USB 配对仍有效，最终同步成功并回安卓。
**这是隔离目录的初始化与通知测试，尚未验证用户在设置中输入密码的
完整流程；原厂日志仍明确报告当前 HFS 没有内容保护，现有 iOS 数据尚未加密。**

`native-38` 已通过普通用户权限测试：两个独立 UID 501 客户端只有签名
权限不同，有原厂 `com.apple.keystore.device` 权限的一方能验证密码、
锁定和解锁，没有权限的一方被拒绝；双方都不能控制实验、替换或导出
系统密钥袋。原厂初始化和通知测试同时通过，USB 配对和日志保持正常，
同步成功后回到安卓。

`native-40` 进一步通过“服务发布前即锁定”的启动验证：第 35 秒截图已
出现原生中文「iPhone 重新启动后需要输入密码」，而测试程序尚未加载
磁盘袋。初始状态为锁定，不能重新初始化为无密码；随后原厂加载和全部
密码/权限/通知检查通过。锁定时 USB 配对及型号读取正常，日志服务返回
「Password protected」；正确解锁后同一配对成功读取 1,032,192 字节、
6,022 行日志。最终数据同步 `errno=0`，原厂安卓启动完成，手机已留在安卓。
这是公开测试密码的自动验证，没有开启 HFS 文件保护。

`native-41` 开始用户实际输入的原生设置测试。内核观察到 `6 → 4 → 5`，
但用户报告「设置重新进入有密码，锁屏没有密码」，因此本轮**不能判通过**。
用户再次在设置验证密码后，同一 USB 配对从日志服务「Password protected」
变为可读取，采集到 851,968 字节、4,974 行日志。手机随后已确认回原厂安卓；
本轮没有取得最终固件日志，不能声称最终同步状态已核对。

该轮探针的 `persisted_envelope_match=-1` 尚不能说明密码袋未保存：
原厂 `keybagd` 的保存代码使用 `0644`，而探针错误地要求文件没有任何组/其他
读取位。修正后允许原厂 `0644` 或 `0600`，仍要求两级目录均为 root `0700`、
普通文件、root 所有、单链接且不可跟随符号链接。后续重载测试将独立比较
落盘密文和内核密文，不读取或记录用户密码。临时启动同时关闭
`autounlock`，增加只读 SpringBoard 状态探针；自动进桌面只发生在早期，
目前不能据此认定它就是本轮锁屏未询问密码的根因。

`native-42` 已证实原袋跨重启加载成功，原厂文件实际为 `0644`，
密文比较结果为一致。用户确认开机第一次需要输入原密码；**后续多次
锁屏/唤醒仍不要求密码**，因此原生锁屏流程仍未通过。关闭自动进桌面
不能解决这个问题。内核重锁状态为 `5`，USB 诊断服务也拒绝未认证访问；
后续重点检查 SpringBoard 的认证缓存、免输密码期限和撤销认证策略。
当前没有启用数据加密。再次在设置认证后读取到 368,640 字节、2,145 行
日志，随后连接中断；已确认原厂安卓启动，但没有最终同步遥测。

`native-45` 保留固件日志通道，已确认重锁时内核为 `5`，但界面仍是
`authenticated=1`、`secure_display=0`；同时 `has_passcode=1`、`grace=0`、
`allow_revoke=1`。后续定位通知送达、实时状态与认证缓存之间的不一致，
目前仍不能判定锁屏修好。最终同步 `errno=0`，已确认回原厂安卓。
之前临时 UEFI 另有 390 秒硬超时，已仅在测试镜像中延长到 1320 秒，
给 XNU 的 1200 秒正常保存退出留出时间。`native-44` 因测试脚本误判
运行状态立即退出，作为失败测试保留记录，不计为验证通过。

`native-46` 进一步确认原厂 MKB 的解锁和重锁通知都已收到（状态事件 2 次、
首次解锁 1 次）；实时与缓存 SBF 状态均为 `2`（已锁定），界面仍错误保持
认证，因此不是内核通知缺失。`native-47` 在临时镜像验证了独立的锁定状态
同步适配：只在真实锁定事件后，使用原生 API 清缓存并按真实锁定状态更新
认证，不处理密码或解锁成功。

`native-47` 已通过本轮手动验证：用户确认锁屏后要求密码，错误密码不能
进入桌面。最终日志记录三次完整重锁/解锁；每次重锁均变为 `authenticated=0`、
`secure_display=1`，正确解锁后回桌面，密码袋密文保持一致。原厂 MKB 共
收到 7 次状态事件、1 次首次解锁事件，没有重复发送首次解锁通知。
这修正了临时镜像中的原生重锁行为；尚不等于正式系统加密完成。本轮保留
固件诊断 USB，没有重新验证原生配对，也没有修改已有配对记录。
最终数据卷同步 `errno=0`，已确认新一轮原厂安卓启动完成；安卓、GPT 和
启动分区均未写入。当前修正只包含在临时实验镜像，尚未并入日常构建。

接下来验证完整的原生设置、密码重试规则和用户实际跨重启解锁，再将
文件保护类与加密卷启动顺序接通。只有这些一并验证后，才初始化正式
加密数据卷，不能把界面要求密码当作加密完成。

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
透明加密读写，也不能据此开启设置中的锁屏密码**。该阶段使用的内核 AES 后端
还不是常数时间实现，所以本轮只使用公开测试口令和生成的数据。
两轮均复用了原有 USB 配对并成功读取系统日志，数据卷同步成功后
返回原厂安卓；没有修改安卓分区、GPT 或启动分区。

独立密钥袋阶段已单独提交为 `2efb96f`。后续实验已将实验路径的 AES
替换为 BearSSL 的位切片实现，并在独立 16 MiB HFS 测试卷下接入
AES-XTS 与扇区认证。`native-24` 写入、卸载并保存密文镜像，回安卓后
`native-25` 重新解密并只读挂载，文件内容和哈希一致；旧密码、错误卷
密钥、篡改密文和只读卷写入均被拒绝。**目前是内存密文块设备，卸载后
显式保存快照；还不是实时持久写盘，也不是现有 HFS 的逐文件保护类。**
系统锁屏密码及现有用户数据迁移仍未启用。

后续 `native-26/27` 已完成实时文件后端验证：独立的 16 MiB 日志式 HFS+
卷直接把认证密文写入 iOS 数据卷上的测试文件，无需卸载后导出快照。
重启前模拟“恢复记录完整、主密文只写了一半”，经过原厂安卓重启后，
下一次 iOS 自动恢复 1 个扇区，文件哈希一致；旧密码和错误卷密钥仍被拒绝。
两轮 USB 配对、系统日志和正常回安卓均通过，数据卷同步返回 `errno=0`。
这不是物理断电测试，也不是全系统加密完成。接下来按用户选择先接原生
锁屏；允许清空 iOS 可以简化初始化，但不能省去系统密钥袋、锁定通知和
文件保护类的实现，也不能用公开测试口令保护真实数据。

## 接下来怎样判定密码已修好

当前每一轮只测试一条明确路径；出现密码输入页或 API 返回成功都不等于
保护链路已经完成。后续顺序如下：

1. 将已验证的原厂保存、启动加载、状态通知及授权接口接到完整的原生
   设置流程，验证用户在手机输入的新密码、锁屏后错误/正确密码，以及
   再次临时启动后的原生解锁。密码不进入命令行或日志。测试模式应保持
   独立，失败不得把已存在的密码袋重新初始化为“无密码”。
2. 实现并验证密码重试/恢复策略、保护类密钥及锁屏时的缓存生命周期。
   当前系统袋 0 的普通 item-key 包装仍保留旧路径，不能把新增的管理
   接口当成已加密的系统钥匙串。隔离测试 root 的管理能力也不是生产
   安全边界；Secure Enclave 和硬件防回滚尚未实现。
3. 在独立测试卷接上文件保护类与实际读写加密，再初始化正式 iOS 数据。
   实际 owning source 中 `bsd/miscfs/hfs/hfs.h` 仍将 `CONFIG_PROTECT`
   强制设为 0，`bsd/dev/hakios.ufs.c` 的 `ufs_strategy` 直接读写缓冲区，
   尚未消费逐文件加密上下文。仅启用标志或假报成功不可解决这一点。
   当前 init 在原生锁屏前挂载 `/private/var`，挂载失败还可回落到 RAM
   卷；正式加密模式必须另行设计启动依赖，并拒绝明文/空卷回落。

已有的独立加密卷和密钥袋测试可用于这些回归，但尚不能给出完整原生
密码与数据加密的可靠完成时间。用户允许抹除 iOS，可省去旧数据迁移；
不能省去以上启动、鉴权和真实密文恢复的验证。

## Current status

The system passcode is **not functional**. The new independent-bag laboratory
below does not enable Settings' passcode flow or migrate the system keybag.
On 2026-11-17 the user reproduced a
failed passcode setup on the paired native-USB image. In test `native-15`,
`profiled` logged "Passcode has been changed", but launchd recorded a
failed lookup for `com.apple.mobile.keybagd.xpc` at the same time. The
user confirmed that setting and using the passcode still failed.

The chosen scope is real keybag persistence and encrypted data with the
native lock-screen passcode connected before migration. The user permits
erasing iOS test data; Android/GPT/boot partitions remain out of scope.
A UI-only password gate is not the goal.
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

## Independent encrypted HFS volume, 2026-11-18

The preceding independent-bag work, RNG prerequisite and test results were
committed separately as `2efb96f`. The following
[filesystem experiment](experimental/crypt-volume/README.md) is a subsequent
change, not part of that checkpoint.

It substitutes BearSSL's portable bitsliced AES only in the laboratory AKS
client and the new test-disk path. The normal kernel crypto provider,
system keybag, existing HFS volume and USB credentials are unchanged.
AES-128/192/256 outputs match CommonCrypto; AES-256-XTS outputs for five
4 KiB data units match independent cryptography/OpenSSL results. The
kernel and probe build, vendor-source checks, and reconstruction/idempotent
staging checks passed. This is not a production side-channel qualification.

`native-24` used a 16 MiB unjournaled HFS+ test filesystem. Its generated
master was wrapped by the independent AKS bag and recovered through the
new-password unlock path. A root-only kernel block device encrypted
filesystem reads/writes with XTS and authenticated every sector with HMAC.
The test mounted HFS, wrote a 128 KiB payload, committed a second file by
rename, verified both, unmounted, and exported a ciphertext snapshot into
a fresh private test directory.

The device checks passed: old-password rejection, wrong raw-key rejection,
modified-sector rejection before mounting, removal of device access after
clearing keys, and absence of the plaintext marker in the saved ciphertext.
The older `native-22` AKS fixture also decrypted successfully with the new
AES implementation. No pre-existing user file was re-encrypted.
Existing USB pairing returned success and identified iOS 16.5 / iPhone10,3 /
D22AP; native syslog delivered 229,376 bytes / 1,257 lines in 20 seconds.

The filesystem fixture SHA-256 was
`10a90508e6050203d86bb432fc8f435efbe2359d4f6872efe97e0932539b495d`.
Only generated files under the new test directory were used. The saved
192-byte keybag fixture and 16,908,416-byte ciphertext image are regular
files on the existing iOS data volume. The kernel driver cannot address
Android partitions, GPT, or boot partitions.

`native-24` completed its data-volume flush with `errno=0` and returned to
stock Android before `native-25`. The second iOS session loaded the same
192-byte keybag fixture and ciphertext snapshot; it did not recreate or
export either file. HFS mounted read-only, both file comparisons passed,
the payload hash above was identical, and a new-file write returned `EROFS`.
Wrong-password, wrong-key, tamper and locked-device checks passed again.
The older independent AKS fixture and non-root opt-in rejection also passed.
The payload hash also matches a separately generated host reference pattern.
USB pairing was reused again, the expected version/model were readable, and
native syslog delivered 204,800 bytes / 1,095 lines in 20 seconds. No new
trust approval was needed. Calls, SMS and AFC export were not repeated in
these modem-disabled filesystem tests.
The final session also completed its data-volume flush with `errno=0` and
returned to stock Android. ADB confirmed `sys.boot_completed=1`, product
`umi`, and MIUI `V14.0.4.0.TJBCNXM`. The phone was left in Android.

The test disk stores ciphertext in RAM during the mount. Persistence is an
explicit clean-unmount/export/file-sync step; writes are not streamed to a
durable backing file. Power loss before export discards that session's
changes. This proves a filesystem can use the encrypted block path, not
HFS per-file protection classes, live durable encrypted storage, hardware
binding, or rollback prevention. Settings' passcode flow remains disabled.

The live-backend gate was subsequently exercised below. Native
protection-class lifecycle integration and the native lock-screen flow
remain prerequisites to encrypting the actual iOS data environment.

## Live encrypted file and redo recovery, 2026-11-18

The new bounded file backend opens only a freshly generated, root-owned
test file on `/dev/ufs_data`; it cannot select a raw partition. A 16 MiB
journaled HFS+ filesystem uses AES-XTS/sector authentication through this
file. The redo record is authenticated and synced before the main record,
and cleared only after that main record is synced. I/O and flush failures
are propagated, and a failed session refuses further I/O.

Host fault injection passed all six write/sync boundaries, including torn
journal/main-record writes and invalid recovery cases. The normal repository
loader also now propagates an EFI `flush_blocks` error before readback/CRC.
The temporary loader already did this; this correction does not invalidate
the earlier recorded temporary-image flush results.

| Check | `native-26` create | `native-27` after Android reboot |
|---|---|---|
| Old password and wrong raw volume key | Rejected | Rejected |
| Files read from live backing with no snapshot export | Pass | Pass |
| Durable record updates before clean unmount | 5 | 0 (read-only verification) |
| Authenticated redo repair | Fixture prepared after unmount | 1 record replayed |
| Paired native USB and syslog | Pass | Pass |
| Final data-volume flush | errno 0 | errno 0 |
| Stock Android boot completed | Yes | Yes |

Both generated files compared byte-for-byte. The 128 KiB payload SHA-256 is
`f92550cf2093006319c0c11d7d6afe23923c9b287aa191584ea54b62d87c8d79`.
The first session cleared/reloaded the live device and reverified its files
before preparing the recovery fixture. The second loaded the same bag/file,
replayed the authenticated record and mounted HFS read-only. It did not
regenerate or export the fixture. USB logs contained 110,994 bytes/603 lines
and 111,584 bytes/749 lines respectively. No new trust approval was required.

The failure was deliberately synthesized in an unmounted test filesystem:
a valid redo record was saved, then half of its target main ciphertext was
corrupted and flushed. This is **not a physical power-cut test**. The current
format has no anti-replay, hardware key binding or rollback prevention.
Original iOS data and system keybag were not encrypted by this experiment.

## Native lock-screen integration sequence

The user permits clearing iOS test data and chose native lock-screen unlock,
not a separate boot password. Therefore the next device setup can initialize
fresh iOS data, but must retain the working USB diagnostic/pairing path and
must not alter Android, GPT or boot partitions.

1. Test real bag state (`ss` flags and `sls` lock state), stable UUID,
   failed authentication and buffer validation on independent bags. Do not
   expose a successful system-passcode result from these isolated checks.
2. Add system-bag ownership, initial no-passcode-to-password creation,
   durable save/load and password replacement, then let original `keybagd`
   exercise them in the disposable iOS environment. The lab's per-client
   handles and selector-0x7e opt-in are not a substitute for this lifecycle.
3. Deliver real lock/unlock notifications and implement protection-class
   keys/cache invalidation. The HFS source currently forces `CONFIG_PROTECT=0`,
   does not compile `hfs_cprotect.c`, and the prepared UFS strategy does not
   consume file-protection keys. Changing a Settings flag cannot fix these.
4. Prove native UI password installation, lock/unlock, wrong-password refusal,
   reboot-to-lock-screen, protected file access and unchanged paired USB.
   Only then initialize/encrypt the actual iOS data volume and re-test.

Simply password-encrypting all of `/private/var` before boot would prevent
the current init from mounting the data needed by the native lock-screen
services. The present init mounts `/dev/ufs_data` directly and can fall back
to RAM on failure; the encrypted path must not silently bypass decryption
with that fallback. Boot-available metadata and protected user data need an
explicit lifecycle. Public test passwords must not become a real default.

### Independent state interface result

The isolated [state adapter](experimental/keybag-state/README.md) now answers
selectors `0x11`/`0x23` using the actual independent bag's authentication
state and returns its stable UUID through `0x17`. In `native-28`, the
native IOConnect probe verified flags/lock-state pairs `(1,1)` before first
unlock, `(4,0)` after correct authentication, and `(5,1)` after relocking.
Wrong passwords preserved the locked state; a short output buffer was
refused without overwriting its byte; handle 0 remained inaccessible.
The normal client still reported its original system state, so this does
not claim that the native system lock screen is fixed.

The prior keybag decrypt/client-isolation/non-root-denial probes also passed.
The captured framebuffer showed the iOS home screen, the existing USB
pairing succeeded, and syslog returned 245,760 bytes / 1,265 lines. The final
data-volume flush returned errno 0. Clean reconstruction/idempotent staging,
the prepared kernel build and the ARM64 probe build passed. The new state
fields do not implement a retry delay, attempt counter or hardware security.

After log collection, `native-28` returned to stock Android; ADB confirmed
`sys.boot_completed=1`, product `umi` and MIUI `V14.0.4.0.TJBCNXM`. The phone
was left in Android. No live iOS data was migrated or erased in these tests.
