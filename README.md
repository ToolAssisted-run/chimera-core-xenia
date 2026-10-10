# chimera-core-xenia

An Xbox 360 core for [Chimera](https://github.com/ToolAssisted-run/chimera),
built on [Xenia Canary](https://github.com/xenia-canary/xenia-canary). Work
in progress: it runs a game in the sandbox a frame at a time, the same way
every time, with pads, sound and savestates and no picture yet.
`docs/PLAN.md` has what was found in the source, the route, and what each
step measured.

    git submodule update --init
    git -C extern/xenia submodule update --init --depth 1
    waterbox/build-native.sh
    waterbox/build-ppc-tests.sh && waterbox/run-ppc-tests.sh
    waterbox/build-guest.sh -m <a miniBox checkout, built>
    waterbox/run-gate.sh
