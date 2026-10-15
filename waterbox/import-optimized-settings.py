#!/usr/bin/env python3
# The per-game settings the core suggests (SuggestSettings, chimera#289), made
# from a checkout of Xenia Manager's "optimized settings" list:
#
#   git clone https://github.com/xenia-manager/optimized-settings <dir>
#   waterbox/import-optimized-settings.py <dir> > waterbox/driver/optimized_settings.inc
#
# The list is a port of A1eNaz's wiki of optimized settings
# (github.com/A1eNaz/Xenia-Game-Settings/wiki): one file per game, named by its
# title id, holding Xenia Canary's own settings with a comment saying what each
# one fixes. Neither has a licence; the core carries the list with credit to
# both and a link (user-decided, 2026-10-15).
#
# Each of Xenia's settings in the list becomes the core's setting of the same
# name (waterbox.config), or is left out with the reason the note gives. The
# output is a C++ table the driver searches by title id.
import json
import os
import subprocess
import sys
import tomllib

# Xenia's settings the core has, by the list's section and name, with how a
# value is written for the core.
SETTINGS = {
    ("GPU", "readback_resolve"): "string",
    ("GPU", "clear_memory_page_state"): "bool",
    ("GPU", "occlusion_query"): "string",
    ("GPU", "occlusion_query_full_counters"): "bool",
    ("GPU", "occlusion_query_fake_lower_threshold"): "int",
    ("GPU", "occlusion_query_fake_upper_threshold"): "int",
    ("GPU", "depth_bias_shader_offset"): "bool",
    ("GPU", "gamma_render_target_as_unorm16"): "bool",
    ("GPU.Debug", "gamma_render_target_as_unorm16"): "bool",
    ("GPU", "readback_memexport"): "bool",
    ("GPU", "gpu_allow_invalid_upload_range"): "bool",
    ("GPU", "force_depth_clamp"): "bool",
    ("GPU", "render_target_path_vulkan"): "string",
    ("CPU", "break_on_unimplemented_instructions"): "bool",
    ("Memory", "protect_zero"): "bool",
    ("Memory", "ignore_offset_for_ranged_allocations"): "bool",
    ("Memory", "scribble_heap"): "bool",
    ("Memory", "scribble_heap_value"): "int",
    ("APU", "xma_decoder"): "string",
    ("APU", "use_dedicated_xma_thread"): "bool",
    ("Kernel", "cl"): "string",
    ("General", "launch_module"): "string",
    ("Storage", "mount_scratch"): "bool",
}

# Settings whose value makes what the graphics card draws part of the
# console's memory: a project with them is a machine of that card. What the
# core does with a suggestion of one is CARD_POLICY below.
CARD = {"readback_resolve", "occlusion_query", "occlusion_query_full_counters", "readback_memexport"}
CARD_SAFE = {("occlusion_query", "fake"), ("readback_resolve", "none"), ("occlusion_query_full_counters", False),
             ("readback_memexport", False)}
# "suggest": suggested like any other; "note": named in the note, not set.
CARD_POLICY = "note"

# Settings the core does not have, and why, in the note's words.
LEFT_OUT = {
    "vsync": "a frame here is always one of the console's sixty a second",
    "framerate_limit": "a frame here is always one of the console's sixty a second",
    "render_target_path_d3d12": "it is for Direct3D 12, and this core draws with Vulkan",
    "d3d12_submit_on_primary_buffer_end": "it is for Direct3D 12, and this core draws with Vulkan",
    "use_fuzzy_alpha_epsilon": "it is for Direct3D 12, and this core draws with Vulkan",
    "gpu": "this core draws with Vulkan, the Renderer setting",
    "apu": "the sound goes to Chimera",
    "controller_hotkeys": "Chimera has its own hotkeys",
    "disassemble_functions": "it writes a log for developers",
    "mount_cache": "the console's storage is the project's",
    "draw_resolution_scale_threshold": "this core draws at the console's own resolution",
    "time_scalar": "the console's clock here runs at its own speed",
    "license_mask": None,  # the Arcade Games setting, below
    "occlusion_query_querybatch_range": "this core's Xenia does not have it",
    "ac6_ground_fix": "this core's Xenia does not have it",
}


# the names the settings page shows, from the package's own declarations;
# the settings it does not declare by Xenia's own name
DISPLAY = {"readback_resolve": "Readback Resolve", "occlusion_query": "Occlusion Query",
           "occlusion_query_full_counters": "Occlusion Query Full Counters",
           "readback_memexport": "Readback Memexport"}
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "waterbox.config"), encoding="utf-8") as f:
    DISPLAY.update({s["name"]: s["display"] for s in json.load(f)["settings"]})


def display(name):
    return DISPLAY.get(name, name)


def value_text(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def comments(path):
    # the comment after each "key = value" line, by key
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if "=" in line and "#" in line and not line.lstrip().startswith("#"):
                key = line.split("=", 1)[0].strip()
                out[key] = line.split("#", 1)[1].strip().rstrip(".")
    return out


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    root = sys.argv[1]
    commit = subprocess.run(["git", "-C", root, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    with open(os.path.join(root, "data", "settings.json"), encoding="utf-8") as f:
        titles = {e["id"].upper(): e["title"] for e in json.load(f)}
    rows = []
    for name in sorted(os.listdir(os.path.join(root, "settings"))):
        if not name.endswith(".toml"):
            continue
        tid = name[:-5].upper()
        path = os.path.join(root, "settings", name)
        with open(path, "rb") as f:
            table = tomllib.load(f)
        why = comments(path)
        values, said, card, out = {}, [], [], []

        def walk(section, kv):
            for k, v in kv.items():
                if isinstance(v, dict):
                    walk(section + "." + k, v)
                    continue
                fix = why.get(k, "")
                kind = SETTINGS.get((section, k))
                if k == "license_mask":
                    values["arcade_license"] = "full" if v else "trial"
                    said.append(("Arcade Games", "full" if v else "trial", fix))
                elif kind:
                    text = value_text(v)
                    if k in CARD and (k, v) not in CARD_SAFE and CARD_POLICY != "suggest":
                        card.append((display(k), text, fix))
                    else:
                        values[k] = v if kind != "string" else str(v)
                        said.append((display(k), text, fix))
                else:
                    out.append((k, value_text(v), LEFT_OUT.get(k, "this core does not have it")))

        for section, kv in table.items():
            walk(section, kv)
        title = titles.get(tid, tid)
        parts = []
        for k, v, fix in said:
            parts.append(f"{k} {v}" + (f" ({fix[0].lower() + fix[1:]})" if fix else ""))
        note = f"Xenia Manager's optimized settings for {title}."
        if parts:
            note += " Set: " + "; ".join(parts) + "."
        if card:
            note += " Not set, because the console would then hold what your graphics card draws, and a movie " \
                    "would play back only on this card: " + "; ".join(
                        f"{k} {v}" + (f" ({fix[0].lower() + fix[1:]})" if fix else "") for k, v, fix in card) + "."
        if out:
            note += " Not used here: " + "; ".join(f"{k} = {v}, as {r}" for k, v, r in out) + "."
        rows.append((tid, title, json.dumps(values, separators=(",", ":")), note))

    print("// GENERATED by waterbox/import-optimized-settings.py; do not edit.")
    print(f"// From github.com/xenia-manager/optimized-settings at {commit},")
    print("// a port of A1eNaz's wiki (github.com/A1eNaz/Xenia-Game-Settings/wiki).")
    print(f"// {len(rows)} games.")
    for tid, title, values, note in rows:
        print(f"{{{c_string(tid)}, {c_string(title)},\n {c_string(values)},\n {c_string(note)}}},")


if __name__ == "__main__":
    main()
