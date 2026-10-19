# Metal

Разбор: что в iOS 16.5 просит Metal, где проходит граница, которую пришлось бы реализовать, и
стоит ли её переходить. Всё, что ниже, проверено по извлечённой системе
(`ios165/rootfs`, кэш `ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64`), по исходникам порта и
по журналам прогонов; адреса — невыровненные (unslid) адреса этого кэша.

Коротко: **граница — одно имя в реестре IOKit и один бандл с подклассом приватного класса**, а за
ней лежит не драйвер, а компилятор чужого промежуточного языка. Честный путь — не писать Metal, а
знать и показывать, что его нет. Первый шаг — `userland/src/metal_test.m`, он уже сделан.

## 1. Как iOS находит Metal

Вся процедура — пять шагов, и ни один из них не спрашивает ни про возможности устройства, ни про
что-либо ещё, кроме реестра IOKit.

1. `MTLRegisterDevices` (`Metal`, 306.6.4, `0x181b115ec`) берёт `IOMainPort`, затем
   `IOServiceMatching("IOAcceleratorES")` — строка `0x181b74481` — и
   `IOServiceGetMatchingServices`. **Это имя класса и есть всё, чем GPU находится**; других путей
   нет.
2. Для каждой найденной службы вызывается `getMetalPluginClassForService` (`0x181a39ba4`). Она
   читает `IORegistryEntryCreateCFProperty(служба, "MetalPluginName")` и проверяет, что это
   `CFString`.
3. Из ответа складывается путь: `"/System/Library/Extensions"` (`0x181b7441b`) + имя +
   `".bundle"` (`0x181b74436`), и бандл открывается `[NSBundle bundleWithPath:]` (`0x181bc1820`).
4. Читается второе свойство, `"MetalPluginClassName"`; класс берётся
   `[bundle classNamed:]` (`0x181bc1920`), а без этого свойства — `[bundle principalClass]`
   (`0x181bca6c0`). Затем `[класс isSubclassOfClass:_MTLDevice]` (`0x181bc79e0`, ссылка на класс —
   `0x1dcf08f68`): **не подкласс приватного `_MTLDevice` — выброшен**.
5. `[[класс alloc] initWithAcceleratorPort:служба]` (`0x181bc6320`) — и этот объект есть устройство.

Код шагов 2–4 Apple заодно выписала словами: в `AGXGPURawCounter` лежит текст сорвавшегося
`assert` (`0x207d22fff`):

```
((cfMmetalPluginName = (CFStringRef)IORegistryEntryCreateCFProperty(acceleratorPort,
    CFSTR("MetalPluginName"), NULL, 0)) != NULL) && CFStringGetCString(...)
```

На этом телефоне (iPhone10,3, A11) плагин — `/System/Library/Extensions/AGXMetalA11.bundle`:
`CFBundleExecutable` `AGXMetalA11`, `NSPrincipalClass` `AGXPrincipalDevice`, версия 227.6.3; вход —
`-[AGXA11Device initWithAcceleratorPort:]`. Сам исполняемый файл лежит в кэше dyld, а не на диске:
в бандле только `Info.plist`, таблицы счётчиков и девять `.metallib`.

`MTLCreateSystemDefaultDevice` (`0x181a34b3c`) выдаёт одно устройство из этого списка под
`dispatch_once` (сторож `0x1e1efe9b8`, блок `0x181a3442c`, массив `0x1e1efe9a0`). **Пустой список —
ответ, данный один раз и сохранённый до конца процесса, молча**: ни строки в журнал, ни кода
ошибки, ни причины. В iOS 16.5 второго способа спросить нет: `MTLCopyAllDevices` объявлен
`API_UNAVAILABLE(ios)` и в двоичном файле отсутствует.

## 2. Что здесь есть вместо этого

Ядро порта поднимает Adreno и отдаёт его userland'у, но под другим именем:
`iokit/Kernel/RedwoodGPU.cpp` (строки 289–307) регистрирует `RedwoodGPU : IOService`,
`setName("RedwoodGPU")`, `kIOUserClientClassKey = "RedwoodGPUUserClient"`. Ни `IOAcceleratorES`, ни
`MetalPluginName`, ни `MetalPluginClassName`. Значит, `MTLRegisterDevices` обходит пустой итератор,
список устройств пуст, и так — в каждом процессе системы.

То есть **Metal не «ломается» — его просто нет, и порт об этом уже говорит правду**, причём в
единственном месте, куда iOS смотрит. Это важно для выводов ниже: никакого бита возможностей,
которым можно было бы соврать в ту или другую сторону, здесь не существует.

## 3. Кто просит Metal и что с ними происходит

Metal загружают **153 образа кэша** (`ipsw dyld imports … Metal`). Из тех, до которых порт
доходит:

| Кто | Что просит | Что сейчас |
| --- | --- | --- |
| `QuartzCore` — оконный сервер | `CA::WindowServer::EAGLServer::renderer()` (`0x1882e2548`) **сначала** пробует `CAMetalContextCreate()` (`0x18837b718`) и `CA::OGL::new_metal_context()` (`0x18847efcc`), и только если рендерера так и нет — путь EAGLContext/OpenGL ES | Работает без Metal: за `EAGLContext` стоит `userland/src/eagl.c`. При отказе обоих путей CoreAnimation пишет `CoreAnimation: Failed to initialize Metal or OpenGLES contexts!` — в журналах порта ровно **один раз за прогон**, потому что `inject.c` (`my_renderer`) запоминает отказ; без этого попытка повторялась бы каждый кадр |
| `CoreImage` | `MTLCreateSystemDefaultDevice`, затем `.metallib` ядер фильтров | Есть путь без Metal (`CGRenderer`, классы `…CPU`), но есть и строка `No Metal renderer available.` (`0x194c477a5`) |
| `CameraUI`, `CameraEffectsKit`, `CMPhoto`, `CMCapture`, `CameraColorProcessing`, `HDRProcessing`, `RawCamera`, `Portrait`, `AppleDepth`, `DepthProcessorV1` | устройство, очередь, текстурный кэш, свои `.metallib` | `Failed to create default Metal device` (`CameraUI`, `0x1cde2bd26`), `"Error: Metal device must not be NULL"` (`CameraEffectsKit`, `0x1ff99168c` — это `assert`), `Cannot create default metal device` (`AppleDepth`, `0x1e747cd48`) |
| `AppleCVA`, `AppleCVAPhoto`, `CoreAppleCVA`, `Vision`, `MediaAnalysis`, `Espresso`, `MLCompute`, `MetalPerformanceShaders*` | то же | `AppleCVA` (`0x1beeb40cd`): `Assertion failed: device = (id<MTLDeviceSPI>)MTLCreateSystemDefaultDevice()`; `AppleCVAPhoto` (`0x1af45f0c7`): `MTLCreateSystemDefaultDevice() is nil`; `Espresso`: `Metal device not found.` |
| `WebCore`, `WebKit`, `WebGPU`, `libANGLE-shared.dylib` | WebGL и WebGPU идут через Metal | Сам Safari работает: страницы рисует CoreGraphics |
| `SpringBoardFoundation`, `SpringBoardUIServices`, `WallpaperKit`, `PosterBoard`, `PaperBoardUI` | эффекты экрана блокировки | Экран блокировки рисуется; часть эффектов — нет |
| `SceneKit`, `SpriteKit`, `RealityKit`/`CoreRE`, `ARKitCore`, `AssetViewer` | всё | Ничего не получают |

**Главное наблюдение: отказ Metal нигде не слышен.** Во всех 112 журналах прогонов
(`darwin-redwood-sileo2/logs`) единственные строки про Metal — наши собственные две
(`no GPU to draw with…`, `SpringBoard: no GPU draws…`), а в журналах с включённым GL —
одна строка CoreAnimation за прогон. Тринадцать тысяч упоминаний `MTLCompilerService` в журналах к
Metal отношения не имеют: это launchd заводит заглушку XPC-службы в домене каждого процесса,
который слинкован с Metal (`created the implicit endpoint upfront` / `draining messages`), а не
запуски компилятора.

Что порт теряет на отрисовке окон, измеримо иначе: у рендерера OpenGL ES в QuartzCore нет шейдеров
примерно для половины из девяноста «функций» CoreAnimation, тогда как у Metal есть все. На деле
CoreAnimation спросил и получил отказ только по двум из них — 40 `sb3` и 60 `p3c`; остальное, чего
не хватает, он рисует вовсе без программы, а три такие функции плюс уменьшение в восемь раз
`eagl.c` рисует своими программами (см. «GPU» в README). Полную перепись даёт `glcensus=1`; в
журналах, которые есть, он не включался.

## 4. Чем пришлось бы быть реализации

Граница узкая, а то, что за ней, — нет. Размеры (из кэша этого же телефона):

| Что | Байт | Символов | Методов ObjC | Классов |
| --- | --- | --- | --- | --- |
| `AGXMetalA11` — драйвер, который пришлось бы заменить | 2 578 880 | 5 798 | 1 158 | 53 |
| `_MTLDevice` — базовый класс, от которого надо наследоваться | — | — | **549** | 36 ivar'ов |
| `AGXA11FamilyDevice` — сколько из них переопределено | — | — | 236 | — |
| `IOGPU` — обёртка над клиентом IOKit | 461 664 | 2 266 | 711 | 36 |
| `MTLCompiler` — передняя часть компилятора шейдеров | 983 544 | 2 971 | — | — |
| `AGXCompilerCore` — его задняя часть под AGX | 2 680 736 | 7 509 | — | — |
| `AppleMetalGLRenderer` — OpenGL ES **поверх** Metal, как это делает сама iOS | 258 256 | 1 092 | 2 | 1 |

Для сравнения — чем порт заменяет графическую библиотеку сейчас: `userland/src/eagl.c` — 3 685
строк, `src/eagl_slots.txt` — 409 записей таблицы, из которых помечено используемыми QuartzCore
**62**, написано руками 30, и заменяется **шесть** методов ObjC у `EAGLContext`.

### Что ломается первым

Не объём классов, а **язык шейдеров**. Metal получает программы не текстом, а в виде AIR —
битового кода LLVM в контейнере `.metallib`; разбирает его `MTLCompiler`, а в машинный код
превращает задняя часть под конкретный GPU (`AGXCompilerCore`), и делается это в отдельном
процессе `MTLCompilerService`. Сам контейнер версионирован (`This version of metallib is too old
and not supported any longer.`, `0x181b6cccb`), а задняя часть компилятора — тоже плагин со своей
проверкой пути и суффикса (`MTLCompilerPluginInterface::init`). В извлечённом корне девять
`.metallib` на 3,75 МБ, и Metal знает к ним прямые пути — например
`/System/Library/Frameworks/CoreImage.framework/CoreImage.metallib` (`0x181b6c367`).

У Mesa компилятор под Adreno (`ir3`) принимает NIR. **Фронтенда AIR нет ни в Mesa, ни где-либо
ещё**, а обойти его нельзя: всё, что просит Metal — CoreAnimation, CoreImage, MPS, камера — несёт
с собой уже скомпилированный AIR. Значит, первым сорвётся не отрисовка, а самый первый
`newLibraryWithData:` на любом системном `.metallib`, то есть раньше, чем что-нибудь будет
нарисовано.

Второе, что ломается, — и ломается тише: наследование от `_MTLDevice` означает, что мы получаем
его 36 приватных ivar'ов, разметка которых принадлежит сборке Metal 306.6.4 и нигде не описана, а
549 его собственных методов эти ivar'ы читают. Ошибка в разметке — порча памяти внутри чужого
кода, без диагностики.

### Три пути и их цена

**(А) Поверхность Metal над Mesa.** Зарегистрировать в `RedwoodGPU` службу класса
`IOAcceleratorES` с двумя свойствами, положить в образ `/System/Library/Extensions/RedwoodMetal.bundle`
(место открыто: `userland/build.sh` собирает корень из файлов iOS по их настоящим путям и уже
кладёт туда свои — например `/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver`) и реализовать
граф объектов Metal на Mesa. Цена: ~53 класса и ~1 200 методов драйвера **плюс компилятор
AIR→NIR**, которого не существует. В единицах этого проекта — не файл и не два, а ещё одна Mesa, и
худшая её половина — без апстрима.

**(Б) Перевод Metal в тот OpenGL ES, который уже работает.** Строго хуже (А): ту же проблему
`_MTLDevice` приходится решать целиком, AIR остаётся на входе (то есть нужен AIR→GLSL), а
единственное, что экономится, — задняя часть под Adreno, то есть именно та часть, которую Mesa и
так даёт бесплатно. Заодно GLES2/3 не выражает вычислительные конвейеры, argument buffers и
значительную часть того, за чем Metal и зовут. Показательно, что сама iOS переводит в обратную
сторону: `AppleMetalGLRenderer.bundle` — это GLES поверх Metal, один класс и 1 092 символа.
Дешёвое направление — GL→Metal, а у порта уже есть то, что делает его ненужным: настоящий GLES.

**(В) Ни то, ни другое.** Признать, что Metal нет, и добиваться, чтобы выбирались пути без него.
Цена — отдельные файлы и десятки строк, в привычном для проекта виде. У всего, что порту
действительно нужно, путь без Metal есть: у CoreImage — `CGRenderer` и процессоры `…CPU`, у
Espresso и CoreML — процессор и ANE, у декодирования видео — свой блок, а не Metal; у
CoreAnimation — рендерер OpenGL ES, который порт уже и использует.

**Вывод — (В).** Metal здесь не «недоделан», а упирается в чужой промежуточный язык, и это не та
стена, которую обходят упорством: её обходят только компилятором AIR, а он сам по себе больше
всего, что в проекте написано про графику. Частичного Metal при этом не бывает: любая дорога
внутрь проходит через `newLibraryWith…` и объект состояния конвейера, так что «только вычисления»
или «только blit» не спасают.

Ограничение честности: драйвер iPhone X (`AGXMetalA11`) переиспользовать нельзя ни в каком виде —
он порождает поток команд AGX и говорит с ядром по ABI AGX; к Adreno это не имеет отношения.

## 5. Ложный след, который стоит знать

Выглядит соблазнительно: в дереве устройств, которое выкладывает загрузчик, есть
`graphics-featureset-class` = `APPLE4` и
`graphics-featureset-fallbacks` = `APPLE3:APPLE3v1:APPLE2:APPLE1:GLES2,0`
(`xnuloader/identity.h`, скопировано с настоящего iPhone X). Последняя ступень — `GLES2,0`, то
есть у самой Apple под всеми наборами Metal предусмотрена ступень OpenGL ES. Напрашивается
действовать как с цветовым охватом: `loader.c`, `dt_identity()` уже переписывает
`artwork-display-gamut` с `P3` на `sRGB`, чтобы WebKit перестал заводить поверхности по десять бит
на цвет и страницы перестали выходить чёрными (вернуть прежнее — `gamut=p3`).

**Так делать не нужно.** Эти два свойства MobileGestalt публикует как `GraphicsFeatureSetClass` и
`GraphicsFeatureSetFallbacks`, а читают их `libMobileGestalt`, `CoreUI` (`CUICatalog`, ключ
`kMGArtworkGraphicsFeatureSetClass`), `CoreThemeDefinition` и `TextureIO` — то есть **каталоги
скомпилированного оформления**: какой вариант текстуры достать из `.car`. Ни одно решение о том,
чем рисовать, на них не завязано. Понизить класс до `GLES2,0` — значит заставить CoreUI брать
другие варианты картинок, а Metal от этого не появится и не исчезнет. Решение одно и уже
принимается правильно: есть ли служба `IOAcceleratorES`.

## 6. Первый шаг: `metal_test` (сделан)

Поскольку отказ Metal нигде не слышен, а весь план выше держится на утверждениях о том, кто и как
падает, первое, что нужно, — это чтобы система сама говорила, что она про Metal думает.
`userland/src/metal_test.m` (собирается `build.sh`, попадает в `/redwood/bin`, запускается
`RC_JOBS=rc.jobs.metal`) печатает всю цепочку раздела 1 по шагам:

1. службы класса `IOAcceleratorES` и у каждой — `MetalPluginName` и `MetalPluginClassName`
   (сейчас: ни одной, и значит Metal не найдёт устройства вовсе);
2. что в реестре **есть** похожего на GPU — `RedwoodGPU`, `IOGPU`, `AGXAccelerator`,
   `IOAccelerator` — и с каким `IOUserClientClass`: видно, что GPU поднят, но под другим именем;
3. бандлы в `/System/Library/Extensions`: грузится ли принципиальный класс каждого и является ли он
   подклассом `_MTLDevice`, — то есть мог ли бы вообще загрузиться плагин в этом образе, и на каком
   шаге всё обрывается;
4. что получает программа: `MTLCreateSystemDefaultDevice()`, а при удаче — имя, `registryID` и
   поддерживаемые семейства;
5. что iOS всё равно рассказывает про графику (раздел 5): `GraphicsFeatureSetClass`,
   `GraphicsFeatureSetFallbacks` и рядом — чтобы было видно, что ответ `APPLE4` не зависит ни от
   чего;
6. умеет ли работать фреймворк, у которого путь без Metal есть: CoreImage прогоняет `CIColorInvert` по четырём
   пикселям и результат читается обратно. Оранжевый `1; 0,5; 0` должен стать примерно
   `0; 128; 255`. Это единственная здесь проверка, отвечающая на вопрос «может ли эта система
   вообще обработать изображение», а он нужен и камере, и Фото.

Почему именно это и почему отдельно от всего: программа полезна, даже если дальше ничего не
случится (вопрос «не из-за Metal ли это?» будет возникать и у камеры, и у Фото, и у WebGL), она
ничего не меняет в поведении системы, и она же — предпосылка для любой следующей работы: сперва
измерить, потом трогать. Собирается и запускается она и в QEMU, где ответ про устройство тот же,
что на телефоне.

Устроено как остальные диагностики порта (`mg_test`, `cg_test`, `font_test`, …):

```sh
userland/build.sh                       # obj/metal_test -> root/bin/metal_test
XL_CMDLINE="... gpu=1" RC_JOBS=rc.jobs.metal ./xl run
```

## 7. Чего ещё стоит проверить дальше (по убыванию пользы)

- **Переписать перепись.** Прогнать `glcensus=1` на сценариях `rc.jobs.gfx` и `rc.jobs.fps` и
  получить наконец полное число: по каким функциям CoreAnimation отказано и сколько отрисовок идёт
  без программы. Сейчас известно только, что спрошено и отказано по двум (40 `sb3`, 60 `p3c`).
- **Камера и Фото через `metal_test`.** Проверить, проходит ли ядро CoreImage на процессоре, и если
  да — какие именно шаги конвейера снимка (`CMPhoto`, `HDRProcessing`, `Portrait`) всё же требуют
  устройства. Это прямо касается ветки `camera`.
- **Найти, у кого путь без Metal есть, но не выбирается.** Самое вероятное место, где можно что-то
  выиграть малыми средствами: не подделывать Metal, а подталкивать к уже существующему запасному
  пути — по образцу `inject.c`, который заменяет записи в таблицах виртуальных функций и уже
  избавил сервер от попытки Metal каждый кадр.
- **Не начинать (А) и (Б)** без фронтенда AIR. Если он когда-нибудь появится (чужой проект, не
  этот), раздел 4 перечисляет, что останется сделать; до тех пор объём работы не уменьшится.
