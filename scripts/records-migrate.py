"""Writes mods-info's records/ folder from its team/ and community/ folders: every mod in one list, each record with
its "platform" (which list it appears in) and its "author" (Made by).

    python scripts/records-migrate.py <mods-info checkout>

Only records/ is written. team/ and community/ stay as they are for launchers up to 1.0.44, which reject unknown keys.
Which rules read a record is decided by its repository in the launcher, so "author" here is only what the page says.
An id in both folders is an error and nothing is written.
"""

import json
import os
import sys

TEAM_AUTHOR = "Multiplayer Everything"


def platform_of(record, folder):
    if folder == "team":
        return "flash" if "flash" in record.get("categories", []) else "pc"
    return "flash" if record.get("kind") == "flash" else "pc"


def author_of(record, folder):
    if folder == "team":
        return TEAM_AUTHOR
    return record.get("author") or record["repo"].split("/")[0]


def migrated(record, folder):
    """The record as the website's recordsCopy writes it, byte for byte: id, repo, the two keys, the rest in its own order."""
    rest = {k: v for k, v in record.items() if k not in ("id", "repo", "platform", "author")}
    return {"id": record["id"], "repo": record["repo"], "platform": platform_of(record, folder), "author": author_of(record, folder), **rest}


def read_folder(root, folder):
    with open(os.path.join(root, folder, "index.json"), encoding="utf-8") as f:
        ids = json.load(f)["mods"]
    records = {}
    for mod_id in ids:
        with open(os.path.join(root, folder, mod_id + ".json"), encoding="utf-8") as f:
            records[mod_id] = json.load(f)
    return records


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    team, community = read_folder(root, "team"), read_folder(root, "community")
    both = sorted(set(team) & set(community))
    if both:
        sys.exit("In both team/ and community/: " + ", ".join(both))
    out = {i: migrated(r, "team") for i, r in team.items()}
    out.update({i: migrated(r, "community") for i, r in community.items()})

    folder = os.path.join(root, "records")
    os.makedirs(folder, exist_ok=True)
    for mod_id, record in out.items():
        with open(os.path.join(folder, mod_id + ".json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(record, f, indent=2, ensure_ascii=False)
            f.write("\n")
    with open(os.path.join(folder, "index.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"mods": sorted(out)}, f, indent=2)
        f.write("\n")
    print(f"records/: {len(out)} mods ({len(team)} team, {len(community)} community)")


if __name__ == "__main__":
    main()
