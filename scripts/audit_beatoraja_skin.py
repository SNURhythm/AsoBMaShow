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
AUDITED_GUARD_CONFIGURATION_DOMAIN = b"ASOBMSKIN-AUDITED-GUARD-CONFIG-V1\0"
OPAQUE_GUARD_VECTOR_DOMAIN = b"ASOBMSKIN-OPAQUE-GUARD-VECTOR-V1\0"
RENDER_IO_NEGATIVE_SCENARIO_ID = "scenario-f7395bddf2b0f715a900b5cd"

MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MAX_REGULAR_FILE_BYTES = 512 * 1024 * 1024
MAX_EXPANDED_BYTES = 4 * 1024 * 1024 * 1024
MAX_FILES = 20_000
MAX_PATH_BYTES = 1_024
MAX_PATH_COMPONENTS = 64
MAX_LUA_SOURCE_BYTES = 16 * 1024 * 1024
MAX_LUA_TOKENS = 1_000_000
SUPPORTED_COMPRESSION = {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}

SOURCE_PROVENANCE = (
    ("src/bms/player/beatoraja/skin/SkinLoader.java", "SkinLoader.load", "selects .luaskin entries and delegates them to LuaSkinLoader"),
    ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.loadHeader", "executes the entry with nil skin_config and converts the returned header table"),
    ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.load", "applies configuration, executes the same entry again, and loads the converted gameplay skin"),
    ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.fromLuaValue", "converts Lua tables recursively by reflected model field name"),
    ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.serializeLuaScript", "dispatches callback values as function, number, recognized name, or script string"),
    ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.setDirectory", "sets the package-local Lua module search directory"),
    ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.execFile", "executes a Lua file in the retained Globals instance"),
    ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.exportMainStateAccessor", "publishes the main_state module for the configured main-state phase"),
    ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.RestrictedIoLib.openFile", "opens only files accepted by the package-rooted restricted Lua IO facade"),
    ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.exportSkinProperty", "publishes selected file, option, enabled-option, and offset configuration"),
    ("src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java", "LegacySkinLuaApi.install", "installs the restricted legacy luajava class, constructor, file, GDX, controller, and HTTP facades"),
    ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.export", "publishes property, timer, event, score, gauge, volume, and judgment functions on main_state"),
    ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.OptionFunction", "implements direct main_state.option calls through BooleanPropertyFactory and immediately reads the result"),
    ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.NumberFunction", "implements direct main_state.number calls through IntegerPropertyFactory and immediately reads the result"),
    ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.FloatNumberFunction", "implements direct main_state.float_number calls through FloatPropertyFactory and immediately reads the result"),
    ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.TextFunction", "implements direct main_state.text calls through StringPropertyFactory and immediately reads the result"),
    ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.TimerFunction", "implements direct main_state.timer calls by reading the requested microtimer from MainState"),
    ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.EventExecFunction", "implements direct main_state.event_exec calls by validating and executing the requested event on MainState"),
    ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.EventIndexFunction", "implements direct main_state.event_index calls through the image-index property factory and immediately reads the result"),
    ("src/bms/player/beatoraja/skin/json/JSONSkinLoader.java", "JSONSkinLoader.loadJsonSkinHeader", "converts header properties, files, offsets, and categories"),
    ("src/bms/player/beatoraja/skin/json/JSONSkinLoader.java", "JSONSkinLoader.loadJsonSkin", "constructs play objects in authored destination order"),
    ("src/bms/player/beatoraja/skin/json/JSONSkinLoader.java", "JSONSkinLoader.setDestination", "inherits omitted destination fields and binds conditions, timer, clip, offsets, and stretch"),
    ("src/bms/player/beatoraja/skin/json/JsonSkin.java", "JsonSkin.Skin", "defaults absent custom-event and custom-timer model arrays to empty"),
    ("src/bms/player/beatoraja/skin/SkinHeader.java", "SkinHeader.setSkinConfigProperty", "reconciles configured custom options, files, and offsets"),
    ("src/bms/player/beatoraja/skin/Skin.java", "Skin.prepare", "removes invalid and statically disabled objects before resource load"),
    ("src/bms/player/beatoraja/skin/Skin.java", "Skin.drawAllObjects", "prepares then draws surviving objects in authored array order"),
    ("src/bms/player/beatoraja/skin/Skin.java", "Skin.updateCustomObjects", "updates the custom-timer phase before the custom-event phase using IntMap backing-hash iteration within each phase"),
    ("src/bms/player/beatoraja/skin/SkinObject.java", "SkinObject.prepareRegion", "applies destination timer, loop, interpolation, and configured offsets"),
    ("src/bms/player/beatoraja/play/PlaySkin.java", "PlaySkin", "stores play-lane line, BPM, stop, time, cover, judge, and timing configuration"),
    ("src/bms/player/beatoraja/play/SkinNote.java", "SkinNote.prepare", "samples normal, mine, hidden, processed, and ten long-note image phases"),
    ("src/bms/player/beatoraja/play/LaneRenderer.java", "LaneRenderer.drawLongNote", "selects distinct LN, CN, and HCN endpoint and body phases"),
    ("src/bms/player/beatoraja/play/SkinBGA.java", "SkinBGA.prepare", "advances BGA state at the play timer before drawing"),
    ("src/bms/player/beatoraja/play/bga/BGAProcessor.java", "BGAProcessor.drawBGA", "draws an active miss sequence exclusively, otherwise base then layer"),
    ("src/bms/player/beatoraja/skin/property/BooleanPropertyFactory.java", "BooleanPropertyFactory.getBooleanProperty", "maps supported boolean IDs or names and returns null for an unknown mapping"),
    ("src/bms/player/beatoraja/skin/property/IntegerPropertyFactory.java", "IntegerPropertyFactory.getIntegerProperty", "maps supported integer IDs or names and returns null for an unknown mapping"),
    ("src/bms/player/beatoraja/skin/property/IntegerPropertyFactory.java", "IntegerPropertyFactory.getImageIndexProperty", "maps supported image-index IDs or names and returns null for an unknown mapping"),
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


@dataclass(frozen=True)
class LuaToken:
    kind: str
    value: str


@dataclass(frozen=True)
class LuaDependency:
    target: str
    kind: str
    criticality: str


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


def read_lua_source(stream: BinaryIO) -> str:
    encoded = bytearray()
    sentinel_limit = MAX_LUA_SOURCE_BYTES + 1
    while len(encoded) < sentinel_limit:
        chunk = stream.read(min(1024 * 1024, sentinel_limit - len(encoded)))
        if not chunk:
            break
        encoded.extend(chunk)
    if len(encoded) > MAX_LUA_SOURCE_BYTES:
        raise AuditError("Lua source exceeds the 16 MiB scanner limit")
    return bytes(encoded).decode("utf-8")


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


def validate_structural_nodes(
    file_paths: Iterable[str],
    explicit_directories: Iterable[str],
    *,
    label: str,
) -> None:
    nodes: dict[str, tuple[str, str, str]] = {}

    def add(raw_path: str, kind: str) -> None:
        raw_components = raw_path.split("/")
        normalized = normalize_relative_path(raw_path, directory=kind == "directory")
        normalized_components = normalized.split("/")
        for length in range(1, len(normalized_components) + 1):
            node_kind = kind if length == len(normalized_components) else "directory"
            normalized_node = "/".join(normalized_components[:length])
            raw_node = "/".join(raw_components[:length])
            key = collision_key(normalized_node)
            previous = nodes.get(key)
            if previous is None:
                nodes[key] = (normalized_node, raw_node, node_kind)
                continue
            previous_normalized, previous_raw, previous_kind = previous
            if previous_normalized != normalized_node or previous_raw != raw_node:
                raise AuditError(
                    f"{label} structural path spelling conflict: "
                    f"{previous_raw!r} and {raw_node!r}"
                )
            if previous_kind != node_kind:
                raise AuditError(
                    f"{label} structural file/directory collision at {normalized_node!r}"
                )
            if node_kind == "file":
                raise AuditError(f"{label} duplicate structural file: {normalized_node!r}")

    for path in file_paths:
        add(path, "file")
    for path in explicit_directories:
        add(path, "directory")


def inspect_archive(archive_path: Path, declared_prefix: str):
    if not archive_path.is_file() or archive_path.is_symlink():
        raise AuditError(f"archive is not a regular file: {archive_path}")
    archive_size = archive_path.stat().st_size
    if archive_size > MAX_ARCHIVE_BYTES:
        raise AuditError("archive exceeds the 2 GiB policy limit")
    archive_digest = sha256_file(archive_path)
    regular: list[tuple[str, zipfile.ZipInfo]] = []
    explicit_directories: list[str] = []
    explicit_directory_spellings: dict[str, str] = {}
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
                explicit_directory_spellings[normalized] = info.filename[:-1]
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

        raw_stripped_files = []
        for item in stripped_files:
            raw_components = item.info.filename.split("/")
            raw_stripped_files.append(
                "/".join(raw_components[1:]) if prefix_text else item.info.filename
            )
        raw_stripped_directories = []
        for directory in explicit_directories:
            raw_directory = explicit_directory_spellings[directory]
            if prefix_text:
                if directory == inferred_prefix:
                    continue
                raw_components = raw_directory.split("/")
                raw_directory = "/".join(raw_components[1:])
            raw_stripped_directories.append(raw_directory)
        validate_structural_nodes(
            raw_stripped_files,
            raw_stripped_directories,
            label="ZIP",
        )

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
                        extracted_text[item.path] = read_lua_source(stream)
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
    raw_directories: list[str] = []
    raw_files: list[str] = []
    for current_root, directory_names, file_names in os.walk(root, topdown=True, followlinks=False):
        current = Path(current_root)
        for name in list(directory_names):
            path = current / name
            if path.is_symlink():
                raise AuditError(f"link in extracted skin tree is not allowed: {path.relative_to(root)}")
            mode = path.stat(follow_symlinks=False).st_mode
            if not stat.S_ISDIR(mode):
                raise AuditError(f"special node in extracted skin tree: {path.relative_to(root)}")
            raw_directories.append(path.relative_to(root).as_posix())
        for name in file_names:
            path = current / name
            mode = path.stat(follow_symlinks=False).st_mode
            if not stat.S_ISREG(mode):
                raise AuditError(f"link or special node in extracted skin tree: {path.relative_to(root)}")
            relative = normalize_relative_path(path.relative_to(root).as_posix())
            raw_files.append(path.relative_to(root).as_posix())
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
    validate_structural_nodes(raw_files, raw_directories, label="extracted tree")
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
        if symbol == "JsonSkin.Skin" and not all(
            re.search(
                rf"public\s+{model_type}\[\]\s+{field}\s*=\s*new\s+{model_type}\s*\[\s*0\s*\]",
                text,
            )
            for model_type, field in (
                ("CustomEvent", "customEvents"),
                ("CustomTimer", "customTimers"),
            )
        ):
            raise AuditError("pinned JsonSkin custom-object defaults are not empty arrays")
        provenance.append(source_provenance(relative_path, symbol, behavior))
    return provenance


def source_provenance(path: str, symbol: str, behavior: str) -> dict:
    return {
        "commit": PINNED_COMMIT,
        "path": path,
        "symbol": symbol,
        "behavior": behavior,
    }


def provenance_for(symbol_name: str) -> list[dict]:
    for path, symbol, behavior in SOURCE_PROVENANCE:
        if symbol == symbol_name:
            return [source_provenance(path, symbol, behavior)]
    raise AuditError(f"internal provenance mapping is missing: {symbol_name}")


def opaque_id(domain: str, value: str) -> str:
    digest = hashlib.sha256((domain + "\0" + value).encode("utf-8")).hexdigest()
    return f"{domain}-{digest[:24]}"


def _update_length_prefixed(digest, value: str) -> None:
    encoded = value.encode("utf-8")
    digest.update(struct.pack(">I", len(encoded)))
    digest.update(encoded)


def audited_guard_configuration_sha256(
    entry_sha256: str,
    source_values: list[tuple[str, str]],
) -> str:
    """Hash source identities for audit evidence, never physical configuration bytes."""
    digest = hashlib.sha256()
    digest.update(AUDITED_GUARD_CONFIGURATION_DOMAIN)
    digest.update(bytes.fromhex(entry_sha256))
    ordered = sorted(source_values, key=lambda item: item[0].encode("utf-8"))
    digest.update(struct.pack(">I", len(ordered)))
    for source_identity, value in ordered:
        _update_length_prefixed(digest, source_identity)
        _update_length_prefixed(digest, value)
    return digest.hexdigest()


def opaque_guard_vector_sha256(
    audited_configuration_sha256: str,
    guard_vector: list[dict],
) -> str:
    digest = hashlib.sha256()
    digest.update(OPAQUE_GUARD_VECTOR_DOMAIN)
    digest.update(bytes.fromhex(audited_configuration_sha256))
    ordered = sorted(guard_vector, key=lambda item: item["guardId"].encode("utf-8"))
    digest.update(struct.pack(">I", len(ordered)))
    for item in ordered:
        _update_length_prefixed(digest, item["guardId"])
        _update_length_prefixed(digest, item["value"])
    return digest.hexdigest()


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


def _long_bracket_end(text: str, start: int) -> tuple[int, int] | None:
    if start >= len(text) or text[start] != "[":
        return None
    position = start + 1
    while position < len(text) and text[position] == "=":
        position += 1
    if position >= len(text) or text[position] != "[":
        return None
    equals = position - start - 1
    closing = "]" + ("=" * equals) + "]"
    end = text.find(closing, position + 1)
    return (len(text), position + 1) if end < 0 else (end + len(closing), position + 1)


def tokenize_lua(text: str) -> list[LuaToken]:
    if len(text.encode("utf-8")) > MAX_LUA_SOURCE_BYTES:
        raise AuditError("Lua source exceeds the 16 MiB scanner limit")
    tokens: list[LuaToken] = []
    position = 0
    while position < len(text):
        character = text[position]
        if character.isspace():
            position += 1
            continue
        if text.startswith("--", position):
            long_comment = _long_bracket_end(text, position + 2)
            if long_comment is not None:
                position = long_comment[0]
            else:
                newline = text.find("\n", position + 2)
                position = len(text) if newline < 0 else newline + 1
            continue
        long_string = _long_bracket_end(text, position)
        if long_string is not None:
            end, content_start = long_string
            closing_length = 2 + (content_start - position - 2)
            tokens.append(LuaToken("string", text[content_start:end - closing_length]))
            position = end
            continue
        if character in {'"', "'"}:
            quote = character
            position += 1
            value: list[str] = []
            while position < len(text):
                character = text[position]
                if character == quote:
                    position += 1
                    break
                if character == "\\" and position + 1 < len(text):
                    escaped = text[position + 1]
                    value.append({"n": "\n", "r": "\r", "t": "\t"}.get(escaped, escaped))
                    position += 2
                    continue
                value.append(character)
                position += 1
            tokens.append(LuaToken("string", "".join(value)))
        elif character.isalpha() or character == "_":
            end = position + 1
            while end < len(text) and (text[end].isalnum() or text[end] == "_"):
                end += 1
            tokens.append(LuaToken("identifier", text[position:end]))
            position = end
        elif character.isdigit():
            end = position + 1
            while end < len(text) and (text[end].isalnum() or text[end] in ".xX+-"):
                end += 1
            tokens.append(LuaToken("number", text[position:end]))
            position = end
        else:
            symbol = next(
                (candidate for candidate in ("...", "==", "~=", "<=", ">=", "..", "::") if text.startswith(candidate, position)),
                character,
            )
            tokens.append(LuaToken("symbol", symbol))
            position += len(symbol)
        if len(tokens) > MAX_LUA_TOKENS:
            raise AuditError("Lua source exceeds the 1,000,000-token scanner limit")
    return tokens


def _lua_context_flags(tokens: list[LuaToken]) -> tuple[list[bool], list[int]]:
    blocks: list[str] = []
    calls: list[str | None] = []
    guarded: list[bool] = []
    function_depths: list[int] = []
    for index, token in enumerate(tokens):
        guarded.append(
            any(block in {"conditional", "repeat"} for block in blocks)
            or any(call in {"pcall", "xpcall"} for call in calls)
        )
        function_depths.append(sum(block == "function" for block in blocks))
        value = token.value
        if value == "function":
            blocks.append("function")
        elif value == "if":
            blocks.append("conditional")
        elif value in {"for", "while"}:
            blocks.append("conditional-wait-do")
        elif value == "repeat":
            blocks.append("repeat")
        elif value == "do":
            if blocks and blocks[-1] == "conditional-wait-do":
                blocks[-1] = "conditional"
            else:
                blocks.append("do")
        elif value == "end" and blocks:
            blocks.pop()
        elif value == "until" and blocks and blocks[-1] == "repeat":
            blocks.pop()
        if value == "(":
            previous = tokens[index - 1].value if index else None
            calls.append(previous if previous in {"pcall", "xpcall"} else None)
        elif value == ")" and calls:
            calls.pop()
    return guarded, function_depths


def _lua_guarded_flags(tokens: list[LuaToken]) -> list[bool]:
    return _lua_context_flags(tokens)[0]


def _call_string_argument(tokens: list[LuaToken], index: int) -> str | None:
    if index + 1 >= len(tokens):
        return None
    argument = index + 1
    if tokens[argument].value == "(":
        argument += 1
    if argument < len(tokens) and tokens[argument].kind == "string":
        return tokens[argument].value
    return None


def scan_lua_dependencies(text: str) -> list[LuaDependency]:
    tokens = tokenize_lua(text)
    guarded = _lua_guarded_flags(tokens)
    path_variables: dict[str, str] = {}
    ambiguous_variables: set[str] = set()
    for index in range(len(tokens) - 7):
        values = [token.value for token in tokens[index:index + 8]]
        if (
            tokens[index].kind == "identifier"
            and values[1:7] == ["=", "skin_config", ".", "get_path", "(", values[6]]
            and tokens[index + 6].kind == "string"
            and values[7] == ")"
            and values[6].lower().endswith(".lua")
        ):
            variable = values[0]
            path = values[6]
            if variable in path_variables and path_variables[variable] != path:
                ambiguous_variables.add(variable)
            path_variables[variable] = path
    for variable in ambiguous_variables:
        path_variables.pop(variable, None)

    dependencies: list[LuaDependency] = []
    for index, token in enumerate(tokens):
        if token.value == "require":
            module_name = _call_string_argument(tokens, index)
            if module_name is not None:
                dependencies.append(
                    LuaDependency(
                        module_name,
                        "require",
                        "optional" if guarded[index] else "critical",
                    )
                )
        if token.value in {"pcall", "xpcall"} and index + 4 < len(tokens):
            if (
                tokens[index + 1].value == "("
                and tokens[index + 2].value == "require"
                and tokens[index + 3].value == ","
                and tokens[index + 4].kind == "string"
            ):
                dependencies.append(LuaDependency(tokens[index + 4].value, "require", "optional"))
        if token.value not in {"dofile", "loadfile"}:
            continue
        argument = index + 1
        if argument < len(tokens) and tokens[argument].value == "(":
            argument += 1
        path = None
        if argument < len(tokens) and tokens[argument].kind == "string":
            path = tokens[argument].value
        elif argument < len(tokens) and tokens[argument].kind == "identifier":
            path = path_variables.get(tokens[argument].value)
        elif argument + 5 < len(tokens):
            values = [candidate.value for candidate in tokens[argument:argument + 6]]
            if (
                values[:5] == ["skin_config", ".", "get_path", "(", values[4]]
                and tokens[argument + 4].kind == "string"
                and values[5] == ")"
            ):
                path = values[4]
        if path is not None and path.lower().endswith(".lua"):
            dependencies.append(
                LuaDependency(
                    path,
                    "file",
                    "optional" if guarded[index] else "critical",
                )
            )
    return dependencies


def analyze_legacy_lua_api(
    loaded: dict[str, str],
    module_criticality: dict[str, str],
) -> dict:
    import_count = 0
    import_helpers: set[str] = set()
    critical_top_level_imports = 0
    bind_classes: dict[str, dict[str, int]] = {}
    constructor_sites = 0
    mkdir_sites = 0
    list_files_sites = 0
    selected_dot_calls: set[str] = set()
    audio_initialization_sites = 0
    audio_play_sites = 0
    audio_dispose_sites = 0
    guarded_audio_sites = 0

    for path, text in loaded.items():
        tokens = tokenize_lua(text)
        guarded, function_depths = _lua_context_flags(tokens)
        for index, token in enumerate(tokens):
            if token.value == "require" and _call_string_argument(tokens, index) == "luajava":
                import_count += 1
                import_helpers.add(path)
                if (
                    module_criticality.get(path) == "critical"
                    and not guarded[index]
                    and function_depths[index] == 0
                ):
                    critical_top_level_imports += 1
            if index + 5 < len(tokens) and [candidate.value for candidate in tokens[index:index + 4]] == [
                "luajava", ".", "bindClass", "("
            ] and tokens[index + 4].kind == "string" and tokens[index + 5].value == ")":
                class_name = tokens[index + 4].value
                record = bind_classes.setdefault(class_name, {"count": 0, "criticalTopLevel": 0})
                record["count"] += 1
                if (
                    module_criticality.get(path) == "critical"
                    and not guarded[index]
                    and function_depths[index] == 0
                ):
                    record["criticalTopLevel"] += 1
            if index + 3 < len(tokens) and [candidate.value for candidate in tokens[index:index + 4]] == [
                "luajava", ".", "new", "("
            ]:
                constructor_sites += 1
            if index + 2 < len(tokens) and tokens[index].value == ":" and tokens[index + 2].value == "(":
                if tokens[index + 1].value == "mkdir":
                    mkdir_sites += 1
                elif tokens[index + 1].value == "listFiles":
                    list_files_sites += 1
                elif tokens[index - 1].value == "audio" and tokens[index + 1].value == "play":
                    audio_play_sites += 1
                    guarded_audio_sites += int(guarded[index - 1])
                elif tokens[index - 1].value == "audio" and tokens[index + 1].value == "dispose":
                    audio_dispose_sites += 1
                    guarded_audio_sites += int(guarded[index - 1])
            if (
                token.kind == "identifier"
                and index > 0
                and index + 1 < len(tokens)
                and tokens[index - 1].value == "."
                and tokens[index + 1].value == "("
            ):
                selected_dot_calls.add(token.value)
            if token.value == "gdx" and index + 9 < len(tokens):
                window = [candidate.value for candidate in tokens[index:index + 10]]
                if window == [
                    "gdx", ".", "app", ":", "getApplicationListener", "(", ")",
                    ":", "getAudioProcessor", "(",
                ]:
                    audio_initialization_sites += 1
                    guarded_audio_sites += int(guarded[index])

    bind_surface = []
    for class_name, record in sorted(bind_classes.items()):
        critical = record["count"] == record["criticalTopLevel"]
        bind_surface.append(
            {
                "className": class_name,
                "siteCount": record["count"],
                "criticality": "critical" if critical else "optional",
                "reachability": "unguarded-top-level" if critical else "guarded-or-deferred",
            }
        )
    audio_site_count = audio_initialization_sites + audio_play_sites + audio_dispose_sites
    mkdir_reachable = "mkdir" in selected_dot_calls
    list_files_reachable = (
        "addSourceSP" in selected_dot_calls
        and "randomChoiceStep1" in selected_dot_calls
    )
    reachable_constructor_sites = int(mkdir_reachable) + int(list_files_reachable)
    return {
        "module": "luajava",
        "helperCount": len(import_helpers),
        "imports": {
            "siteCount": import_count,
            "criticality": "critical" if import_count == critical_top_level_imports else "optional",
            "reachability": "unguarded-top-level" if import_count == critical_top_level_imports else "guarded-or-deferred",
        },
        "bindClass": bind_surface,
        "fileFacade": {
            "constructorSiteCount": constructor_sites,
            "reachableConstructorSiteCount": reachable_constructor_sites,
            "mkdirSiteCount": mkdir_sites,
            "mkdirReachableFromSelectedEntry": mkdir_reachable,
            "listFilesSiteCount": list_files_sites,
            "listFilesReachableFromSelectedEntry": list_files_reachable,
            "criticality": "critical" if reachable_constructor_sites else "optional",
            "reachability": (
                "configured-load-listFiles-deferred-mkdir"
                if list_files_reachable and not mkdir_reachable
                else "selected-and-deferred-sites"
                if reachable_constructor_sites
                else "deferred-no-selected-callers"
            ),
        },
        "audioFacade": {
            "initializationSiteCount": audio_initialization_sites,
            "playSiteCount": audio_play_sites,
            "disposeSiteCount": audio_dispose_sites,
            "criticality": "optional" if audio_site_count == guarded_audio_sites else "critical",
            "reachability": "pcall-guarded" if audio_site_count == guarded_audio_sites else "unguarded-sites-present",
        },
    }


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
    dependency_edges: dict[str, list[LuaDependency]] = {}
    queue = [entry]
    while queue:
        path = queue.pop(0)
        text = loaded.get(path)
        if text is None:
            text = lua_text.get(path)
            if text is None:
                raise AuditError(f"selected Lua dependency is missing from the archive: {path!r}")
            loaded[path] = text
            dependency_edges[path] = scan_lua_dependencies(text)
        parent_disposition = criticality[path]
        for dependency in dependency_edges[path]:
            disposition = (
                "critical"
                if parent_disposition == "critical" and dependency.criticality == "critical"
                else "optional"
            )
            if dependency.kind == "require":
                module_path = dependency.target.replace(".", "/") + ".lua"
                if module_path not in lua_text:
                    previous = host_modules.get(dependency.target)
                    if previous is None or (previous == "optional" and disposition == "critical"):
                        host_modules[dependency.target] = disposition
                    continue
            else:
                module_path = normalize_relative_path(dependency.target)
                if module_path not in lua_text:
                    raise AuditError(
                        f"selected Lua dependency is missing from the archive: {module_path!r}"
                    )
            previous = criticality.get(module_path)
            if previous is None or (previous == "optional" and disposition == "critical"):
                criticality[module_path] = disposition
                queue.append(module_path)
    return loaded, criticality, host_modules


def _function_ranges(tokens: list[LuaToken]) -> list[tuple[int, int, str | None]]:
    _, function_depths = _lua_context_flags(tokens)
    ranges = []
    for start, token in enumerate(tokens):
        if token.value != "function":
            continue
        name = None
        if (
            start >= 3
            and tokens[start - 1].value == "="
            and tokens[start - 2].kind == "identifier"
            and tokens[start - 3].value in {".", ":"}
        ):
            name = tokens[start - 2].value
        elif start + 1 < len(tokens) and tokens[start + 1].kind == "identifier":
            name = tokens[start + 1].value
        depth = function_depths[start]
        end = start + 1
        while end < len(tokens) and function_depths[end] > depth:
            end += 1
        ranges.append((start, end, name))
    return ranges


def _direct_function_facts(
    tokens: list[LuaToken],
    start: int,
    end: int,
) -> tuple[bool, set[str]]:
    _, function_depths = _lua_context_flags(tokens)
    direct_depth = function_depths[start] + 1
    direct_io = False
    calls = set()
    for index in range(start + 1, end):
        if function_depths[index] != direct_depth:
            continue
        if (
            index + 2 < end
            and function_depths[index + 1] == direct_depth
            and function_depths[index + 2] == direct_depth
            and [token.value for token in tokens[index:index + 3]] == ["io", ".", "open"]
        ):
            direct_io = True
        if (
            tokens[index].kind == "identifier"
            and index + 1 < end
            and function_depths[index + 1] == direct_depth
            and tokens[index + 1].value == "("
            and not (
                index > start
                and tokens[index - 1].value in {"function", "if", "elseif", "while", "for"}
            )
        ):
            calls.add(tokens[index].value)
    return direct_io, calls


def _io_bearing_function_names(loaded: dict[str, str]) -> set[str]:
    functions: dict[str, tuple[bool, set[str]]] = {}
    for text in loaded.values():
        tokens = tokenize_lua(text)
        for start, end, name in _function_ranges(tokens):
            if name is None:
                continue
            direct_io, calls = _direct_function_facts(tokens, start, end)
            previous_direct, previous_calls = functions.get(name, (False, set()))
            functions[name] = (previous_direct or direct_io, previous_calls | calls)
    io_bearing = {name for name, (direct, _) in functions.items() if direct}
    changed = True
    while changed:
        changed = False
        for name, (_, calls) in functions.items():
            if name not in io_bearing and calls & io_bearing:
                io_bearing.add(name)
                changed = True
    return io_bearing


def _condition_property_guards(tokens: list[LuaToken], start: int) -> set[str]:
    guards = set()
    for index in range(start + 1, len(tokens) - 3):
        if tokens[index].value == "then":
            break
        if (
            tokens[index].value == "PROPERTY"
            and tokens[index + 1].value == "."
            and tokens[index + 2].kind == "identifier"
            and tokens[index + 3].value == "("
        ):
            guards.add(tokens[index + 2].value)
    return guards


def _active_property_guards(tokens: list[LuaToken]) -> list[set[str]]:
    blocks: list[dict] = []
    result = []
    for index, token in enumerate(tokens):
        result.append(set().union(*(block["guards"] for block in blocks)) if blocks else set())
        value = token.value
        if value == "function":
            blocks.append({"kind": "function", "guards": set()})
        elif value == "if":
            blocks.append({"kind": "if", "guards": _condition_property_guards(tokens, index)})
        elif value == "elseif":
            for block in reversed(blocks):
                if block["kind"] == "if":
                    block["guards"] = _condition_property_guards(tokens, index)
                    break
        elif value == "else":
            for block in reversed(blocks):
                if block["kind"] == "if":
                    block["guards"] = set()
                    break
        elif value in {"for", "while"}:
            blocks.append({"kind": "loop-wait-do", "guards": set()})
        elif value == "repeat":
            blocks.append({"kind": "repeat", "guards": set()})
        elif value == "do":
            if blocks and blocks[-1]["kind"] == "loop-wait-do":
                blocks[-1]["kind"] = "loop"
            else:
                blocks.append({"kind": "do", "guards": set()})
        elif value == "end" and blocks:
            blocks.pop()
        elif value == "until" and blocks and blocks[-1]["kind"] == "repeat":
            blocks.pop()
    return result


def _path_variables(tokens: list[LuaToken]) -> dict[str, str]:
    paths: dict[str, str] = {}
    ambiguous = set()
    for index in range(len(tokens) - 7):
        if (
            tokens[index].kind == "identifier"
            and [token.value for token in tokens[index + 1:index + 6]]
            == ["=", "skin_config", ".", "get_path", "("]
            and tokens[index + 6].kind == "string"
            and tokens[index + 7].value == ")"
        ):
            variable = tokens[index].value
            path = tokens[index + 6].value
            if variable in paths and paths[variable] != path:
                ambiguous.add(variable)
            paths[variable] = path
    for variable in ambiguous:
        paths.pop(variable, None)
    return paths


def _dofile_targets(tokens: list[LuaToken]) -> dict[int, str]:
    variables = _path_variables(tokens)
    targets = {}
    for index, token in enumerate(tokens):
        if token.value != "dofile":
            continue
        argument = index + 1
        if argument < len(tokens) and tokens[argument].value == "(":
            argument += 1
        target = None
        if argument < len(tokens) and tokens[argument].kind == "string":
            target = tokens[argument].value
        elif argument < len(tokens) and tokens[argument].kind == "identifier":
            target = variables.get(tokens[argument].value)
        if target is not None:
            targets[index] = normalize_relative_path(target)
    return targets


def _has_retained_io_callback(tokens: list[LuaToken], io_functions: set[str]) -> bool:
    retained_fields = {"act", "draw", "event", "timer", "value"}
    for start, end, _ in _function_ranges(tokens):
        if (
            start < 2
            or tokens[start - 1].value != "="
            or tokens[start - 2].value not in retained_fields
        ):
            continue
        direct_io, calls = _direct_function_facts(tokens, start, end)
        if direct_io or calls & io_functions:
            return True
    return False


def _render_io_guard_sources(loaded: dict[str, str]) -> list[str]:
    io_functions = _io_bearing_function_names(loaded)
    retained_io_files = {
        path for path, text in loaded.items()
        if _has_retained_io_callback(tokenize_lua(text), io_functions)
    }
    guard_names = set()
    for text in loaded.values():
        tokens = tokenize_lua(text)
        active = _active_property_guards(tokens)
        dofile_targets = _dofile_targets(tokens)
        for index, token in enumerate(tokens):
            if (
                token.kind == "identifier"
                and token.value in io_functions
                and index >= 4
                and [candidate.value for candidate in tokens[index - 4:index]]
                == ["CUSTOM", ".", "FUNC", "."]
                and index + 1 < len(tokens)
                and tokens[index + 1].value == "("
            ):
                guard_names.update(active[index])
            target = dofile_targets.get(index)
            if target in retained_io_files:
                guard_names.update(active[index])

    sources = []
    for guard_name in sorted(guard_names, key=lambda value: value.encode("utf-8")):
        definitions = []
        for path, text in loaded.items():
            tokens = tokenize_lua(text)
            values = [token.value for token in tokens]
            for index in range(len(tokens) - 7):
                if (
                    values[index:index + 4] == ["module", ".", guard_name, "="]
                    and values[index + 4:index + 7] == ["customoption", ".", "chiled"]
                    and values[index + 7] == "("
                ):
                    definitions.append(path + "\0" + guard_name)
        if len(definitions) != 1:
            raise AuditError(
                f"render-I/O guard must have exactly one selected-closure definition: {guard_name!r}"
            )
        sources.append(definitions[0])
    return sources


def _call_argument_count(tokens: list[LuaToken], open_index: int) -> int:
    if open_index >= len(tokens) or tokens[open_index].value != "(":
        raise AuditError("internal Lua call scanner lost its opening parenthesis")
    if open_index + 1 < len(tokens) and tokens[open_index + 1].value == ")":
        return 0
    parentheses = braces = brackets = 0
    commas = 0
    for index in range(open_index, len(tokens)):
        value = tokens[index].value
        if value == "(":
            parentheses += 1
        elif value == ")":
            parentheses -= 1
            if parentheses == 0:
                return commas + 1
        elif value == "{":
            braces += 1
        elif value == "}":
            braces -= 1
        elif value == "[":
            brackets += 1
        elif value == "]":
            brackets -= 1
        elif value == "," and parentheses == 1 and braces == 0 and brackets == 0:
            commas += 1
    raise AuditError("unterminated Lua call in selected closure")


def _audited_guard_configurations(entry_sha: str, guard_sources: list[str]) -> list[dict]:
    guard_pairs = [(source, opaque_id("guard", source)) for source in guard_sources]

    def configuration(role: str, reachable_guard_id: str | None = None) -> dict:
        source_values = [
            (source, "reachable" if guard_id == reachable_guard_id else "not-reachable")
            for source, guard_id in guard_pairs
        ]
        audited_sha = audited_guard_configuration_sha256(entry_sha, source_values)
        guard_vector = sorted(
            [
                {"guardId": guard_id, "value": value}
                for (source, guard_id), (_, value) in zip(guard_pairs, source_values)
            ],
            key=lambda item: item["guardId"].encode("utf-8"),
        )
        vector_sha = opaque_guard_vector_sha256(audited_sha, guard_vector)
        return {
            "id": opaque_id("configuration", role + "\0" + audited_sha),
            "role": role,
            "auditedGuardConfigurationSha256": audited_sha,
            "guardVectorSha256": vector_sha,
            "guardVector": guard_vector,
        }

    passing = configuration("passing")
    if not guard_pairs:
        return [passing]
    first_guard_id = min((guard_id for _, guard_id in guard_pairs), key=lambda value: value.encode("utf-8"))
    return [passing, configuration("negative", first_guard_id)]


def analyze_selected_file_io_surface(
    loaded: dict[str, str],
    entry_sha: str,
) -> tuple[dict, dict[str, int]]:
    token_sets = [tokenize_lua(text) for text in loaded.values()]
    all_tokens = [token for tokens in token_sets for token in tokens]
    for object_name in ("customTimers", "customEvents"):
        if any(token.value == object_name for token in all_tokens):
            raise AuditError(
                f"selected custom object map {object_name!r} is not statically proven empty"
            )

    dofile_count = 0
    io_modes = {"default": 0, "r": 0, "w": 0, "a": 0}
    method_counts = {"lines": 0, "write": 0, "close": 0}
    write_argument_counts = {"zero": 0, "one": 0, "multiple": 0}
    list_files_count = 0
    for tokens in token_sets:
        values = [token.value for token in tokens]
        targets = _dofile_targets(tokens)
        for index, token in enumerate(tokens):
            if token.value == "dofile":
                dofile_count += 1
                if index not in targets:
                    raise AuditError("selected dofile site is not a statically resolved virtual path")
                open_index = index + 1
                if open_index >= len(tokens) or tokens[open_index].value != "(":
                    raise AuditError("selected dofile site does not use the one-argument call shape")
                if _call_argument_count(tokens, open_index) != 1:
                    raise AuditError("selected dofile site does not use exactly one argument")
            if values[index:index + 3] == ["io", ".", "open"]:
                open_index = index + 3
                argument_count = _call_argument_count(tokens, open_index)
                mode = "default"
                if argument_count >= 2:
                    depth = 0
                    mode_token = None
                    for candidate in range(open_index + 1, len(tokens)):
                        if tokens[candidate].value == "(":
                            depth += 1
                        elif tokens[candidate].value == ")":
                            if depth == 0:
                                break
                            depth -= 1
                        elif tokens[candidate].value == "," and depth == 0:
                            mode_token = tokens[candidate + 1]
                            break
                    if mode_token is None or mode_token.kind != "string":
                        raise AuditError("selected io.open mode is not a static string")
                    mode = mode_token.value
                if mode not in io_modes:
                    raise AuditError(f"selected io.open mode is outside the audited host surface: {mode!r}")
                io_modes[mode] += 1
            if token.value == ":" and index + 2 < len(tokens) and tokens[index + 2].value == "(":
                method = tokens[index + 1].value
                if method in method_counts:
                    method_counts[method] += 1
                    argument_count = _call_argument_count(tokens, index + 2)
                    if method in {"lines", "close"} and argument_count != 0:
                        raise AuditError(f"selected file handle {method} site is not zero-argument")
                    if method == "write":
                        category = "zero" if argument_count == 0 else "one" if argument_count == 1 else "multiple"
                        write_argument_counts[category] += 1
                elif method == "listFiles":
                    list_files_count += 1
                    if _call_argument_count(tokens, index + 2) != 0:
                        raise AuditError("selected listFiles site is not zero-argument")

    guard_sources = _render_io_guard_sources(loaded)
    configurations = _audited_guard_configurations(entry_sha, guard_sources)
    render_phases = bool(guard_sources)
    selected = {
        "schemaVersion": 1,
        "authority": "audited-upstream-selected-closure",
        "upstreamFacts": {
            "dofile": {
                "siteCount": dofile_count,
                "argumentShape": "one-virtual-path",
                "returnShape": "callee-return-values",
                "phases": ["configured-load"],
            },
            "ioOpen": {
                "siteCount": sum(io_modes.values()),
                "modes": [
                    {"mode": mode, "siteCount": io_modes[mode]}
                    for mode in ("default", "r", "w", "a")
                ],
                "phases": ["configured-load", "render-callback"] if render_phases else ["configured-load"],
            },
            "handleMethods": [
                {
                    "method": "lines",
                    "siteCount": method_counts["lines"],
                    "argumentShape": "zero",
                    "returnShape": "iterator",
                },
                {
                    "method": "write",
                    "siteCount": method_counts["write"],
                    "argumentSiteCounts": write_argument_counts,
                    "acceptedArgumentShape": "zero-or-more",
                    "returnShape": "same-handle",
                    "chainable": True,
                },
                {
                    "method": "close",
                    "siteCount": method_counts["close"],
                    "argumentShape": "zero",
                    "returnShape": "true-on-success",
                },
            ],
            "legacyDirectoryScan": {
                "siteCount": list_files_count,
                "method": "listFiles",
                "phases": ["configured-load"],
                "returnShape": "virtual-path-array-or-nil",
            },
            "phaseEvidence": [
                {
                    "phase": "configured-load",
                    "operationKinds": [
                        "filesystemRead",
                        "filesystemWrite",
                        "filesystemDirectoryScan",
                    ],
                },
                *([
                    {
                        "phase": "render-callback",
                        "operationKinds": ["filesystemRead", "filesystemWrite"],
                        "guardCount": len(guard_sources),
                    }
                ] if render_phases else []),
            ],
        },
        "auditedGuardConfigurations": configurations,
        "asoBMaShowPolicy": {
            "authority": "AsoBMaShow",
            "nestedWriteParentCreation": "safe-automatic-overlay-parents",
            "renderTransitionHandles": "invalidate-release-read-buffers-discard-unclosed-write-buffers",
            "dirtyHandleTransition": "validation-failure",
            "postTransitionOperationCriticality": "session-critical",
            "denyBeforeEffect": True,
            "performedAndDeniedCountersSeparate": True,
            "externalIdentitySerialization": "opaque-ids-and-digests-only",
            "serializationOrder": "deterministic",
        },
    }
    return selected, {"customTimers": 0, "customEvents": 0}


def read_constant_definitions(loaded: dict[str, str]):
    definitions: dict[tuple[str, str], int] = {}
    categories = set(PROPERTY_CATEGORIES) | {"TIMER", "BUTTON"}
    category_modules: dict[str, set[str]] = {category: set() for category in categories}
    for text in loaded.values():
        tokens = tokenize_lua(text)
        for index in range(len(tokens) - 3):
            if (
                tokens[index].value in categories
                and tokens[index + 1].value == "="
                and tokens[index + 2].value == "require"
            ):
                module_name = _call_string_argument(tokens, index + 2)
                if module_name is not None:
                    category_modules[tokens[index].value].add(
                        module_name.replace(".", "/") + ".lua"
                    )
    for category, paths in category_modules.items():
        for path in paths:
            text = loaded.get(path)
            if text is None:
                continue
            tokens = tokenize_lua(text)
            for index in range(len(tokens) - 2):
                if (
                    tokens[index].kind != "identifier"
                    or not re.fullmatch(r"[A-Z][A-Z0-9_]*", tokens[index].value)
                    or tokens[index + 1].value != "="
                ):
                    continue
                value_index = index + 2
                sign = 1
                if tokens[value_index].value == "-" and value_index + 1 < len(tokens):
                    sign = -1
                    value_index += 1
                if tokens[value_index].kind != "number" or not tokens[value_index].value.isdigit():
                    continue
                definitions[(category, tokens[index].value)] = sign * int(tokens[value_index].value)
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
    tokens = [token for text in loaded.values() for token in tokenize_lua(text)]
    surface = []
    for name in OBJECT_NAMES:
        if any(
            token.value == name
            and index + 1 < len(tokens)
            and tokens[index + 1].value == "="
            for index, token in enumerate(tokens)
        ):
            symbol = {
                "note": "SkinNote.prepare",
                "bga": "BGAProcessor.drawBGA",
                "destination": "JSONSkinLoader.setDestination",
            }.get(name, "JSONSkinLoader.loadJsonSkin")
            surface.append(
                surface_evidence(
                    "object",
                    name,
                    "critical" if name in {"note", "destination"} else "optional",
                    provenance_for(symbol),
                )
            )

    definitions = read_constant_definitions(loaded)
    property_origins: dict[tuple[str, str], dict[str, list[str]]] = {}
    timer_origins: dict[str, dict[str, list[str]]] = {}
    event_origins: dict[str, dict[str, list[str]]] = {}

    def add_origin(collection, key, origin: str, *symbols: str) -> None:
        origin_symbols = collection.setdefault(key, {}).setdefault(origin, [])
        for symbol in symbols:
            if symbol not in origin_symbols:
                origin_symbols.append(symbol)

    def evidence_for(origins: dict[str, list[str]]) -> list[dict]:
        symbols = []
        for origin in ("direct", "model"):
            for symbol in origins.get(origin, []):
                if symbol not in symbols:
                    symbols.append(symbol)
        return [provenance_for(symbol)[0] for symbol in symbols]

    model_property_provenance = {
        "boolean": "BooleanPropertyFactory.getBooleanProperty",
        "integer": "IntegerPropertyFactory.getIntegerProperty",
        "float": "FloatPropertyFactory.getRateProperty",
        "string": "StringPropertyFactory.getStringProperty",
        "offset": "SkinLuaAccessor.exportSkinProperty",
    }
    for index in range(len(tokens) - 4):
        if (
            tokens[index].value != "MAIN"
            or tokens[index + 1].value != "."
            or tokens[index + 2].value not in set(PROPERTY_CATEGORIES) | {"TIMER", "BUTTON"}
            or tokens[index + 3].value != "."
            or tokens[index + 4].kind != "identifier"
        ):
            continue
        category = tokens[index + 2].value
        name = tokens[index + 4].value
        value = definitions.get((category, name))
        stable_value = str(value) if value is not None else f"name:{name}"
        if category == "TIMER":
            add_origin(
                timer_origins,
                stable_value,
                "model",
                "TimerPropertyFactory.getTimerProperty",
            )
        elif category == "BUTTON":
            add_origin(
                event_origins,
                stable_value,
                "model",
                "EventFactory.getEvent",
            )
        else:
            property_type = PROPERTY_CATEGORIES[category]
            add_origin(
                property_origins,
                (property_type, stable_value),
                "model",
                model_property_provenance[property_type],
            )
    direct_calls = {
        "option": (
            "boolean",
            "MainStatePropertyLuaApiExporter.OptionFunction",
            "BooleanPropertyFactory.getBooleanProperty",
        ),
        "number": (
            "integer",
            "MainStatePropertyLuaApiExporter.NumberFunction",
            "IntegerPropertyFactory.getIntegerProperty",
        ),
        "float_number": (
            "float",
            "MainStatePropertyLuaApiExporter.FloatNumberFunction",
            "FloatPropertyFactory.getRateProperty",
        ),
        "text": (
            "string",
            "MainStatePropertyLuaApiExporter.TextFunction",
            "StringPropertyFactory.getStringProperty",
        ),
        "event_index": (
            "integer",
            "MainStatePropertyLuaApiExporter.EventIndexFunction",
            "IntegerPropertyFactory.getImageIndexProperty",
        ),
    }
    for index in range(len(tokens) - 4):
        if (
            tokens[index].value != "main_state"
            or tokens[index + 1].value != "."
            or tokens[index + 3].value != "("
        ):
            continue
        api = tokens[index + 2].value
        argument = index + 4
        sign = ""
        if tokens[argument].value == "-" and argument + 1 < len(tokens):
            sign = "-"
            argument += 1
        if tokens[argument].kind not in {"number", "string"}:
            continue
        raw_id = sign + tokens[argument].value
        if api in direct_calls:
            property_type, *symbols = direct_calls[api]
            add_origin(
                property_origins,
                (property_type, raw_id),
                "direct",
                *symbols,
            )
        elif api == "timer":
            add_origin(
                timer_origins,
                raw_id,
                "direct",
                "MainStatePropertyLuaApiExporter.TimerFunction",
            )
        elif api == "event_exec":
            add_origin(
                event_origins,
                raw_id,
                "direct",
                "MainStatePropertyLuaApiExporter.EventExecFunction",
            )

    for property_type, item_id in sorted(property_origins):
        surface.append(
            surface_evidence(
                "property",
                f"{property_type}:{item_id}",
                "critical",
                evidence_for(property_origins[(property_type, item_id)]),
            )
        )
    for item_id in sorted(timer_origins, key=lambda value: value.encode("utf-8")):
        surface.append(
            surface_evidence(
                "timer",
                item_id,
                "critical",
                evidence_for(timer_origins[item_id]),
            )
        )
    for item_id in sorted(event_origins, key=lambda value: value.encode("utf-8")):
        surface.append(
            surface_evidence(
                "event",
                item_id,
                "optional",
                evidence_for(event_origins[item_id]),
            )
        )

    for path, disposition in sorted(module_criticality.items(), key=lambda item: item[0].encode("utf-8")):
        surface.append(
            surface_evidence(
                "module",
                opaque_id("module", path),
                disposition,
                provenance_for("SkinLuaAccessor.setDirectory"),
            )
        )
    for module_name, disposition in sorted(host_modules.items()):
        module_id = "host-main-state" if module_name == "main_state" else opaque_id("module", "host:" + module_name)
        if module_name == "luajava":
            symbol = "LegacySkinLuaApi.install"
        elif module_name == "main_state":
            symbol = "SkinLuaAccessor.exportMainStateAccessor"
        else:
            symbol = "SkinLuaAccessor.setDirectory"
        surface.append(surface_evidence("module", module_id, disposition, provenance_for(symbol)))

    file_apis: dict[str, str] = {}
    for path, text in loaded.items():
        module_tokens = tokenize_lua(text)
        guarded = _lua_guarded_flags(module_tokens)
        parent_disposition = module_criticality[path]
        for index, token in enumerate(module_tokens):
            api = None
            if token.kind == "identifier" and token.value in {"require", "dofile", "loadfile"}:
                api = token.value
            elif (
                token.value in {"main_state", "skin_config", "io", "luajava"}
                and index + 2 < len(module_tokens)
                and module_tokens[index + 1].value == "."
                and module_tokens[index + 2].kind == "identifier"
            ):
                api = token.value + "." + module_tokens[index + 2].value
            if api is None:
                continue
            disposition = (
                "critical"
                if parent_disposition == "critical" and not guarded[index]
                else "optional"
            )
            previous = file_apis.get(api)
            if previous is None or (previous == "optional" and disposition == "critical"):
                file_apis[api] = disposition
    for api, disposition in sorted(file_apis.items()):
        if api.startswith("main_state."):
            provenance = provenance_for("MainStatePropertyLuaApiExporter.export")
        elif api.startswith("skin_config."):
            provenance = provenance_for("SkinLuaAccessor.exportSkinProperty")
        elif api.startswith("luajava."):
            provenance = provenance_for("LegacySkinLuaApi.install")
        elif api.startswith("io."):
            provenance = provenance_for("SkinLuaAccessor.RestrictedIoLib.openFile")
        elif api == "require":
            provenance = provenance_for("SkinLuaAccessor.setDirectory")
        else:
            provenance = provenance_for("SkinLuaAccessor.execFile")
        surface.append(surface_evidence("file-api", api, disposition, provenance))
    surface.sort(key=lambda item: (item["kind"], item["id"]))
    return surface


def pending_field(value=None) -> dict:
    return {"status": "pending", "value": value}


def acceptance_contract(
    archive_sha: str,
    tree_sha: str,
    entry_sha: str,
    selected_file_io_surface: dict,
) -> dict:
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
    configurations = selected_file_io_surface["auditedGuardConfigurations"]
    passing_vectors = [
        item["guardVectorSha256"] for item in configurations if item["role"] == "passing"
    ]
    negative_configurations = [item for item in configurations if item["role"] == "negative"]
    negative_scenarios = []
    if negative_configurations:
        negative = negative_configurations[0]
        negative_scenarios.append(
            {
                "id": RENDER_IO_NEGATIVE_SCENARIO_ID,
                "guardConfigurationId": negative["id"],
                "auditedGuardConfigurationSha256": negative["auditedGuardConfigurationSha256"],
                "expectedGuardVectorSha256": negative["guardVectorSha256"],
                "expectedDeniedOperation": "filesystemRead",
                "expectedDiagnostic": "skin_file_render_phase_denied",
                "expectedAction": "discard_frame_disable_session_same_frame_builtin",
                "criticality": "session-critical-sandbox-integrity",
                "performedCountersExpected": {
                    "filesystemReads": 0,
                    "filesystemWrites": 0,
                    "filesystemDirectoryScans": 0,
                    "resourceUploads": 0,
                },
                "deniedCountersExpected": {
                    "filesystemReads": "positive",
                    "filesystemWrites": 0,
                    "filesystemDirectoryScans": 0,
                    "resourceUploads": 0,
                },
                "overlayDigestCapture": "asynchronous-after-session-teardown",
                "overlayDigestBefore": "pending",
                "overlayDigestAfter": "pending",
            }
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
            "activatedRevisionSha256": pending_field(),
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
            {
                "aspect": layout["aspect"],
                "mode": layout["mode"],
                "timestampsMicros": [],
                "status": "pending",
                "evidenceReference": None,
            }
            for layout in layouts
        ],
        "timerEventTrace": {
            "status": "pending",
            "selectedIds": [],
            "observedOrder": [],
            "evidenceReference": None,
        },
        "passingGuardVectorSha256": passing_vectors,
        "negativeScenarios": negative_scenarios,
        "protocol": {"warmupSeconds": 30, "measurementSeconds": 180, "repetitions": 3},
        "layouts": layouts,
        "limits": {
            "p99SkinCpuFrameFraction": 0.9,
            "missedPresentationPercent": 0.5,
            "residentMemoryDriftMiB": 32,
            "activeRenderFilesystemReads": 0,
            "activeRenderFilesystemWrites": 0,
            "activeRenderFilesystemDirectoryScans": 0,
            "activeRenderResourceUploads": 0,
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
    legacy_surface = analyze_legacy_lua_api(loaded, module_criticality)
    payload_by_id = {record["id"]: record for record in archive_data["payloads"]}
    entry_payload_id = opaque_id("payload", entry)
    entry_sha = payload_by_id[entry_payload_id]["sha256"]
    selected_file_io_surface, selected_custom_object_maps = analyze_selected_file_io_surface(
        loaded,
        entry_sha,
    )
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
        "acceptanceContract": acceptance_contract(
            archive_data["archiveSha256"],
            disk_tree_sha,
            entry_sha,
            selected_file_io_surface,
        ),
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
        "legacyLuaApiSurface": legacy_surface,
        "selectedFileIoSurface": selected_file_io_surface,
        "selectedCustomObjectMaps": selected_custom_object_maps,
        "timerEventOrdering": {
            "phaseOrder": ["customTimers", "customEvents"],
            "withinPhase": "IntMap-backing-hash-iteration",
            "sortedById": False,
            "selectedIdTraceRequired": True,
        },
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
