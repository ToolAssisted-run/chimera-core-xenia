# chimera-core-xenia

An Xbox 360 core for [Chimera](https://github.com/ToolAssisted-run/chimera),
the frontend for tool-assisted speedruns. It is built on
[Xenia Canary](https://github.com/xenia-canary/xenia-canary) and produces one
file, `xenia.chimeraCore`, that goes in Chimera's `Cores` folder.

## Status

Early. One game has been tried, Mirror's Edge, and it runs from power-on
through its menus into the first level, with picture, sound, controllers and
savestates. Nothing else has been tested.

- The picture is drawn by Xenia's Vulkan renderer on your graphics card,
  through Chimera's Vulkan bridge. It needs a Chimera build that has the
  bridge and a card with a Vulkan driver.
- It is slow. With the default CPU Speed setting a game that keeps the
  processor busy runs at roughly a quarter of real time on a fast computer
  (CPU Speed 500 is about four times faster, for games that load at it).
- What a game saves can be exported from Chimera, but a save cannot be put
  in before the game starts yet.
- The host processor must have AVX2 and FMA.
- A game that tells the console to end it, or to start another program,
  stops there: the picture stays and nothing more happens. The core runs
  one program.

## What you bring

A disc image (`.iso`) of a game you own, or an Xbox Live Arcade game: one
file with no extension and a long name of letters and digits, as it sits in
a console's storage under `Content/0000000000000000/<title>/000D0000`. No
Arcade game has been tried yet (see Status), and Games on Demand, which are
a small file beside a folder of data files, are not supported. The console's
own system software is not needed. Xenia Canary's
[compatibility list](https://github.com/xenia-canary/game-compatibility)
says which games work in Xenia itself.

## Settings

- **Renderer**: `vulkan-hw` draws on the graphics card. `null` draws nothing
  and needs no card; the game runs exactly the same, with a black picture.
- **Controllers**: how many of the four controllers are plugged in.
- **CPU Speed**: how many million instructions the console's processor runs
  in a second. 2000, the default, is the closest to a real console; some
  games never finish loading below it (Prince of Persia Classic). 500 and
  1000 run faster on your computer, for a game that loads at them. A movie
  made at one speed does not play back at another.
- **Arcade Games**: for Xbox Live Arcade games only. `full` tells the game
  it was bought; `trial` makes it its free trial version. A disc game is
  not affected.

## Building and testing

[docs/BUILDING.md](docs/BUILDING.md) has every step. [docs/PLAN.md](docs/PLAN.md)
is the design log: what was decided, why, and what was measured.

## Licence

This repository's own code is MIT. Xenia Canary is BSD-3-Clause. The built
package also contains two LGPL libraries (FFmpeg and libmspack) and several
permissive ones; `waterbox/package-licenses.json` lists every one, and the
package carries their licence texts. See [LICENSE](LICENSE).
