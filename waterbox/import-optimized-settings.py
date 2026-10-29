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
# Of those, the ones the core offers as settings: all of them.
OFFERED_CARD = {"setting.readback_resolve", "setting.occlusion_query", "setting.occlusion_query_full_counters",
                "setting.readback_memexport"}
# "suggest": suggested like any other; "note": named in the note, not set -
# the owner's choice: offered as settings, never set by the suggestion
# (user-decided, 2026-10-16).
CARD_POLICY = "note"

# Settings the core does not have, and why: a key of languages/en.json each
# (the note is keys, chimera docs/translations.md).
LEFT_OUT = {
    "vsync": "suggest.reason.sixty_frames",
    "framerate_limit": "suggest.reason.sixty_frames",
    "render_target_path_d3d12": "suggest.reason.direct3d12",
    "d3d12_submit_on_primary_buffer_end": "suggest.reason.direct3d12",
    "use_fuzzy_alpha_epsilon": "suggest.reason.direct3d12",
    "gpu": "suggest.reason.vulkan",
    "apu": "suggest.reason.sound",
    "controller_hotkeys": "suggest.reason.hotkeys",
    "disassemble_functions": "suggest.reason.developer_log",
    "mount_cache": "suggest.reason.storage",
    "draw_resolution_scale_threshold": "suggest.reason.resolution",
    "time_scalar": "suggest.reason.clock",
    "license_mask": None,  # the Arcade Games setting, below
    "occlusion_query_querybatch_range": "suggest.reason.not_in_xenia",
    "ac6_ground_fix": "suggest.reason.not_in_xenia",
}
# the note: a text each, a line each, each key's arguments after a 0x1F
ARG, LINE = "\x1f", "\x1d"


def item(key, *args):
    return ARG.join([key] + [str(a) for a in args])


# the names the settings page shows (keys), from the package's own declarations
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "waterbox.config"), encoding="utf-8") as f:
    DISPLAY = {s["name"]: s["display"] for s in json.load(f)["settings"]}


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
    # 0x1F and 0x1D each written as an octal escape, which takes at most
    # three digits and so never runs into the text after it
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\x1f", "\\037").replace("\x1d", "\\035") + '"'


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
                    said.append((display("arcade_license"), "full" if v else "trial", fix))
                elif kind:
                    text = value_text(v)
                    if k in CARD and (k, v) not in CARD_SAFE and CARD_POLICY != "suggest":
                        card.append((display(k), text, fix))
                    else:
                        values[k] = v if kind != "string" else str(v)
                        said.append((display(k), text, fix))
                else:
                    out.append((k, value_text(v), LEFT_OUT.get(k, "suggest.reason.not_in_core")))

        for section, kv in table.items():
            walk(section, kv)
        title = titles.get(tid, tid)

        def setting(k, v, fix):
            # the list's own reason, as it wrote it, is not ours to translate
            return item("suggest.item_why", k, v, fix[0].lower() + fix[1:]) if fix else item("suggest.item", k, v)
        lines = [item("suggest.title", title)]
        if said:
            lines += ["suggest.set"] + [setting(k, v, fix) for k, v, fix in said]
        offered = [x for x in card if x[0] in OFFERED_CARD]
        held = [x for x in card if x[0] not in OFFERED_CARD]
        if offered:
            lines += ["suggest.card"] + [setting(k, v, fix) for k, v, fix in offered]
        if held:
            lines += ["suggest.not_offered"] + [setting(k, v, fix) for k, v, fix in held]
        if out:
            lines += ["suggest.not_used"] + [item("suggest.left_out", k, v, r) for k, v, r in out]
        rows.append((tid, title, json.dumps(values, separators=(",", ":")), LINE.join(lines)))

    print("// GENERATED by waterbox/import-optimized-settings.py; do not edit.")
    print(f"// From github.com/xenia-manager/optimized-settings at {commit},")
    print("// a port of A1eNaz's wiki (github.com/A1eNaz/Xenia-Game-Settings/wiki).")
    print(f"// {len(rows)} games.")
    for tid, title, values, note in rows:
        print(f"{{{c_string(tid)}, {c_string(title)},\n {c_string(values)},\n {c_string(note)}}},")


if __name__ == "__main__":
    main()
