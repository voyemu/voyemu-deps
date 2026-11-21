# Сборка

Voyemu собирается CMake и Ninja из `src/app/`, всегда в Release и всегда закреплённым компилятором Zig. Одно дерево исходников собирается для macOS, Windows и Linux; причины — в записях [набор инструментов Zig](../why/zig-toolchain.ru.md), [только Ninja](../why/ninja-only.ru.md) и [только Release](../why/release-only.ru.md). Хост написан на C++20, а код гостя QNX — на C11; выбор объясняет запись [C++ для хоста](../why/cpp.ru.md). Исходники разложены так: `src/app/` — хост на C++, `src/prepare/` — подготовка на Java и её тесты, `src/guest/` — гостевой код Android и QNX, `src/dev/` — инструменты разработчика (среди них диагностика QNX в `src/dev/qnx/`), `src/licenses/` — уведомления сторонних лицензий.

## Инструменты

Сборке нужны Zig 0.15.2, CMake 3.31.10, Ninja и JDK 25 для тестов, проверок и рецептов Java, закреплённые для каждого хоста сборки в `optionalTools.macosBuild`, `optionalTools.windowsBuild` и `optionalTools.linuxBuild` файла `src/app/toolchain.json`. Каждый пин называет URL, SHA-256 и каталог в `.local/toolchains/`, куда распаковывается архив. Обёртки компилятора в `src/dev/toolchains/` запускают Zig из этого каталога, поэтому системный компилятор не участвует. Кеш Zig обёртки держат в `.local/build/zig-cache/`, если вызывающий не задал `ZIG_GLOBAL_CACHE_DIR`. Порядок установки: установить инструменты своего хоста по этим пинам в указанные каталоги и проверить SHA-256 каждого; распаковать архивы Zig и CMake без каталога верхнего уровня, чтобы `zig` или `zig.exe` лежал прямо в `.local/toolchains/zig-0.15.2`; добавить в `PATH` каталог Ninja и каталог `bin` CMake (на macOS — `CMake.app/Contents/bin`). На macOS сборке нужен ещё SDK из Command Line Tools, которые устанавливает `xcode-select --install`; Xcode не требуется. Файл набора инструментов macOS берёт SDK из `xcrun --sdk macosx --show-sdk-path`, а `-DVOYEMU_MACOS_SDK=<path>` выбирает другой `MacOSX.sdk`. Подготовка машин этот JDK не использует: она работает в среде Java, которую устанавливает `deps`, см. [CI](ci.ru.md).

## Настройка и сборка

```sh
cmake -S src/app -B .local/build/native -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/src/dev/toolchains/zig-macos-arm64.cmake" -DBUILD_TESTING=ON
cmake --build .local/build/native
```

Команды выполняются из корня проекта; на Windows — в PowerShell с `zig-windows-x64.cmake`, на Linux — с `zig-linux-x64.cmake`. `-DBUILD_TESTING=OFF` собирает только лаунчер. Каждая настройка передаёт `-G Ninja` и файл набора инструментов Zig. Другой генератор CMake отклоняет, поэтому дереву, настроенному другим генератором, нужен новый каталог сборки. Тип сборки всегда Release. С деревом сборки `.local/build/native/` лаунчер попадает в корень проекта, с любым другим остаётся внутри дерева.

| Файл набора инструментов | Цель |
| --- | --- |
| `zig-macos-arm64.cmake` | macOS на Apple Silicon с SDK из Command Line Tools |
| `zig-windows-x64.cmake` | Windows x64 с MinGW-w64, на самом Windows или кросс-сборкой на Linux |
| `zig-linux-x64.cmake` | Linux x86-64 с базой glibc 2.34 |

База glibc 2.34 — самая старая glibc среди поддержанных дистрибутивов Linux: в RHEL 9 версия 2.34, в Ubuntu 22.04 — 2.35, в Debian 12 — 2.36. Каждый из них поставляет libcurl и OpenSSL 3, которые лаунчер загружает при запуске вместо собственной библиотеки TLS; см. [deps](../how/preparation.ru.md#deps).

Файлы набора инструментов отключают отладочную информацию и файлы PDB. Диагностика идёт по [логам](../how/logs.ru.md), а не в отладчике.

## Оптимизация

Release компилирует каждую цель с `-Os`, включая закреплённые сторонние исходники; флаги задаёт `src/app/CMakeLists.txt`. Каждая функция и каждый объект данных получают свою секцию (`-ffunction-sections -fdata-sections`), поэтому компоновщики отбрасывают всё, на что нет ссылок: ld.lld — по `--gc-sections`, который Zig передаёт всегда, lld-link — по своему умолчанию `/OPT:REF`, компоновщик Mach-O из Zig — по отдельным символам. lld-link по умолчанию ещё и сливает одинаковые функции (`/OPT:ICF`). На Linux и macOS исходники на C — это только закреплённые SDL3, curl, nghttp2 и miniz — компилируются без таблиц раскрутки (`-fno-asynchronous-unwind-tables -fno-unwind-tables`): через них не должно проходить ни одно исключение, поэтому каждый их обратный вызов в код на C++ объявлен `noexcept` или перехватывает свои исключения.

Исходники, которые обрабатывают каждый пиксель или байт кадра, компилируются с `-O2`. `voyemu_frame_path_sources` перечисляет преобразование кадров QNX, графический обработчик, поток кадров Android, декодер PNG окна, загрузочную анимацию и стартовые ролики; за ними следуют исходники SDL3 с stb_image, поверхностями, форматами пикселей и блиттерами. Каждый исходник с покадрового пути окна входит в этот список. Сборка, настроенная с `-DBUILD_TESTING=OFF`, — так релиз собирает лаунчер — компилируется и компонуется с полной оптимизацией при компоновке (`-flto=full`) на Linux и Windows; сборки с тестами и macOS компонуются без неё, см. [Release с оптимизацией размера](../why/size-optimized-release.ru.md).

Встроенные в лаунчер данные — каталог профилей, файлы проекта из `src/app/project_files.cmake`, шрифты и изображения окна — хранятся как потоки deflate без заголовка, которые пишет `src/app/embedded_deflate.cmake` средствами CMake; лаунчер распаковывает каждый из них через miniz при первом обращении.

## Параметры

| Параметр | По умолчанию | Действие |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Собирает тесты и регистрирует их в CTest |
| `VOYEMU_BUILD_CLUSTER` | `ON` | Собирает окно на SDL3 |
| `VOYEMU_BUILD_GRAPHICS` | `ON` | Собирает графический обработчик QNX с ANGLE |
| `VOYEMU_EXECUTABLE_OUTPUT_DIRECTORY` | См. выше | Куда записывается лаунчер |
| `VOYEMU_JDK` | Закреплённый JDK хоста | Каталог JDK для тестов и проверок Java |

## Исходники зависимостей

Сторонние исходники скачиваются при настройке в кеш сборки и никогда не попадают в дерево исходников. Dear ImGui, nlohmann/json, miniz, lzokay и заголовки libarchive закреплены в `src/app/dependencies.lock.json`, SDL3 — в `src/app/platform/component.json`. На Linux тот же файл закрепляет в `window_linux` исходники X11, Wayland, xkbcommon и libdecor: с их заголовками компилируются драйверы X11 и Wayland в SDL3. Там же закреплён expat для `wayland-scanner`, который настройка собирает из исходников Wayland. Сами библиотеки SDL3 загружает из системы при открытии окна, поэтому сборка ничего не берёт с хоста сборки. CMake сверяет каждый файл с пином до сборки; см. [пины](pins.ru.md).

Сборка Voyemu не компилирует ядра гостей. Ядра разработки — ресурсы релизов, собранные в CI; `fetch` скачивает их по пинам на любом хосте, см. [рабочий процесс kernels](ci.ru.md#рабочий-процесс-kernels).

Движок QNX, помощники e2fsprogs, гостевые двоичные файлы QNX и нативные компоненты Android — тоже ресурсы релизов, собранные в CI. `deps` скачивает движок по пину `prebuilt` из `src/app/qnx-host.json` и на macOS и Linux собирает его по рецепту, только пока этот пин не внесён; подготовка скачивает помощников по пинам `e2fsprogs.helpers`, гостевые двоичные файлы — по пинам `qnxGuestModules`, а нативные компоненты — по пинам `androidNativeComponents` из `src/app/toolchain.json`. См. [рабочий процесс qnx-host](ci.ru.md#рабочий-процесс-qnx-host), [рабочий процесс e2fs-helpers](ci.ru.md#рабочий-процесс-e2fs-helpers), [рабочий процесс qnx-guest](ci.ru.md#рабочий-процесс-qnx-guest) и [рабочий процесс android-native](ci.ru.md#рабочий-процесс-android-native).

## Сборка для Windows на Linux

`zig-windows-x64.cmake` собирает `voyemu.exe` на Linux теми же командами с инструментами `optionalTools.linuxBuild`, например в `.local/build/windows`. CI запускает выбранные тесты этой сборки на Linux под Wine, а также тесты в отдельном задании на Windows; Wine не заменяет нативную приёмку Windows, см. [CI](ci.ru.md).
