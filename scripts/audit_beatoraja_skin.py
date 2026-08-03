#!/usr/bin/env python3
"""Audit an external SCURO archive without copying or redistributing its payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import struct
import subprocess
import sys
import unicodedata
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Iterable, Iterator


PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
TARGET_VERSION = "4.02"
OFFICIAL_SOURCE_URL = "https://www.kasacontent.com/musicgame/beatoraja/4226/"
TERMS_URL = "https://www.kasacontent.com/musicgame/beatoraja/4635/"
ACQUISITION_DATE = "2026-08-03"
TREE_DOMAIN = b"ASOBMSKIN-TREE-V1\0"

MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MAX_REGULAR_FILE_BYTES = 512 * 1024 * 1024
MAX_EXPANDED_BYTES = 4 * 1024 * 1024 * 1024
MAX_FILES = 20_000
MAX_PATH_BYTES = 1_024
MAX_PATH_COMPONENTS = 64
SUPPORTED_COMPRESSION = {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}

SOURCE_PROVENANCE = (
    ("src/bms/player/beatoraja/skin/SkinLoader.java", "SkinLoader.load", "selects .luaskin entries and delegates them to LuaSkinLoader"),
    ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.loadHeader", "executes the entry with nil skin_config and converts the returned header table"),
    ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.load", "applies configuration, executes the same entry again, and loads the converted gameplay skin"),
    ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.fromLuaValue", "converts Lua tables recursively by reflected model field name"),
    ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.serializeLuaScript", "dispatches callback values as function, number, recognized name, or script string"),
    ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.setDirectory", "sets the package-local Lua module search directory"),
    ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.execFile", "executes a Lua file in the retained Globals instance"),
    ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.exportSkinProperty", "publishes selected file, option, enabled-option, and offset configuration"),
    ("src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java", "LegacySkinLuaApi.install", "installs the restricted legacy luajava class, constructor, file, GDX, controller, and HTTP facades"),
    ("src/bms/player/beatoraja/skin/json/JSONSkinLoader.java", "JSONSkinLoader.loadJsonSkinHeader", "converts header properties, files, offsets, and categories"),
    ("src/bms/player/beatoraja/skin/json/JSONSkinLoader.java", "JSONSkinLoader.loadJsonSkin", "constructs play objects in authored destination order"),
    ("src/bms/player/beatoraja/skin/json/JSONSkinLoader.java", "JSONSkinLoader.setDestination", "inherits omitted destination fields and binds conditions, timer, clip, offsets, and stretch"),
    ("src/bms/player/beatoraja/skin/SkinHeader.java", "SkinHeader.setSkinConfigProperty", "reconciles configured custom options, files, and offsets"),
    ("src/bms/player/beatoraja/skin/Skin.java", "Skin.prepare", "removes invalid and statically disabled objects before resource load"),
    ("src/bms/player/beatoraja/skin/Skin.java", "Skin.drawAllObjects", "prepares then draws surviving objects in authored array order"),
    ("src/bms/player/beatoraja/skin/Skin.java", "Skin.updateCustomObjects", "updates custom timers before custom events, each in ascending ID order"),
    ("src/bms/player/beatoraja/skin/SkinObject.java", "SkinObject.prepareRegion", "applies destination timer, loop, interpolation, and configured offsets"),
    ("src/bms/player/beatoraja/play/PlaySkin.java", "PlaySkin", "stores play-lane line, BPM, stop, time, cover, judge, and timing configuration"),
    ("src/bms/player/beatoraja/play/SkinNote.java", "SkinNote.prepare", "samples normal, mine, hidden, processed, and ten long-note image phases"),
    ("src/bms/player/beatoraja/play/LaneRenderer.java", "LaneRenderer.drawLongNote", "selects distinct LN, CN, and HCN endpoint and body phases"),
    ("src/bms/player/beatoraja/play/SkinBGA.java", "SkinBGA.prepare", "advances BGA state at the play timer before drawing"),
    ("src/bms/player/beatoraja/play/bga/BGAProcessor.java", "BGAProcessor.drawBGA", "draws an active miss sequence exclusively, otherwise base then layer"),
    ("src/bms/player/beatoraja/skin/property/BooleanPropertyFactory.java", "BooleanPropertyFactory.getBooleanProperty", "maps supported boolean IDs or names and returns null for an unknown mapping"),
    ("src/bms/player/beatoraja/skin/property/IntegerPropertyFactory.java", "IntegerPropertyFactory.getIntegerProperty", "maps supported integer IDs or names and returns null for an unknown mapping"),
    ("src/bms/player/beatoraja/skin/property/FloatPropertyFactory.java", "FloatPropertyFactory.getRateProperty", "maps supported float IDs or names and returns null for an unknown mapping"),
    ("src/bms/player/beatoraja/skin/property/StringPropertyFactory.java", "StringPropertyFactory.getStringProperty", "maps supported string IDs or names and returns null for an unknown mapping"),
    ("src/bms/player/beatoraja/skin/property/TimerPropertyFactory.java", "TimerPropertyFactory.getTimerProperty", "maps nonnegative timer IDs and rejects negative IDs"),
    ("src/bms/player/beatoraja/skin/property/EventFactory.java", "EventFactory.getEvent", "maps supported event IDs or names and returns null for an unknown mapping"),
)

OBJECT_NAMES = (
    "source", "font", "text", "image", "imageset", "note", "value", "floatvalue",
    "slider", "graph", "gaugegraph", "judgegraph", "bpmgraph", "timingvisualizer",
    "hiterrorvisualizer", "timingdistributiongraph", "hiddenCover", "liftCover", "bga",
    "judge", "gauge", "destination", "customEvents", "customTimers",
)

PROPERTY_CATEGORIES = {
    "OP": "boolean",
    "NUM": "integer",
    "GRAPH": "float",
    "STRING": "string",
    "SLIDER": "float",
    "OFFSET": "offset",
}


class AuditError(RuntimeError):
    pass


@dataclass(frozen=True)
class ZipRegularFile:
    path: str
    info: zipfile.ZipInfo


@dataclass(frozen=True)
class DiskRegularFile:
    path: str
    disk_path: Path
    byte_count: int


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        copy_to_digest(stream, digest, MAX_ARCHIVE_BYTES)
    return digest.hexdigest()


def copy_to_digest(stream: BinaryIO, digest, maximum: int) -> int:
    count = 0
    while True:
        chunk = stream.read(1024 * 1024)
        if not chunk:
            return count
        count += len(chunk)
        if count > maximum:
            raise AuditError("input exceeded its policy limit while being read")
        digest.update(chunk)


def normalize_relative_path(raw_path: str, *, directory: bool = False) -> str:
    if not isinstance(raw_path, str) or not raw_path or "\0" in raw_path:
        raise AuditError("empty or invalid archive path")
    if "\\" in raw_path or raw_path.startswith("/") or re.match(r"^[A-Za-z]:", raw_path):
        raise AuditError(f"non-relative archive path: {raw_path!r}")
    if directory and raw_path.endswith("/"):
        raw_path = raw_path[:-1]
    components = raw_path.split("/")
    if not components or any(component in ("", ".", "..") for component in components):
        raise AuditError(f"unsafe archive path component: {raw_path!r}")
    if len(components) > MAX_PATH_COMPONENTS:
        raise AuditError(f"archive path has too many components: {raw_path!r}")
    normalized = "/".join(unicodedata.normalize("NFC", component) for component in components)
    try:
        encoded = normalized.encode("utf-8")
    except UnicodeEncodeError as error:
        raise AuditError(f"archive path is not valid UTF-8: {raw_path!r}") from error
    if len(encoded) > MAX_PATH_BYTES:
        raise AuditError(f"archive path is too long: {raw_path!r}")
    return normalized


def collision_key(path: str) -> str:
    return unicodedata.normalize("NFC", path).casefold()


def zip_entry_kind(info: zipfile.ZipInfo) -> str:
    unix_mode = (info.external_attr >> 16) & 0xFFFF
    file_type = stat.S_IFMT(unix_mode)
    if info.is_dir():
        if file_type not in (0, stat.S_IFDIR):
            return "special"
        return "directory"
    if file_type not in (0, stat.S_IFREG):
        return "special"
    return "file"


def parents(path: str) -> Iterator[str]:
    components = path.split("/")
    for length in range(1, len(components)):
        yield "/".join(components[:length])


def inspect_archive(archive_path: Path, declared_prefix: str):
    if not archive_path.is_file() or archive_path.is_symlink():
        raise AuditError(f"archive is not a regular file: {archive_path}")
    archive_size = archive_path.stat().st_size
    if archive_size > MAX_ARCHIVE_BYTES:
        raise AuditError("archive exceeds the 2 GiB policy limit")
    archive_digest = sha256_file(archive_path)
    regular: list[tuple[str, zipfile.ZipInfo]] = []
    explicit_directories: list[str] = []
    seen: dict[str, tuple[str, str]] = {}
    total_expanded = 0
    try:
        archive = zipfile.ZipFile(archive_path)
    except (OSError, zipfile.BadZipFile) as error:
        raise AuditError(f"invalid ZIP archive: {error}") from error
    with archive:
        for info in archive.infolist():
            if info.flag_bits & 0x1:
                raise AuditError(f"encrypted ZIP entry is not allowed: {info.filename!r}")
            if info.compress_type not in SUPPORTED_COMPRESSION:
                raise AuditError(f"unsupported ZIP compression method: {info.compress_type}")
            kind = zip_entry_kind(info)
            if kind == "special":
                raise AuditError(f"link or special ZIP entry is not allowed: {info.filename!r}")
            normalized = normalize_relative_path(info.filename, directory=kind == "directory")
            key = collision_key(normalized)
            if key in seen:
                previous_path, previous_kind = seen[key]
                raise AuditError(
                    f"duplicate or colliding ZIP path: {previous_path!r} ({previous_kind}) and {normalized!r} ({kind})"
                )
            seen[key] = (normalized, kind)
            if kind == "directory":
                explicit_directories.append(normalized)
                continue
            if info.file_size > MAX_REGULAR_FILE_BYTES:
                raise AuditError(f"regular ZIP entry exceeds the 512 MiB limit: {normalized!r}")
            regular.append((normalized, info))
            if len(regular) > MAX_FILES:
                raise AuditError("ZIP has more than 20,000 regular files")
            total_expanded += info.file_size
            if total_expanded > MAX_EXPANDED_BYTES:
                raise AuditError("ZIP expanded bytes exceed the 4 GiB limit")
        if not regular:
            raise AuditError("ZIP contains no regular files")

        file_keys = {collision_key(path): path for path, _ in regular}
        directory_keys = {collision_key(path): path for path in explicit_directories}
        for path, _ in regular:
            for parent in parents(path):
                if collision_key(parent) in file_keys:
                    raise AuditError(f"ZIP file/directory collision at {parent!r}")
        for directory in explicit_directories:
            if collision_key(directory) in file_keys:
                raise AuditError(f"ZIP file/directory collision at {directory!r}")
            for parent in parents(directory):
                if collision_key(parent) in file_keys:
                    raise AuditError(f"ZIP file/directory collision at {parent!r}")

        first_components = {path.split("/", 1)[0] for path, _ in regular if "/" in path}
        every_file_below_component = all("/" in path for path, _ in regular)
        inferred_prefix = (
            next(iter(first_components))
            if every_file_below_component and len(first_components) == 1
            else "."
        )
        declared = "." if declared_prefix == "." else normalize_relative_path(declared_prefix)
        if declared != inferred_prefix:
            raise AuditError(
                f"archive package prefix mismatch: declared {declared!r}, inferred {inferred_prefix!r}"
            )

        prefix_text = inferred_prefix + "/" if inferred_prefix != "." else ""
        stripped_files: list[ZipRegularFile] = []
        stripped_seen: set[str] = set()
        for path, info in regular:
            if prefix_text:
                if not path.startswith(prefix_text):
                    raise AuditError(f"archive payload is outside prefix {inferred_prefix!r}: {path!r}")
                stripped = path[len(prefix_text):]
            else:
                stripped = path
            if not stripped:
                raise AuditError("regular ZIP entry became empty after wrapper stripping")
            key = collision_key(stripped)
            if key in stripped_seen:
                raise AuditError(f"post-strip ZIP path collision: {stripped!r}")
            stripped_seen.add(key)
            stripped_files.append(ZipRegularFile(stripped, info))

        for directory in explicit_directories:
            if prefix_text:
                if directory == inferred_prefix:
                    continue
                if not directory.startswith(prefix_text):
                    raise AuditError(
                        f"explicit ZIP directory is outside prefix {inferred_prefix!r}: {directory!r}"
                    )
                stripped_directory = directory[len(prefix_text):]
                if not stripped_directory:
                    raise AuditError("explicit ZIP directory became empty after wrapper stripping")
            else:
                stripped_directory = directory
            if collision_key(stripped_directory) in stripped_seen:
                raise AuditError(f"post-strip ZIP file/directory collision: {stripped_directory!r}")

        stripped_files.sort(key=lambda item: item.path.encode("utf-8"))
        tree_digest = hashlib.sha256()
        tree_digest.update(TREE_DOMAIN)
        tree_digest.update(struct.pack(">Q", len(stripped_files)))
        payload_records = []
        extracted_text: dict[str, str] = {}
        for item in stripped_files:
            path_bytes = item.path.encode("utf-8")
            tree_digest.update(struct.pack(">I", len(path_bytes)))
            tree_digest.update(path_bytes)
            tree_digest.update(struct.pack(">Q", item.info.file_size))
            file_digest = hashlib.sha256()
            try:
                with archive.open(item.info, "r") as stream:
                    count = 0
                    while True:
                        chunk = stream.read(1024 * 1024)
                        if not chunk:
                            break
                        count += len(chunk)
                        if count > item.info.file_size or count > MAX_REGULAR_FILE_BYTES:
                            raise AuditError(f"ZIP entry exceeded its declared size: {item.path!r}")
                        tree_digest.update(chunk)
                        file_digest.update(chunk)
                if count != item.info.file_size:
                    raise AuditError(f"ZIP entry size mismatch while reading: {item.path!r}")
            except (OSError, RuntimeError, zipfile.BadZipFile, NotImplementedError) as error:
                raise AuditError(f"could not safely read ZIP entry {item.path!r}: {error}") from error
            payload_records.append(
                {
                    "id": opaque_id("payload", item.path),
                    "kind": file_kind(item.path),
                    "byteCount": item.info.file_size,
                    "sha256": file_digest.hexdigest(),
                }
            )
            if item.path.lower().endswith((".lua", ".luaskin")):
                try:
                    with archive.open(item.info, "r") as stream:
                        extracted_text[item.path] = stream.read(MAX_REGULAR_FILE_BYTES + 1).decode("utf-8")
                except (UnicodeDecodeError, OSError, RuntimeError, zipfile.BadZipFile) as error:
                    raise AuditError(f"Lua source is not readable UTF-8 text: {item.path!r}: {error}") from error

    return {
        "archiveSha256": archive_digest,
        "archiveByteCount": archive_size,
        "prefix": inferred_prefix,
        "treeSha256": tree_digest.hexdigest(),
        "files": stripped_files,
        "payloads": payload_records,
        "luaText": extracted_text,
    }


def inspect_disk_tree(root: Path):
    if not root.is_dir() or root.is_symlink():
        raise AuditError(f"skin root is not a real directory: {root}")
    regular: list[DiskRegularFile] = []
    seen: dict[str, str] = {}
    total_size = 0
    for current_root, directory_names, file_names in os.walk(root, topdown=True, followlinks=False):
        current = Path(current_root)
        for name in list(directory_names):
            path = current / name
            if path.is_symlink():
                raise AuditError(f"link in extracted skin tree is not allowed: {path.relative_to(root)}")
            mode = path.stat(follow_symlinks=False).st_mode
            if not stat.S_ISDIR(mode):
                raise AuditError(f"special node in extracted skin tree: {path.relative_to(root)}")
        for name in file_names:
            path = current / name
            mode = path.stat(follow_symlinks=False).st_mode
            if not stat.S_ISREG(mode):
                raise AuditError(f"link or special node in extracted skin tree: {path.relative_to(root)}")
            relative = normalize_relative_path(path.relative_to(root).as_posix())
            key = collision_key(relative)
            if key in seen:
                raise AuditError(f"colliding path in extracted skin tree: {seen[key]!r} and {relative!r}")
            seen[key] = relative
            byte_count = path.stat(follow_symlinks=False).st_size
            if byte_count > MAX_REGULAR_FILE_BYTES:
                raise AuditError(f"extracted regular file exceeds the 512 MiB limit: {relative!r}")
            regular.append(DiskRegularFile(relative, path, byte_count))
            if len(regular) > MAX_FILES:
                raise AuditError("extracted tree has more than 20,000 regular files")
            total_size += byte_count
            if total_size > MAX_EXPANDED_BYTES:
                raise AuditError("extracted tree exceeds the 4 GiB expanded limit")
    if not regular:
        raise AuditError("extracted skin tree contains no regular files")
    regular.sort(key=lambda item: item.path.encode("utf-8"))
    digest = hashlib.sha256()
    digest.update(TREE_DOMAIN)
    digest.update(struct.pack(">Q", len(regular)))
    for item in regular:
        path_bytes = item.path.encode("utf-8")
        digest.update(struct.pack(">I", len(path_bytes)))
        digest.update(path_bytes)
        digest.update(struct.pack(">Q", item.byte_count))
        with item.disk_path.open("rb") as stream:
            count = copy_to_digest(stream, digest, MAX_REGULAR_FILE_BYTES)
        if count != item.byte_count:
            raise AuditError(f"extracted file changed while auditing: {item.path!r}")
    return digest.hexdigest(), regular


def validate_reference_root(root: Path, require_clean: bool = True):
    if not root.is_dir():
        raise AuditError(f"Beatoraja root is not a directory: {root}")
    def run(*arguments: str) -> str:
        try:
            result = subprocess.run(
                ["git", "-C", str(root), *arguments],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as error:
            raise AuditError(f"could not execute git: {error}") from error
        if result.returncode:
            raise AuditError(result.stderr.strip() or result.stdout.strip() or "git command failed")
        return result.stdout
    commit = run("rev-parse", "HEAD").strip()
    if commit != PINNED_COMMIT:
        raise AuditError(f"Beatoraja reference must be pinned at {PINNED_COMMIT}; found {commit}")
    if require_clean and run("status", "--porcelain").strip():
        raise AuditError("Beatoraja reference has uncommitted changes")
    provenance = []
    for relative_path, symbol, behavior in SOURCE_PROVENANCE:
        path = root / relative_path
        if not path.is_file():
            raise AuditError(f"required pinned source is missing: {relative_path}")
        text = path.read_text(encoding="utf-8")
        symbol_name = symbol.rsplit(".", 1)[-1]
        if symbol_name not in text:
            raise AuditError(f"required pinned symbol is missing: {relative_path}:{symbol}")
        provenance.append(source_provenance(relative_path, symbol, behavior))
    return provenance


def source_provenance(path: str, symbol: str, behavior: str) -> dict:
    return {
        "commit": PINNED_COMMIT,
        "path": path,
        "symbol": symbol,
        "behavior": behavior,
    }


def provenance_for(symbol_suffix: str) -> list[dict]:
    for path, symbol, behavior in SOURCE_PROVENANCE:
        if symbol.endswith(symbol_suffix):
            return [source_provenance(path, symbol, behavior)]
    raise AuditError(f"internal provenance mapping is missing: {symbol_suffix}")


def opaque_id(domain: str, value: str) -> str:
    digest = hashlib.sha256((domain + "\0" + value).encode("utf-8")).hexdigest()
    return f"{domain}-{digest[:24]}"


def file_kind(path: str) -> str:
    extension = PurePosixPath(path).suffix.lower()
    if extension in {".lua", ".luaskin"}:
        return "lua"
    if extension in {".ttf", ".otf", ".fnt", ".woff", ".woff2"}:
        return "font"
    if extension in {".ogg", ".wav", ".mp3", ".flac", ".aac", ".m4a"}:
        return "audio"
    if extension in {".mp4", ".avi", ".mkv", ".webm", ".mov", ".mpg", ".mpeg"}:
        return "video"
    if extension in {".zip", ".7z", ".rar", ".tar", ".gz", ".bz2", ".xz"}:
        return "archive"
    if extension in {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".tga", ".svg"}:
        return "image"
    return "other"


def choose_entry(lua_text: dict[str, str]) -> str:
    candidates = sorted(
        (
            path for path in lua_text
            if path.lower().endswith(".luaskin") and re.search(r"(?:^|[/_-])play7(?:[/_.-]|$)", path, re.IGNORECASE)
        ),
        key=lambda path: path.encode("utf-8"),
    )
    if not candidates:
        candidates = sorted(
            (path for path in lua_text if path.lower().endswith(".luaskin") and "7" in PurePosixPath(path).name),
            key=lambda path: path.encode("utf-8"),
        )
    if not candidates:
        raise AuditError("archive has no identifiable 7-key Lua skin entry")
    return candidates[0]


def loaded_lua_closure(entry: str, lua_text: dict[str, str]):
    loaded: dict[str, str] = {}
    criticality: dict[str, str] = {entry: "critical"}
    host_modules: dict[str, str] = {}
    queue = [entry]
    while queue:
        path = queue.pop(0)
        if path in loaded:
            continue
        text = lua_text.get(path)
        if text is None:
            raise AuditError(f"selected Lua dependency is missing from the archive: {path!r}")
        loaded[path] = text
        for module_name in re.findall(r'''\brequire\s*\(?\s*["']([^"']+)["']''', text):
            module_path = module_name.replace(".", "/") + ".lua"
            if module_path in lua_text:
                if module_path not in criticality:
                    criticality[module_path] = "critical"
                    queue.append(module_path)
            else:
                host_modules[module_name] = "critical"
        for module_path in re.findall(
            r'''skin_config\.get_path\s*\(\s*["']([^"']+\.lua)["']''', text
        ):
            normalized = normalize_relative_path(module_path)
            if normalized not in lua_text:
                raise AuditError(f"selected Lua dependency is missing from the archive: {normalized!r}")
            if normalized not in criticality:
                criticality[normalized] = "optional"
                queue.append(normalized)
    return loaded, criticality, host_modules


def read_constant_definitions(loaded: dict[str, str]):
    definitions: dict[tuple[str, str], int] = {}
    definition_files = {
        "OP": "Root/mainoption.lua",
        "NUM": "Root/mainnumber.lua",
        "TIMER": "Root/maintimer.lua",
        "STRING": "Root/mainstring.lua",
        "BUTTON": "Root/mainbutton.lua",
        "GRAPH": "Root/maingraph.lua",
        "SLIDER": "Root/mainslider.lua",
        "OFFSET": "Root/mainoffset.lua",
    }
    for category, path in definition_files.items():
        text = loaded.get(path, "")
        for name, value in re.findall(r"(?m)^\s*([A-Z][A-Z0-9_]*)\s*=\s*(-?\d+)\s*,?", text):
            definitions[(category, name)] = int(value)
    return definitions


def surface_evidence(kind: str, item_id: str, criticality: str, provenance: list[dict]) -> dict:
    if criticality not in {"critical", "optional"}:
        raise AuditError(f"dependency lacks a critical/optional disposition: {kind}:{item_id}")
    if not provenance:
        raise AuditError(f"dependency lacks pinned source provenance: {kind}:{item_id}")
    return {
        "kind": kind,
        "id": item_id,
        "criticality": criticality,
        "provenance": provenance,
    }


def build_surface(entry: str, loaded: dict[str, str], module_criticality, host_modules):
    combined = "\n".join(loaded.values())
    surface = []
    for name in OBJECT_NAMES:
        if re.search(rf"(?:\.|\b){re.escape(name)}\s*=", combined):
            suffix = {
                "note": "prepare",
                "bga": "drawBGA",
                "destination": "setDestination",
            }.get(name, "loadJsonSkin")
            surface.append(
                surface_evidence(
                    "object",
                    name,
                    "critical" if name in {"note", "destination"} else "optional",
                    provenance_for(suffix),
                )
            )

    definitions = read_constant_definitions(loaded)
    property_ids: set[tuple[str, str]] = set()
    timer_ids: set[str] = set()
    event_ids: set[str] = set()
    for category, name in re.findall(r"\bMAIN\.(OP|NUM|GRAPH|STRING|SLIDER|OFFSET|TIMER|BUTTON)\.([A-Z][A-Z0-9_]*)", combined):
        value = definitions.get((category, name))
        stable_value = str(value) if value is not None else f"name:{name}"
        if category == "TIMER":
            timer_ids.add(stable_value)
        elif category == "BUTTON":
            event_ids.add(stable_value)
        else:
            property_ids.add((PROPERTY_CATEGORIES[category], stable_value))
    direct_calls = {
        "option": "boolean",
        "number": "integer",
        "float_number": "float",
        "text": "string",
        "event_index": "integer",
    }
    for api, property_type in direct_calls.items():
        for raw_id in re.findall(rf"\bmain_state\.{api}\s*\(\s*(-?\d+|[\"'][^\"']+[\"'])", combined):
            property_ids.add((property_type, raw_id.strip("\"'")))
    for raw_id in re.findall(r"\bmain_state\.timer\s*\(\s*(-?\d+|[\"'][^\"']+[\"'])", combined):
        timer_ids.add(raw_id.strip("\"'"))
    for raw_id in re.findall(r"\bmain_state\.event_exec\s*\(\s*(-?\d+|[\"'][^\"']+[\"'])", combined):
        event_ids.add(raw_id.strip("\"'"))

    property_provenance = {
        "boolean": "getBooleanProperty",
        "integer": "getIntegerProperty",
        "float": "getRateProperty",
        "string": "getStringProperty",
        "offset": "exportSkinProperty",
    }
    for property_type, item_id in sorted(property_ids):
        surface.append(
            surface_evidence(
                "property",
                f"{property_type}:{item_id}",
                "critical",
                provenance_for(property_provenance[property_type]),
            )
        )
    for item_id in sorted(timer_ids, key=lambda value: value.encode("utf-8")):
        surface.append(surface_evidence("timer", item_id, "critical", provenance_for("getTimerProperty")))
    for item_id in sorted(event_ids, key=lambda value: value.encode("utf-8")):
        surface.append(surface_evidence("event", item_id, "optional", provenance_for("getEvent")))

    for path, disposition in sorted(module_criticality.items(), key=lambda item: item[0].encode("utf-8")):
        surface.append(
            surface_evidence(
                "module",
                opaque_id("module", path),
                disposition,
                provenance_for("setDirectory"),
            )
        )
    for module_name, disposition in sorted(host_modules.items()):
        module_id = "host-main-state" if module_name == "main_state" else opaque_id("module", "host:" + module_name)
        behavior = "install" if module_name == "luajava" else "setDirectory"
        surface.append(surface_evidence("module", module_id, disposition, provenance_for(behavior)))

    file_apis = set(re.findall(
        r"\b(?:main_state|skin_config|io|luajava)\.[A-Za-z_][A-Za-z0-9_]*|\b(?:require|dofile|loadfile)\b",
        combined,
    ))
    for api in sorted(file_apis):
        if api.startswith("main_state."):
            provenance = provenance_for("execFile")
        elif api.startswith("skin_config."):
            provenance = provenance_for("exportSkinProperty")
        elif api.startswith("luajava."):
            provenance = provenance_for("LegacySkinLuaApi.install")
        else:
            provenance = provenance_for("setDirectory")
        surface.append(surface_evidence("file-api", api, "critical", provenance))
    surface.sort(key=lambda item: (item["kind"], item["id"]))
    return surface


def pending_field(value=None) -> dict:
    return {"status": "pending", "value": value}


def acceptance_contract(archive_sha: str, tree_sha: str, entry_sha: str) -> dict:
    layouts = [
        {"aspect": aspect, "mode": mode, "status": "pending", "evidenceReference": None}
        for aspect in ("16:9", "4:3")
        for mode in ("fit", "stretch", "custom")
    ]
    criteria = (
        "portable-tests", "desktop-main-build", "ios-release-verification",
        "zip-install", "folder-import", "manual-files-install", "entry-discovery",
        "configuration-persistence", "seven-key-gameplay", "bga-and-hud",
        "fit-layout", "stretch-layout", "custom-layout", "compatibility-diagnostics",
        "sandbox", "same-frame-fallback", "built-in-renderer-regression", "performance",
    )
    return {
        "schemaVersion": 1,
        "hardwareModel": pending_field("non-unique model identifier required"),
        "iPadOS": pending_field("exact version required"),
        "drawableSize": pending_field({"width": None, "height": None}),
        "safeInsets": pending_field({"top": None, "right": None, "bottom": None, "left": None}),
        "configuredHz": pending_field(),
        "measurementBuild": pending_field({"commit": None, "configuration": None, "sourceClean": None}),
        "externalDigests": {
            "archiveSha256": archive_sha,
            "payloadTreeSha256": tree_sha,
            "entrySha256": entry_sha,
            "configurationSha256": pending_field(),
        },
        "syntheticChartHashes": [
            {"scenario": scenario, "sha256": None, "status": "pending"}
            for scenario in ("normal-notes", "long-note-variants", "timing-gimmicks", "failure-and-song-end")
        ],
        "autoplayScripts": [
            {"scenario": scenario, "scriptSha256": None, "status": "pending"}
            for scenario in ("normal-notes", "long-note-variants", "timing-gimmicks", "failure-and-song-end")
        ],
        "screenshotTimestamps": [
            {"aspect": layout["aspect"], "mode": layout["mode"], "timestampsMicros": [], "status": "pending"}
            for layout in layouts
        ],
        "protocol": {"warmupSeconds": 30, "measurementSeconds": 180, "repetitions": 3},
        "layouts": layouts,
        "limits": {
            "p99SkinCpuFrameFraction": 0.9,
            "missedPresentationPercent": 0.5,
            "residentMemoryDriftMiB": 32,
            "activeRenderFilesystemReads": 0,
            "activeRenderUploads": 0,
            "liveResourceGrowthAfterTenExits": 0,
        },
        "completionCriteria": [
            {"id": criterion, "status": "pending", "evidenceReference": None}
            for criterion in criteria
        ],
    }


def build_manifest(arguments, archive_data, disk_tree_sha, provenance):
    lua_text = archive_data["luaText"]
    entry = choose_entry(lua_text)
    loaded, module_criticality, host_modules = loaded_lua_closure(entry, lua_text)
    surface = build_surface(entry, loaded, module_criticality, host_modules)
    payload_by_id = {record["id"]: record for record in archive_data["payloads"]}
    entry_payload_id = opaque_id("payload", entry)
    entry_sha = payload_by_id[entry_payload_id]["sha256"]
    archive_payload = {
        "id": opaque_id("payload", "archive:" + archive_data["archiveSha256"]),
        "kind": "archive",
        "byteCount": archive_data["archiveByteCount"],
        "sha256": archive_data["archiveSha256"],
    }
    payloads = sorted(
        [*archive_data["payloads"], archive_payload],
        key=lambda item: item["id"],
    )
    return {
        "schemaVersion": 1,
        "beatorajaCommit": PINNED_COMMIT,
        "targetVersion": TARGET_VERSION,
        "archiveFilename": arguments.archive_path.name,
        "archiveByteCount": archive_data["archiveByteCount"],
        "archiveSha256": archive_data["archiveSha256"],
        "archivePackagePrefix": archive_data["prefix"],
        "archivePayloadTreeSha256": archive_data["treeSha256"],
        "auditedSourceTreeSha256": disk_tree_sha,
        "extractedPackageRootIdentity": "skin-tree:" + disk_tree_sha,
        "officialSource": {"url": OFFICIAL_SOURCE_URL, "accessed": ACQUISITION_DATE},
        "usageTerms": {
            "url": TERMS_URL,
            "accessed": ACQUISITION_DATE,
            "localTestingPermitted": True,
            "privateScreenshotsPermitted": True,
            "redistributionPermitted": False,
        },
        "acceptanceContract": acceptance_contract(archive_data["archiveSha256"], disk_tree_sha, entry_sha),
        "entries": [
            {
                "identity": "entry-" + hashlib.sha256(entry.encode("utf-8")).hexdigest()[:24],
                "path": entry,
                "format": "lua",
                "keys": 7,
                "sha256": entry_sha,
                "criticality": "critical",
            }
        ],
        "surface": surface,
        "externalPayloadDigests": payloads,
        "traceVersions": {
            "luaLanguage": 1,
            "destination": 1,
            "properties": 1,
            "timersEvents": 1,
        },
        "sourceProvenance": provenance,
    }


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--beatoraja-root", required=True, type=Path)
    parser.add_argument("--archive-path", required=True, type=Path)
    parser.add_argument("--archive-package-prefix", required=True)
    parser.add_argument("--skin-root", required=True, type=Path)
    parser.add_argument("--expected-archive-sha256")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path)
    mode.add_argument("--verify", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        if arguments.expected_archive_sha256 is not None and not re.fullmatch(
            r"[0-9a-f]{64}", arguments.expected_archive_sha256
        ):
            raise AuditError("--expected-archive-sha256 must be a lowercase 64-character SHA-256")
        provenance = validate_reference_root(arguments.beatoraja_root)
        archive_data = inspect_archive(arguments.archive_path, arguments.archive_package_prefix)
        if (
            arguments.expected_archive_sha256 is not None
            and archive_data["archiveSha256"] != arguments.expected_archive_sha256
        ):
            raise AuditError(
                "archive SHA-256 mismatch: expected "
                f"{arguments.expected_archive_sha256}, computed {archive_data['archiveSha256']}"
            )
        disk_tree_sha, _ = inspect_disk_tree(arguments.skin_root)
        if disk_tree_sha != archive_data["treeSha256"]:
            raise AuditError(
                "archive payload and extracted skin root have different SkinTreeDigestV1 values: "
                f"{archive_data['treeSha256']} != {disk_tree_sha}"
            )
        manifest = build_manifest(arguments, archive_data, disk_tree_sha, provenance)
        rendered = json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        if arguments.verify is not None:
            if not arguments.verify.is_file():
                raise AuditError(f"verification manifest does not exist: {arguments.verify}")
            expected = arguments.verify.read_text(encoding="utf-8")
            if expected != rendered:
                raise AuditError(f"verification manifest is stale: {arguments.verify}")
            print(f"Beatoraja skin reference manifest verified: {arguments.verify}")
        else:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(rendered, encoding="utf-8")
            print(f"Beatoraja skin reference manifest written: {arguments.output}")
        return 0
    except (AuditError, OSError, UnicodeError, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
