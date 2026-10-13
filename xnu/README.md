# Ядро

Наши правки XNU `xnu-8796.121.2` (ядро iOS 16.5, Darwin 22.5.0):

- `redwood.patch` — изменения в файлах Apple (60 файлов);
- `files/` — новые файлы по их путям в дереве XNU (37 файлов);
- `apply.sh` — берёт исходники у Apple и накладывает всё это; `export.sh` — обратное действие.

Собирается на Linux стоковым clang, без macOS и Xcode.

## Сборка

Нужно:

- пакеты (Arch): `sudo pacman -S --needed base-devel git curl clang lld python perl xz`;
- [theos](https://theos.dev/docs/installation-linux) в `~/theos` (или в `$THEOS`): из него берутся cctools
  (`toolchain/linux/iphone/bin`: линкер ld64, nm, otool, strip…) и `sdks/iPhoneOS16.5.sdk`.

```sh
xnu/apply.sh             # ~6 с: тег xnu-8796.121.2 из apple-oss-distributions/xnu + наши правки
                         #        → xnu-src/xnu-8796.121.2, ветка redwood, правки не закоммичены
xnu-deps/setup.sh        # ~30 с: исходники зависимостей, mig/unifdef/tcsh, SDK-надстройка, две библиотеки
./xnu-build.sh -j8 -k    # ~45 с на 12 потоках
```

Результат: `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
`./xl run` и `qemu/run-ios.sh` берут ядро оттуда сами (а `prebuilt/` — только когда его нет).

Проверено: в пустом каталоге, где лежало только содержимое репозитория, эти три команды дали ядро того же
размера и с тем же набором символов (49 407), что и рабочее. Диск: ~600 МБ, не считая theos.

Что нужно знать:

- **`-k` обязателен.** Единственная ошибка сборки — установка lldbmacros (make завершается с кодом 2) — безвредна.
- В пути к репозиторию не должно быть пробелов (makefile'ы XNU их не переживают).
- Обёртка `xnu-deps/host/bin/xnu-clang` зовёт `/usr/bin/clang`. Проверено с clang 22.1.8.
- После правки заголовков ядра, которые видят две внешние библиотеки (pthread kext, firehose):
  `xnu-deps/setup.sh --rebuild-libs`.
- DriverKit-генератор Apple (`iig`) закрыт. Вместо него — заглушки `xnu-deps/iig-impl/OSObject.iig.cpp`,
  сгенерированные по списку неопределённых символов линкера. Если набор изменится:
  `xnu-deps/gen-iig-stubs.py xnu-deps/iig-impl/undefined-all.txt xnu-deps/iig-impl qemu/kernel.syms`, затем `rm -rf BUILD`.
- Для `qemu/sym.py` после пересборки: `nm -n …/mach.development.bcm2837.unstripped > qemu/kernel.syms`.

## Работа с деревом

Правки делаются прямо в `xnu-src/xnu-8796.121.2` (обычное git-дерево Apple на ветке `redwood`). Перед коммитом
в этот репозиторий — `xnu/export.sh` (перегенерирует патч и `files/`), а чтобы обновить готовое ядро в
`prebuilt/` — `tools/update-prebuilt.sh`.

## Что добавлено в ядро

| Файлы | Что |
|---|---|
| `osfmk/arm64/gic_v3_irq.c`, `generic_arm64_support.c`, `kpc.c`, `ppl/`, `tunables/` | платформа: GICv3, таймер, заглушки вырезанных Apple файлов |
| `osfmk/arm64/psci_smp.c` | несколько процессоров: запуск ядер через PSCI (boot-arg `cpus=N`), их остановка перед возвратом в загрузчик |
| `osfmk/arm64/loader_return.c` | возврат в загрузчик с журналом, «сердцебиение», сторожевой таймер гипервизора Qualcomm |
| `osfmk/arm64/user_exception_trace.c` | отладка userland: `usertrap:`, `userabort:`, стеки потоков (`ustacks`) |
| `osfmk/minicrypto/` | замена corecrypto: MD5/SHA-1/SHA-2, HMAC, DRBG, AES (исходники Apple corecrypto не использованы) |
| `iokit/Kernel/GenericARMPE.cpp` | эксперт платформы и CPU |
| `iokit/Kernel/MiniIOSurface.*`, `MiniIOMobileFramebuffer.cpp`, `MiniAppleKeyStore.cpp` | замены закрытых драйверов: поверхности, экран (с масштабированием в панель и записью экрана), хранилище ключей |
| `bsd/miscfs/packfs/`, `bsd/miscfs/ramfs/` | свои ФС: сжатый образ только для чтения (корень) и записываемая ФС в памяти |
| `bsd/kern/kern_minimac.c`, `libkern/amfi/` | заглушки AMFI и Sandbox (MAC-политики), права из подписи программы |
| `bsd/dev/redwood_input.c`, `redwood_goodix.c`, `redwood_virtio_input.c` | ввод: `/dev/redwood-touch`, драйвер Goodix GT9916 (SPI бит-бангом по GPIO), virtio-input для QEMU |
| `bsd/dev/redwood_cpufreq.c` | частота процессора (Qualcomm EPSS) |
| `bsd/dev/redwood_net.c`, `bsd/sys/redwood_net.h`, `redwood_virtio_net.c` | сетевой интерфейс `en0` через сетевой KPI ядра (без IONetworkingFamily) и граница между ним и тем, что носит кадры («линк»: три функции в `redwood_net.h`); первый линк — virtio-net для QEMU |
| `bsd/dev/redwood_adreno.c`, `iokit/Kernel/RedwoodGPU.cpp` | GPU телефона (Adreno 642L): пошаговое включение (`adreno=N`: питание, GMU, zap-шейдер, микрокод), буферы и очередь команд; клиент IOKit с подмножеством msm DRM для Mesa |
| `bsd/pthread/pthread_kext_static.c` | pthread kext, слинкованный в ядро |

Патч, помимо врезок под всё это, чинит то, что мешает собрать и запустить открытый XNU вне Apple: makefile'ы под
Linux, конфигурацию `BCM2837` (она взята за основу), ошибки в заброшенном коде (mockfs, не-PPL pmap, percpu на одном
процессоре) и т. п.

Лицензия: производная работа от XNU, APSL 2.0.
