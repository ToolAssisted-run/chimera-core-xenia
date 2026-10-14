#!/usr/bin/env python3
"""Writes a .chimeraProject of no frames for chimera-run to open.

A project is what Chimera hands the core when a person makes one in the New
Project window: the game in its slot, under its own file name, and a value
for every declared setting. The engine checks the file against the slot's
declared formats when it opens the project, which a game named on
chimera-run's command line never goes through - and an Xbox Live Arcade game
is a file with no extension (chimera issue 250).

usage: make-project.py <package> <out.chimeraProject> <game file> [--set=<setting>=<value> ...]
                       [--file=<slot>=<path> ...]

A --file is another file of the project, in the slot named (content,
savedata); chimera-run is then given the folder it is in with --files.

The project has no input: chimera-run records the frames it is asked to run.
"""
import hashlib
import json
import os
import sys
import zipfile


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 24), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def main():
    package, out, game = sys.argv[1:4]
    cfg = json.loads(zipfile.ZipFile(package).read("waterbox.config"))
    settings = {d["name"]: d["default"] for d in cfg.get("settings", []) if d.get("default") is not None}
    files = []
    for arg in sys.argv[4:]:
        if arg.startswith("--file="):
            slot, path = arg[len("--file="):].split("=", 1)
            files.append({"name": os.path.basename(path), "sha1": sha1(path), "slot": slot})
            continue
        name, value = arg[len("--set="):].split("=", 1)
        if name not in settings:
            sys.exit("make-project.py: the package declares no setting %r" % name)
        settings[name] = value
    game_sha1 = sha1(game)
    project = {
        "id": "xenia-gate-01",
        "title": "Xbox 360 through Chimera",
        "description": "written by waterbox/tests/make-project.py",
        "core": {"name": cfg["coreName"], "version": cfg["version"], "sha1": sha1(package)},
        "rerecords": 0,
        "files": [{"name": os.path.basename(game), "sha1": game_sha1, "slot": "game"}] + files,
        "settings": settings,
        "firmware": [],
        "coreCache": [],
        "input": "[Input]\nLogKey:#\n[/Input]\n",
        "markers": [],
        "branches": [],
        "headers": {
            "MovieVersion": "Chimera Project File v1.1",
            "Platform": cfg["systemId"],
            "SHA1": game_sha1,
            "LastInputFrame": "-1",
            "VsyncNumerator": str(cfg["video"]["vsyncNumerator"]),
            "VsyncDenominator": str(cfg["video"]["vsyncDenominator"]),
        },
    }
    with open(out, "w") as f:
        json.dump(project, f, indent="\t")
    return 0


if __name__ == "__main__":
    sys.exit(main())
