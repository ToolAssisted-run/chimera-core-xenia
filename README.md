# chimera-core-xenia

An Xbox 360 core for [Chimera](https://github.com/ToolAssisted-run/chimera),
built on [Xenia Canary](https://github.com/xenia-canary/xenia-canary). Work
in progress: it builds, headless, and runs nothing yet. `docs/PLAN.md` has
what was found in the source, the route, and what has been done.

    git submodule update --init
    git -C extern/xenia submodule update --init --depth 1
    waterbox/build-native.sh
