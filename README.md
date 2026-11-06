# ROTK for Mac

*The Lord of the Rings: The Return of the King* (original Xbox), running natively on macOS (Apple silicon: M1 and newer).
You need your own copy of the game: this repository and the DMG contain **no game data and no game code** (only our HD texture packs).
[Українською нижче.](#українською)

## Install
1. Open `ROTK.dmg` and drag **ROTK** to Applications (or anywhere you like).
2. The app is not signed by Apple: the first time, **right-click ROTK → Open → Open** (Gatekeeper).
3. Pick your disc image (`.iso`) when asked. ROTK imports it to `~/Library/Application Support/ROTK/data`,
   then builds the game for your Mac with Apple's Command Line Tools (macOS offers to install them if
   they are missing). This takes a few minutes, once.
4. The launcher opens: choose the **language** (English by default, or Українська: menus, subtitles and the
   launcher itself; voices stay original), the frame rate (60 or 120 on ProMotion displays) and the gamma,
   then **Play**. Your choice is remembered.

HD textures for menus, fonts and all 17 levels come with the app and are on by default.

Controls: a gamepad only (Xbox or PlayStation; button prompts match it automatically). Discs: the US (NTSC) disc is tested; EU (PAL): untested.

## Building from source
```
tools/bootstrap.sh                                   # third_party (x86recomp, Zydis, SoftFloat)
lift/lift.sh --detach --pin                          # lift your default.xbe (heavy, minutes)
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build out -j3
tests/run.sh quick
tools/make_dmg.sh out out/dmg/ROTK.dmg               # the DMG: no game data, no game code
```

---

## Українською

*Володар перснів: Повернення короля* (оригінальний Xbox) нативно на macOS (Apple Silicon: M1 і новіші).
Потрібна ваша власна копія гри: у цьому репозиторії й у DMG **немає ні даних, ні коду гри** (лише наші HD-паки текстур).

### Встановлення
1. Відкрийте `ROTK.dmg` і перетягніть **ROTK** у «Програми» (або куди завгодно).
2. Застосунок не підписаний Apple: першого разу **правий клік на ROTK → Відкрити → Відкрити** (Gatekeeper).
3. Оберіть образ свого диска (`.iso`). ROTK імпортує його в `~/Library/Application Support/ROTK/data` і
   збирає гру для вашого Mac інструментами Apple Command Line Tools (якщо їх нема, macOS запропонує
   встановити). Це кілька хвилин, один раз.
4. Відкриється лаунчер: оберіть **мову** (за замовчуванням English, або Українська: меню, субтитри й сам
   лаунчер; голоси оригінальні), частоту кадрів (60 або 120 на дисплеях ProMotion) і гаму, тоді **Грати**.
   Вибір запам'ятовується.

HD-текстури меню, шрифтів і всіх 17 рівнів уже в застосунку й увімкнені за замовчуванням.

Керування: лише геймпад (Xbox або PlayStation; підказки кнопок — автоматично). Диски: американський (NTSC) перевірено; європейський (PAL) — не перевірено.
