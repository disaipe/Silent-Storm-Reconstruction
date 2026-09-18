# Сборка под Linux (в разработке)

Этот документ описывает экспериментальную Linux-сборку движка `A5` (Silent Storm Reconstruction). Статус и обоснование решений — см. [`linux-port.md`](linux-port.md). Windows/MSVC-сборка (см. корневой `README.md`) не затрагивается: `CMakeLists.txt` ветвится по платформе (`if(WIN32)`/`if(UNIX)`), поведение под Windows не меняется.

## Статус (обновлено 2026-09-18, все библиотечные модули собираются)

Ветка `linux-port` (от `develop`). Сборка проверена компилятором (`g++`/GCC 16) в рабочем окружении: **`cmake --build` проходит полностью, без ошибок**, все цели по умолчанию собираются в статические библиотеки. Осталось подключить рендер — `Main`/`Game` всё ещё выключены.

| Подсистема | Статус на Linux |
|---|---|
| `Misc`, `FileIO`, `Image` | **Собираются** |
| `MiscDll`, `Script`, `DBFormat`, `ADOImport`, `FModSound`, `Input` | **Собираются** |
| БД (`ADOImport`) | Собирается (`BasicDBLinux.cpp`) — только чтение готового `game.db`, COM/ADO-часть недоступна (см. [linux-port.md](linux-port.md#разбор-этапа-бд-adoimport)) |
| Звук (`FModSound`) | Собирается — **заглушка (no-op)**, `FMSoundStub.cpp` |
| Видео (`Bink`) | Код готов — **заглушка (no-op)**, `GBinkPlayerStub.cpp` (собирается в составе `Main`, т.е. ещё не проверен) |
| Ввод | Собирается — минимальная SDL2-реализация (`InputSDL2.cpp`), только чтобы приложение стартовало — не полноценный порт биндов |
| Окно/точка входа | Код готов — `Game/WinFrameSDL2.cpp` + `Game/MainLinux.cpp` (ещё не проверены компилятором — часть `Game`) |
| `Main`/`Game` (рендер) | Целенаправленно выключены (`S2_LINUX_FULL_ENGINE=OFF` по умолчанию) — `Gfx*.cpp` ещё содержит сырой D3D9, dxvk-native не подключен |

### Следующий шаг: `Main`/`Game` и рендер

Все библиотеки-зависимости готовы, поэтому дальше — включать `S2_LINUX_FULL_ENGINE=ON` и разбирать `Main`. Ожидаемый объём работ там принципиально больше: `Gfx*.cpp` содержит сырой D3D9, нужен dxvk-native; плюс `Main` — самый большой модуль, и тех же MSVC-измов (см. ниже) в нём наверняка ещё много. Ближайшая осмысленная цель — окно + очистка экрана через dxvk-native.

### Win32-шим (`Misc/PlatformCompat.h` / `.cpp`)

Единая точка, куда добавляются недостающие Win32-символы вместо россыпи `#ifdef` по движку. Сейчас покрывает: типы и MSVC-ключевые слова, `MessageBox`/`OutputDebugString`, таймеры, критические секции, потоки, `CreateEvent`/`SetEvent`/`ResetEvent` (на `condition_variable`), `LoadLibrary`/`GetProcAddress` (на `dlopen`), `WideCharToMultiByte`/`MultiByteToWideChar` (на `iconv`; `CP_ACP` = CP1251, как и в retail-сборке с русской локалью), `FindFirstFile`/`FindNextFile`/`FindClose` (на `opendir`+`fnmatch`, регистронезависимо — под mixed-case ассеты), `itoa`/`_itow`, `stricmp`, `_wtof`/`_wtoi`/`_wtol` и MSVC-перегрузки `swprintf`/`vswprintf` без размера буфера.

### Устранённые MSVC-измы

Все правки приняты и MSVC — Windows-сборка не менялась по поведению:

- **Анонимные union/struct с нетривиальными членами** (`Geom.h`: `SPlane`/`SHMatrix`/`CQuat`; `aiPosition.h`: `SMove`). Заменены на именованные поля + аксессоры; в `SMove` дублирующая пара `first`/`second` нигде не использовалась и удалена.
- **Forward-declared enum без базового типа** — 101 объявление и 47 определений теперь пишутся как `enum E... : int`. Базовый тип (а значит и размер каждого сериализуемого enum-поля) остаётся тем же, что был у MSVC.
- **Явные специализации внутри класса** (`Streams.h`) — заменены обычными перегрузками.
- **Двухфазный поиск имён**: `typename` на зависимых итераторах (`BasicChunk1.h`, `DataFormat.h`), `this->` на членах зависимой базы (`basic2.h`, `BasicDB.h`).
- **`name##::`** в макросах регистрации (`BasicFactory.h`, `BasicDB.h`) — склейка идентификатора с `::` не образует валидный препроцессорный токен.
- **Указатели, приведённые к `int`** (`BasicFactory.h`, `lsaver.cpp`) — через `intptr_t`. Формат сохранений не меняется: на диск попадает только `CLuaFuncID`.
- **Глобальная `random`** (`RandomGen.h`) конфликтует с POSIX `random(3)` — на не-Windows переименована макросом в `s2_random`, все места вызова не тронуты.
- **Пути в `#include`** — обратные слэши приведены к `/`, регистр каталогов исправлен под реальные имена на диске.
- **Мёртвый код** `MinMaxTiles<>`/`MinMaxTiles4Solids<>` (`DataMap.cpp`) удалён: шаблоны нигде не инстанцировались и обращались к несуществующим полям `CTemplVariant` — MSVC не проверял неинстанцированные тела.

## Требования

- **CMake ≥ 3.21**
- **GCC или Clang** (проверено на GCC 16)
- **SDL2** (dev-пакет, например `sdl2` в Manjaro/Arch, `libsdl2-dev` в Debian/Ubuntu)
- **Vulkan loader + заголовки** (`vulkan-icd-loader`, `vulkan-headers` в Manjaro/Arch)
- **meson + ninja** — нужны для сборки [dxvk-native](https://github.com/Gcenx/dxvk-native) (слой трансляции D3D9 → Vulkan, на котором строится рендер)
- **OpenAL** — зарезервировано под будущую замену звука (сейчас не обязательно, звук — заглушка)

Установка на Manjaro/Arch:
```
sudo pacman -S cmake gcc sdl2 vulkan-icd-loader vulkan-headers meson ninja openal
```

## Сборка

```
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux -j$(nproc)
```

Результат: статические библиотеки (`libMisc.a`, `libFileIO.a`, `libImage.a`, `libMiscDll.a`, `libFModSound.a`, `libADOImport.a`, `libInput.a`, `libScript.a`, `libDBFormat.a`) появятся в `build-linux/`. Исполняемый `Game` пока не собирается — см. «Следующий шаг» выше.

## Ассеты

В репозитории нет игровых файлов. Для запуска рядом с `Game`-бинарником нужны:
- `game.db` — бинарный кэш игровых данных (собирается штатно на Windows-стороне пайплайна из `DataImport.exe`, либо копируется из существующей Windows/Steam-установки игры).
- каталог `res/` — ресурсы игры (модели, текстуры, звуки, видео) из той же установки.

Без них бинарник запускается, но не сможет загрузить игровые данные.

## Известные ограничения на этом этапе

- Звук и видео отключены (заглушки) — см. [linux-port.md](linux-port.md) для плана их полноценного порта (OpenAL, FFmpeg).
- Ввод — минимальный, без полного маппинга клавиш/джойстиков.
- Рендер — не гарантирует полноценную 3D-сцену на первых итерациях; ближайшая цель — открыть окно и очистить экран через dxvk-native.
- Инструменты содержимого (`DataImport`, `ShaderCompiler`, `PkgBuilder`, `FontGen`, `TexConv`, `TexMipStrip`) на Linux пока не собираются — они не нужны для запуска уже собранного контента.
