import json
import os
from datetime import datetime, timezone

version = os.environ["VERSION"]
supported_games = os.environ.get("SUPPORTED_GAMES", "jak1").split(",")
repo = os.environ.get("GITHUB_REPOSITORY", "Zed-Mod-School/OG-Mod-Base-mumble-test")

mod_list_path = os.path.join(os.environ.get("GITHUB_WORKSPACE", "."), "mod_list.json")

with open(mod_list_path, "r") as f:
    mod_list = json.load(f)

mod = mod_list["mods"]["mumble-proximity-chat"]

base_url = f"https://github.com/{repo}/releases/download/{version}"

new_version = {
    "version": version.lstrip("v"),
    "publishedDate": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "supportedGames": [g.strip() for g in supported_games],
    "assets": {
        "windows": f"{base_url}/windows-{version}.zip",
        "linux": f"{base_url}/linux-{version}.tar.gz",
        "macos-intel": f"{base_url}/macos-intel-{version}.tar.gz",
    },
    "assetDownloadCounts": {
        "windows": 0,
        "linux": 0,
        "macos-intel": 0,
    },
}

# Deduplicate - don't add the same version twice
cleaned = version.lstrip("v")
mod["versions"] = [v for v in mod["versions"] if v["version"] != cleaned]
mod["versions"].insert(0, new_version)

# Update top-level supportedGames from all versions
all_games = set()
for v in mod["versions"]:
    if v.get("supportedGames"):
        all_games.update(v["supportedGames"])
if all_games:
    mod["supportedGames"] = sorted(all_games)

mod_list["lastUpdated"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

with open(mod_list_path, "w") as f:
    json.dump(mod_list, f, indent=2)
    f.write("\n")

print(f"Updated mod_list.json with version {version}")
