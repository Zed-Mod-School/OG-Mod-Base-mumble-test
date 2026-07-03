import json
import os
from datetime import datetime, timezone

MOD_ID = os.environ.get("MOD_ID", "mumble-proximity-chat-beta")
DISPLAY_NAME = "Mumble Proximity Chat - BETA"
# ids of this mod from before the BETA rename - migrated automatically below
LEGACY_MOD_IDS = ["mumble-proximity-chat"]

version = os.environ["VERSION"]
supported_games = [g.strip() for g in os.environ.get("SUPPORTED_GAMES", "jak1").split(",") if g.strip()]
repo = os.environ.get("GITHUB_REPOSITORY", "Zed-Mod-School/OG-Mod-Base-mumble-test")
repo_url = f"https://github.com/{repo}"
# cover/thumbnail art, always kept in sync by this script
ART_URL = f"https://raw.githubusercontent.com/{repo}/main/.github/assets/mod-cover.png"

mod_list_path = os.path.join(os.environ.get("GITHUB_WORKSPACE", "."), "mod_list.json")

now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

# Load the existing list, or start a fresh one if it was never committed
if os.path.exists(mod_list_path):
    with open(mod_list_path, "r") as f:
        mod_list = json.load(f)
    print(f"Loaded existing {mod_list_path}")
else:
    mod_list = {}
    print(f"{mod_list_path} not found, bootstrapping a new one")

mod_list.setdefault("schemaVersion", "1.0.0")
mod_list.setdefault("sourceName", repo.split("/")[-1])
mod_list.setdefault("mods", {})
mod_list.setdefault("texturePacks", {})

# migrate entries from before the BETA rename (keeps their version history)
for legacy_id in LEGACY_MOD_IDS:
    if legacy_id in mod_list["mods"] and MOD_ID not in mod_list["mods"]:
        mod_list["mods"][MOD_ID] = mod_list["mods"].pop(legacy_id)
        print(f"Migrated mod entry {legacy_id} -> {MOD_ID}")

# Create the mod entry if missing; setdefault fields can be hand-edited,
# assigned fields are owned by this script and always overwritten
mod = mod_list["mods"].setdefault(MOD_ID, {})
mod["displayName"] = DISPLAY_NAME
mod.setdefault(
    "description",
    "Proximity voice chat for Jak 1 via the Mumble Link plugin. Voice volume "
    "and direction follow player and camera positions in-game.",
)
mod.setdefault("authors", ["Zed"])
mod.setdefault("tags", ["multiplayer", "voice-chat"])
mod.setdefault("websiteUrl", repo_url)
mod.setdefault("supportedGames", supported_games)
mod.setdefault("versions", [])
mod["coverArtUrl"] = ART_URL
mod["thumbnailArtUrl"] = ART_URL
mod.setdefault("perGameConfig", None)
mod.setdefault("externalLink", None)

base_url = f"{repo_url}/releases/download/{version}"

new_version = {
    "version": version.lstrip("v"),
    "publishedDate": now,
    "supportedGames": supported_games,
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

mod_list["lastUpdated"] = now

with open(mod_list_path, "w") as f:
    json.dump(mod_list, f, indent=2)
    f.write("\n")

print(f"Updated mod_list.json with version {version}")
