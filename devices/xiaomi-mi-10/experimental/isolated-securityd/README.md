# 原生 SecItem 与独立钥匙串数据库实验

本实验接续[原生保存事务](../native-keybag-transaction/README.md)与
[原厂 AKS 隔离客户端](../ordinary-aks/README.md)。目标是让原生 `SecItem`
访问独立数据库及实验密钥袋，最终验证保护类、改密、重启和旧条目兼容。
**已通过独立数据的原生 CRUD、A/C 保护与磁盘跨重启读取；尚未迁移系统钥匙串，
也未完成整个 iOS 数据卷加密。**

后续 [保存事务组合验证](TRANSACTIONS.md) 的 `native-94/95` 已通过：原厂诊断
密码服务改密、保存失败和丢失回执后，独立磁盘钥匙串仍能读取最初的 A/C 条目；
跨安卓重启保留失败计数，再次改密也不丢失条目。它仍使用公开合成数据，未切换
真实密码服务、实时认证通知或系统钥匙串，原生 USB 配对留待集中回归。

## 原生调用路由

本机 iOS 16.5 的 `Security.framework` 导出 `gSecurityd`，原厂服务自身也使用
该函数表处理本进程 `SecItem` 调用。测试子进程先取得这些符号，确认函数表为空，
再设置仅返回拒绝结果的四个回调。首先用合成属性执行只读查询，证明进入回调后，
才调用新增、修改和删除 API。四种调用均须得到预期错误且恰好到达各自回调一次。
这比只拦截共享缓存之外的用户态符号更容易证明调用实际进入哪条路径。

`SecSetCustomHomeURL` 和 `SecCopyBaseFilesURL` 的进程内测试覆盖用户/系统两种
基础目录。未带这些修改的父进程对照自身函数表和目录，并对照真实系统袋的加密
序列化内容及状态，确认子进程没有改变它们。
**改基础目录本身不构成完整文件隔离**；原厂数据库代码的下一层预检还使用 `chroot`。

`native-85` 已通过上述原生四种 API 的拒绝回调和父进程对照。
本轮没有创建条目、访问真实条目内容，或对真实系统袋尝试认证。

## 原厂数据库代码的独立执行

`prepare-backend.py` 只接受 SHA-256 为
`d94e7307b196107572b44eb4bb4aeefbd7446a3719b447132ae9c915bb67d4d9`
的本地 iOS 16.5 `securityd`。给诊断副本添加必须加载的测试库，并将原 `main`
入口改成陷阱；测试库退出前不启动原来的监听、后台维护或云服务主流程。
输入 Apple 二进制保持不变，不加入 Git。

为了不扩大已验证的临时 RAM 启动包上限，helper 只携带变化块。父进程读取系统中的
原文件，先验证原始哈希，再在独占新建的 `/var/tmp/kNN` 中重建诊断副本，验证完整
输出哈希后才执行。变化块也有编译进父进程的哈希、长度与边界检查。它不是固件刷写。

诊断库按顺序验证：

1. 原厂 AKS 框架已进入带 entitlement 的内核隔离客户端；只读回执必须匹配。
   在任何映射之前必须确认默认袋 0 返回错误，不能回落到真实系统袋。
2. 原厂数据库函数表的前四个入口与固定版本的地址一致。
3. 进入独占测试目录的 `chroot` 并切换工作目录；确认原系统可执行文件路径不可见、
   独立 `Library/Keychains` 存在且权限正确，随后才调用数据库代码。
   因此数据库、WAL、SHM 及其它通过普通文件路径打开的恢复文件都受测试根目录限制。
4. 通过进程内函数表执行原生 `SecItem`。本阶段没有注册第二个 XPC 服务，
   不能宣称跨进程客户端到独立 `securityd` 的协议链路已经通过。

`native-86` 已通过上述范围检查；对不存在的合成条目返回 `errSecItemNotFound`
（-25300），没有创建主数据库。`native-87` 随后尝试新增公开合成数据，返回
`errSecNotAvailable`（-25291）；测试目录只出现 Analytics 辅助目录，主数据库未创建。
两轮父进程均确认真实系统袋未变。

## 默认袋与原厂封装接口

后续的错误链定位了三个独立问题，不能将前面的预检当作 CRUD 已通过：

- `native-88`：底层是 class 6、bag 0 的 `ks_crypt` 返回 `e00002c2`。
  原厂服务使用默认句柄，初版隔离客户端只接受非零实验袋。
- `native-89`：新增显式的客户端内默认袋映射。无效目的句柄、无效源句柄、锁定、
  错误公开测试口令、卸载和新袋不自动接管均通过；但原厂条目写入仍失败。
  原因是服务静态链接的 AKS 与共享缓存框架拥有各自的 IOKit 客户端。
- `native-90`：通过固定版本的内置连接函数取得第二条连接，先验证隔离回执，
  再加载同一份合成袋的加密序列化内容。两条连接的封装输出逐字节相同；
  原厂 Add 的底层错误变为 `e00002db`（空间不足）。原实验 `UKW2` 需要
  16 字节额外空间，而原厂接口只预留 AES-KW 的 8 字节。此轮 Add 失败，
  后面的 CRUD 没有执行，不能记作成功。

`stage-default-bag.py` 仅接受精确的 v15 实验源码，添加 `aks_set_system(h, 0)`
的客户端内映射。需要已解锁的非零实验袋；卸载清除映射，下一袋不会自动继承。
真实系统袋与没有实验 entitlement 的客户端仍走原有分派。两个测试连接各自有袋，
不通过共享可变全局状态传播认证。内置连接、封装入口及原厂函数表地址见
[abi.json](abi.json)，只适用于固定哈希并加 dyld slide 的原文件。

`stage-native-wrap.py` 在该版本之上加入 `umi_native_item_keys.h`。只有显式绑定
的句柄 0 使用原生长度的 RFC 3394 封装；非零实验句柄保留 `UKW2`。
两种格式使用不同的派生域，不通过猜测密文或解密失败后回退来选择格式。
该实现仍只支持 A/C（6/7）类，不提供硬件绑定、Secure Enclave 或防回滚保证。

`native-wrap-test.c` 在 Mac 的 ASan/UBSan 下同时运行旧格式检查和新格式检查：
覆盖 16–64 字节密钥、逐字节损坏、错误袋/类别、容量边界、两种格式交叉拒绝、
A/C 锁定语义与重启状态清除。宿主测试已通过；实机结果以
[results.json](results.json) 和下文为准。

从仓库根目录可复现宿主检查（输出路径自行选择）：

```sh
clang -std=c11 -O2 -g -Wall -Wextra -Werror \
  -Wno-unused-function -Wno-deprecated-declarations \
  -fsanitize=address,undefined \
  devices/xiaomi-mi-10/experimental/isolated-securityd/native-wrap-test.c \
  devices/xiaomi-mi-10/experimental/native-keybag-transaction/item-keys.c \
  -o /tmp/umi-native-wrap-test
/tmp/umi-native-wrap-test
```

## 构建与边界

`build-preflight.py` 构建拒绝回调与父进程；`build-backend.py` 构建原厂代码诊断库、
变化块与父进程。`--write-probe` 仅允许在独占测试根目录中尝试公开合成条目。
两个准备 helper 的脚本都要求新输出目录，保留原密码、USB 和系统 securityd 任务。
内核按普通 AKS 的 `stage.py`、本目录 `stage-default-bag.py`、`stage-native-wrap.py`
顺序准备；后两个脚本只接受各自固定哈希的输入。新增实验源只在一次性构建树中使用。
启动前检查 APFS 全部条目，并解压实际 boot 产物验证嵌入的内核/helper；每轮结束
正常同步并回原厂安卓。原始系统日志、签名副本及 Apple 文件只保留在本机工作目录。

本实验没有迁移真实数据库，没有认证真实密码，没有启用新的日常密码策略或加密
80 GiB。USB 源码未变，当前保留固件日志通道，原生配对仍需后续集中回归。
不能由 `native-85` 至 `native-90` 的预检推导 CRUD 或持久化成功；后续实测见下文。

## 原生 CRUD 与持久化的区别

`native-91` 首次通过原厂 `SecItemAdd / CopyMatching / Update / Delete` 的实际操作，
重复新增返回 -25299，删除后返回 -25300。锁定后 A 类读取返回 -25308 且无数据，
C 类仍能读取；错误合成口令不能恢复 A 类访问，正确口令后可读。两个实验客户端
独立卸载后默认句柄均失效，真实系统袋的加密序列化与状态一致。

这一轮**使用内存数据库**，没有证明磁盘持久化。固定版本在检测到
`__OSINSTALL_ENVIRONMENT` 时使用 `file::memory:?cache=shared`。
持久化模式只修改诊断副本中这一处分支，让它选用原来计算的磁盘路径，保留该
环境变量的其它行为与原 main 陷阱；chroot 必须在数据库访问前建立。
`--persistence create/verify` 同时要求两个客户端隔离和 CRUD 模式。
创建轮使用新目录，保存加密测试袋及 A/C 条目；验证轮必须找到同一个目录，
且诊断可执行文件与本轮固定哈希完全一致，不能覆盖已有可执行文件掩盖不匹配。

磁盘检查要求独立 SQLite 文件存在、权限 0600、完整性检查返回 `ok`。
数据库、WAL 和加密袋中不得出现公开合成内容的完整明文标记。该检查不是对所有
文件内容或原始 UFS 扇区的保密性证明，也不等同于 80 GiB 数据卷加密。

`native-92` 在 `/var/tmp/k92` 成功创建磁盘数据库及两条持久 A/C 记录，保存
136 字节加密测试袋，并通过原生 CRUD、锁定策略、SQLite 完整性和明文标记检查。
经过一次原厂安卓启动，`native-93` 使用同一目录与同一加密袋：加载状态为尚未认证，
错误公开测试口令被拒绝，正确口令后绑定默认袋；原有 A/C 条目读回与预期完全一致。
新一轮 CRUD、重锁策略和数据库完整性再次通过。首次认证前检查在 AKS 层进行，
此时没有绑定默认袋；不能把它表述成已测试真实系统首次解锁前的 SecItem 行为。

所有轮次都核对真实系统袋内容和状态，最终同步返回 `errno=0` 后回原厂安卓。
原生 USB 配对本轮未重测。失败轮保留在 [results.json](results.json)，没有计入成功。
日志中的 `encryption NOT QUALIFIED` 表示诊断工具未批准切换真实钥匙串/数据卷，
不应将它改读为整卷加密已经通过。

保存事务与持久失败策略的独立组合验证已在 `native-94/95` 完成。后续的
[认证与迁移验证](LIFECYCLE.md) 覆盖独立认证通知、保护期间拒绝关闭密码、旧条目
事务回滚和跨重启读取。接入正式服务还需处理其它保护类、关闭密码的数据处置，
以及真实旧数据来源核对和数据库迁移。

源码参照为 Apple 的
[客户端分派实现](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/OSX/sec/ipc/client.c)、
[服务端函数表声明](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/OSX/sec/ipc/securityd_client.h)
及[数据库入口](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/keychain/securityd/SecItemServer.c)。
该公开源码版本不等同于手机二进制；实际地址/接口以固定哈希的本地反汇编和实机预检为准。
