"""Rewrites our Flash team records (tests/fixtures/team) in the short form 1.0.43 reads: a "flash" object instead of
hand-written buttons. The launcher makes the buttons from it (src-tauri/src/launcher/community.rs, flash_actions).

    python scripts/flash-records-short.py <from folder> <to folder>

Only the buttons change; every other key of a record is kept as it is. A record whose buttons the short form can't
say exactly (other arguments, other files per button) is reported and left alone. The Rust test
flash_team_records_in_the_short_form_make_the_same_buttons proves the rest start the same command lines.
"""

import json
import os
import sys

DEFAULT_TEXT = {
    "offline": ("Play offline", "The game on your own"),
    "local": ("Play on one keyboard", None),
    "play": ("Play the original", "Without the online mod"),
}


def keys_of(action):
    args = action.get("args", [])
    return args[args.index("--coop-keys") + 1] if "--coop-keys" in args else None


def short(r):
    acts = {a["id"]: a for a in r["actions"]}
    online = acts.get("online")
    if not online:
        raise ValueError("no online button")
    files = online["swf"]
    if len(files) != 1:
        raise ValueError("the online button has several files")
    keys = keys_of(online)
    if not keys:
        raise ValueError("the online button has no key map")
    hosts = [n for n in range(2, 7) if ("host" if n == 2 else f"host{n}") in acts]
    flash = {"file": files[0], "keys": keys, "maxPlayers": max(hosts)}
    expected = {"online", "join"} | {f"room{n}" for n in hosts if n > 2} | {"host" if n == 2 else f"host{n}" for n in hosts}
    for name in expected:
        a = acts.get(name)
        if not a or a["swf"] != files or keys_of(a) != keys:
            raise ValueError(f"button {name} differs")
    rest = set(acts) - expected
    for name in rest:
        a = acts[name]
        if name in ("offline", "local"):
            o = {}
            if name == "local":
                o["local"] = True
            if keys_of(a) != keys:
                o["keys"] = keys_of(a) or False
            label, sub = DEFAULT_TEXT[name]
            if a["label"] != label:
                o["label"] = a["label"]
            if sub is None:
                sub = f"Up to {flash['maxPlayers']} players on this PC"
            if a.get("description") != sub:
                o["description"] = a["description"]
            if a["swf"] != files or set(a) - {"id", "label", "icon", "offline", "description", "swf", "args"}:
                raise ValueError(f"button {name} differs")
            flash["offline"] = o
        elif name == "play":
            o = {"files": a["swf"]}
            if a.get("description") != DEFAULT_TEXT["play"][1]:
                o["description"] = a["description"]
            if a.get("args") or a.get("edition") != "original":
                raise ValueError("the original button differs")
            flash["original"] = o
        else:
            raise ValueError(f"button {name} isn't one the launcher makes")
    out = {}
    for k, v in r.items():
        if k == "actions":
            out["flash"] = flash
        elif k == "minLauncher":
            out[k] = "1.0.43"
        else:
            out[k] = v
    return out


def main(src, dst):
    os.makedirs(dst, exist_ok=True)
    index = json.load(open(os.path.join(src, "index.json"), encoding="utf-8"))
    for id_ in index["mods"]:
        r = json.load(open(os.path.join(src, id_ + ".json"), encoding="utf-8"))
        if "actions" in r:
            try:
                r = short(r)
            except ValueError as e:
                print(f"{id_}: kept as it is ({e})")
        with open(os.path.join(dst, id_ + ".json"), "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(r, indent=2, ensure_ascii=False) + "\n")
    with open(os.path.join(dst, "index.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(index, indent=2) + "\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
