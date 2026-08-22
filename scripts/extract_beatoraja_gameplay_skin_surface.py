#!/usr/bin/env python3
"""Extract the pinned Beatoraja gameplay-skin source surface.

The snapshot deliberately records source membership, not AsoBMaShow support.
The adjacent ledger is where the current implementation state is classified.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
SCHEMA_VERSION = 1
REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SOURCE_SURFACE_PATH = (
    REPOSITORY_ROOT / "docs/skin-compat/beatoraja-gameplay-source-surface-v1.json"
)
LEDGER_PATH = (
    REPOSITORY_ROOT / "docs/skin-compat/beatoraja-gameplay-feature-ledger-v1.json"
)
SKIN_ROOT = Path("src/bms/player/beatoraja/skin")


@dataclass(frozen=True)
class JavaClass:
    name: str
    body: str


def normalized_name(value: str) -> str:
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1-\2", value)
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def source_path(path: Path) -> str:
    return path.as_posix()


def read_source(root: Path, relative_path: Path) -> str:
    path = root / relative_path
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise RuntimeError(f"cannot read {path}: {error}") from error


def find_closing_brace(source: str, opening_brace: int) -> int:
    depth = 0
    for index in range(opening_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise RuntimeError("unclosed Java class body")


def json_classes(source: str) -> dict[str, JavaClass]:
    classes: dict[str, JavaClass] = {}
    for match in re.finditer(r"\b(?:public\s+)?static\s+class\s+(\w+)\s*\{", source):
        name = match.group(1)
        opening_brace = source.find("{", match.start(), match.end())
        closing_brace = find_closing_brace(source, opening_brace)
        classes[name] = JavaClass(name=name, body=source[opening_brace + 1 : closing_brace])
    return classes


def top_level_public_fields(java_class: JavaClass) -> list[tuple[str, str]]:
    fields: list[tuple[str, str]] = []
    depth = 0
    field_pattern = re.compile(
        r"^\s*public\s+(?!static\s+class\b)([\w<>?, \[\].]+?)\s+(\w+)\s*(?:=.*)?;\s*$"
    )
    for line in java_class.body.splitlines():
        if depth == 0:
            match = field_pattern.match(line)
            if match:
                fields.append((match.group(1).strip(), match.group(2)))
        depth += line.count("{") - line.count("}")
    return fields


def referenced_class_names(type_name: str, known_classes: set[str]) -> set[str]:
    return set(re.findall(r"\b[A-Z]\w*\b", type_name)) & known_classes


def reachable_json_classes(json_skin: str, *play_loaders: str) -> tuple[dict[str, JavaClass], set[str]]:
    classes = json_classes(json_skin)
    if "Skin" not in classes:
        raise RuntimeError("JsonSkin.Skin was not found")
    known = set(classes)
    # JsonSkin.Skin is the gameplay document root, so retain its fields even
    # when a field names a select/configuration-only child. Only classes named
    # by the actual gameplay object loaders can expand into child fields.
    reachable = {"Skin"}
    pending = []
    for loader in play_loaders:
        for class_name in re.findall(r"JsonSkin\.(\w+)", loader):
            if class_name in known and class_name not in reachable:
                reachable.add(class_name)
                pending.append(class_name)
    while pending:
        name = pending.pop()
        java_class = classes.get(name)
        if java_class is None:
            continue
        for type_name, _ in top_level_public_fields(java_class):
            for candidate in referenced_class_names(type_name, known):
                if candidate not in reachable:
                    reachable.add(candidate)
                    pending.append(candidate)
    return classes, reachable


def enum_constants(source: str, enum_name: str) -> list[str]:
    match = re.search(rf"\benum\s+{re.escape(enum_name)}\b[^{{]*\{{", source)
    if match is None:
        raise RuntimeError(f"enum {enum_name} was not found")
    opening_brace = source.find("{", match.start(), match.end())
    body = source[opening_brace + 1 : find_closing_brace(source, opening_brace)]
    constants = []
    depth = 0
    for line in body.splitlines():
        if depth == 0:
            constant = re.match(r"\s*([A-Z][A-Z0-9_]*)\s*(?:\(|,)", line)
            if constant:
                constants.append(constant.group(1))
        depth += line.count("{") - line.count("}")
    return constants


def add_feature(features: dict[str, dict], identifier: str, path: str, symbol: str) -> None:
    feature = {"id": identifier, "source": {"path": path, "symbol": symbol}}
    previous = features.get(identifier)
    if previous is not None and previous != feature:
        raise RuntimeError(f"ambiguous source surface ID {identifier}")
    features[identifier] = feature


def add_json_surface(features: dict[str, dict], root: Path) -> None:
    relative = SKIN_ROOT / "json/JsonSkin.java"
    json_skin = read_source(root, relative)
    loader_relative = SKIN_ROOT / "json/JsonPlaySkinObjectLoader.java"
    base_loader_relative = SKIN_ROOT / "json/JsonSkinObjectLoader.java"
    classes, reachable = reachable_json_classes(
        json_skin,
        read_source(root, loader_relative),
        read_source(root, base_loader_relative),
    )
    for class_name in sorted(reachable):
        for _, field_name in top_level_public_fields(classes[class_name]):
            add_feature(
                features,
                f"json.field.{normalized_name(class_name)}-{normalized_name(field_name)}",
                source_path(relative),
                f"JsonSkin.{class_name}.{field_name}",
            )


def add_lr2_surface(features: dict[str, dict], root: Path) -> None:
    sources = (
        (SKIN_ROOT / "lr2/LR2SkinCSVLoader.java", "CSVCommand", "csv-command"),
        (SKIN_ROOT / "lr2/LR2PlaySkinLoader.java", "PlayCommand", "play-command"),
        (SKIN_ROOT / "lr2/LR2SkinHeaderLoader.java", "HeaderCommand", "header-command"),
    )
    for relative, enum_name, kind in sources:
        source = read_source(root, relative)
        for command in enum_constants(source, enum_name):
            add_feature(
                features,
                f"lr2.{kind}.{normalized_name(command)}",
                source_path(relative),
                f"{enum_name}.{command}",
            )
        for command in re.findall(r'new\s+CommandWord\("([^"]+)"\)', source):
            add_feature(
                features,
                f"lr2.{kind}.{normalized_name(command)}",
                source_path(relative),
                f"{relative.stem}.CommandWord({command})",
            )


def add_lua_object_surface(features: dict[str, dict], root: Path) -> None:
    json_relative = SKIN_ROOT / "json/JsonSkin.java"
    lua_relative = SKIN_ROOT / "lua/LuaSkinLoader.java"
    classes, reachable = reachable_json_classes(
        read_source(root, json_relative),
        read_source(root, SKIN_ROOT / "json/JsonPlaySkinObjectLoader.java"),
        read_source(root, SKIN_ROOT / "json/JsonSkinObjectLoader.java"),
    )
    for class_name in sorted(reachable):
        for _, field_name in top_level_public_fields(classes[class_name]):
            add_feature(
                features,
                f"lua.object-field.{normalized_name(class_name)}-{normalized_name(field_name)}",
                source_path(lua_relative),
                f"LuaSkinLoader.fromLuaValue(JsonSkin.{class_name}.{field_name})",
            )


def add_lua_exports(features: dict[str, dict], root: Path) -> None:
    names = (
        "SkinLuaAccessor.java",
        "SkinFileLuaApiExporter.java",
        "SkinHttpLuaApiExporter.java",
        "SkinAudioLuaApiExporter.java",
        "LegacySkinLuaApi.java",
    )
    for name in names:
        relative = SKIN_ROOT / "lua" / name
        source = read_source(root, relative)
        class_name = relative.stem
        # The legacy facade builds several nested LuaTable instances. Include
        # every named table assignment, not only its top-level facade, so the
        # finite graphics/input/controller members are source-surface entries.
        export_targets = {"table", "facade", "luajava", "debug"}
        export_targets.update(re.findall(r"\bLuaTable\s+(\w+)\s*=", source))
        target_pattern = "|".join(sorted(map(re.escape, export_targets)))
        exports = re.findall(rf'\b(?:{target_pattern})\.set\("([^"]+)"', source)
        if class_name == "SkinLuaAccessor":
            exports.extend(re.findall(r'\bglobals\.set\("(skin_config)"', source))
        for export in exports:
            add_feature(
                features,
                f"lua.export.{normalized_name(export)}",
                source_path(relative),
                f"{class_name}.export({export})",
            )

    loader_relative = SKIN_ROOT / "lua/LuaSkinLoader.java"
    loader_source = read_source(root, loader_relative)
    for binding_type in re.findall(r"put\((\w+)\.class", loader_source):
        kind = {
            "BooleanProperty": "property",
            "IntegerProperty": "property",
            "FloatProperty": "property",
            "StringProperty": "property",
            "TimerProperty": "timer",
            "FloatWriter": "writer",
            "StringWriter": "writer",
            "Event": "event",
        }.get(binding_type)
        if kind is None:
            continue
        add_feature(
            features,
            f"lua.{kind}.{normalized_name(binding_type)}",
            source_path(loader_relative),
            f"LuaSkinLoader.serializerMap({binding_type})",
        )

    add_feature(
        features,
        "lua.offset.destination-offset",
        source_path(loader_relative),
        "LuaSkinLoader.fromLuaValue(JsonSkin.Destination.offset)",
    )


def extract(root: Path) -> dict:
    features: dict[str, dict] = {}
    add_json_surface(features, root)
    add_lr2_surface(features, root)
    add_lua_object_surface(features, root)
    add_lua_exports(features, root)
    return {
        "schemaVersion": SCHEMA_VERSION,
        "pinnedCommit": PINNED_COMMIT,
        "features": [features[identifier] for identifier in sorted(features)],
    }


PLAN_FORMATS = "docs/superpowers/plans/2026-08-21-beatoraja-gameplay-skin-contract-and-formats.md"
PLAN_VISUALIZERS = "docs/superpowers/plans/2026-08-21-beatoraja-gameplay-skin-visualizers.md"
PLAN_ASSETS = "docs/superpowers/plans/2026-08-21-beatoraja-gameplay-skin-assets-and-host.md"

TIMING_DISTRIBUTION_NOOP_SOURCE = {
    "path": "src/bms/player/beatoraja/skin/SkinTimingDistributionGraph.java",
    "symbol": "SkinTimingDistributionGraph.prepare",
}

IMPLEMENTED_LUA_OBJECT_PREFIXES = (
    "lua.object-field.animation-",
    "lua.object-field.bga-",
    "lua.object-field.destination-",
    "lua.object-field.float-value-",
    "lua.object-field.gauge-",
    "lua.object-field.graph-",
    "lua.object-field.hidden-cover-",
    "lua.object-field.image-",
    "lua.object-field.image-set-",
    "lua.object-field.judge-",
    "lua.object-field.lift-cover-",
    "lua.object-field.note-set-",
    "lua.object-field.rect-",
    "lua.object-field.slider-",
    "lua.object-field.text-",
    "lua.object-field.value-",
)

IMPLEMENTED_LUA_IDS = {
    "lua.event.event",
    "lua.export.enabled-options",
    "lua.export.file-path",
    "lua.export.get-path",
    "lua.export.offset",
    "lua.export.option",
    "lua.export.skin-config",
    "lua.offset.destination-offset",
    "lua.property.boolean-property",
    "lua.property.float-property",
    "lua.property.integer-property",
    "lua.property.string-property",
    "lua.timer.timer-property",
    "lua.writer.float-writer",
    "lua.writer.string-writer",
}


def missing(identifier: str, plan: str, task: str) -> dict:
    return {"id": identifier, "status": "missing", "plan": plan, "task": task}


def implemented(identifier: str) -> dict:
    return {
        "id": identifier,
        "status": "implemented",
        "implementation": "src/skin/beatoraja/LuaSkinTableDecoder.cpp",
        "tests": "tests/beatoraja_skin_model_tests.cpp",
    }


def implemented_at(identifier: str, implementation: str, tests: str) -> dict:
    return {
        "id": identifier,
        "status": "implemented",
        "implementation": implementation,
        "tests": tests,
    }


def source_defined_noop(identifier: str, source: dict) -> dict:
    return {"id": identifier, "status": "source-defined-noop", "source": source}


def legacy_export(identifier: str) -> dict:
    if identifier.startswith("lua.export.file-") or identifier in {
        "lua.export.mkdir",
        "lua.export.list-files",
    }:
        return implemented_at(
            identifier,
            "src/skin/beatoraja/LuaSkinHostModules.cpp; src/skin/beatoraja/LuaSkinFileSystem.cpp",
            "tests/lua_skin_file_system_tests.cpp; tests/lua_skin_host_modules_tests.cpp",
        )
    if identifier.startswith("lua.export.http-") or identifier in {
        "lua.export.bind-class",
        "lua.export.new",
        "lua.export.new-instance",
        "lua.export.open-connection",
        "lua.export.set-request-method",
        "lua.export.set-connect-timeout",
        "lua.export.connect",
        "lua.export.get-response-code",
        "lua.export.get-input-stream",
        "lua.export.read-line",
    }:
        return implemented_at(
            identifier,
            "src/skin/beatoraja/LuaSkinHostModules.cpp; src/skin/beatoraja/LuaSkinHttpClient.cpp; src/skin/beatoraja/LuaSkinCurlHttpTransport.cpp; src/skin/beatoraja/LuaSkinFoundationHttpTransport.mm",
            "tests/lua_skin_host_modules_tests.cpp; tests/lua_skin_http_transport_tests.cpp",
        )
    if identifier.startswith("lua.export.audio-"):
        return implemented_at(
            identifier,
            "src/skin/beatoraja/LuaSkinAudioHost.cpp; src/skin/beatoraja/LuaSkinApplicationAudioBackend.cpp; src/audio/AudioWrapper.cpp; src/audio/AudioMix.cpp",
            "tests/lua_skin_host_modules_tests.cpp; tests/audio_mix_tests.cpp; tests/audio_wrapper_lifecycle_tests.cpp; tests/play_skin_session_tests.cpp",
        )
    return implemented_at(
        identifier,
        "src/skin/beatoraja/LuaSkinHostModules.cpp; src/skin/beatoraja/LuaSkinLegacyInputHost.cpp; src/input/InputDeviceRegistry.cpp; src/input/SDLInputBackend.cpp",
        "tests/lua_skin_host_modules_tests.cpp; tests/input_device_registry_tests.cpp; tests/play_skin_session_tests.cpp; tests/gameplay_skin_session_factory_tests.cpp",
    )


def ledger_feature(feature: dict) -> dict:
    identifier = feature["id"]
    if identifier.startswith("json.field."):
        return missing(identifier, PLAN_FORMATS, "Task 4: JSON gameplay document decoder")
    if identifier.startswith("lr2.csv-command.") or identifier.startswith("lr2.header-command."):
        return missing(identifier, PLAN_FORMATS, "Task 5: LR2 syntax, encoding, header, and include engine")
    if identifier.startswith("lr2.play-command."):
        return missing(identifier, PLAN_FORMATS, "Task 6: LR2 gameplay commands to canonical model")
    if identifier in IMPLEMENTED_LUA_IDS:
        return implemented(identifier)
    if identifier.startswith("lua.export."):
        return legacy_export(identifier)
    if "font" in identifier:
        return missing(identifier, PLAN_ASSETS, "Task 1: Bitmap .fnt and LR2FONT resources")
    if "pmchara" in identifier:
        return missing(identifier, PLAN_ASSETS, "Task 3: Complete Pomyu/PM-character rendering")
    if "practice" in identifier:
        return missing(identifier, PLAN_ASSETS, "Task 4: SkinPractice object")
    if identifier == "lua.object-field.skin-source":
        return missing(identifier, PLAN_ASSETS, "Task 2: Skin source movies")
    if identifier == "lua.object-field.text-event":
        return missing(identifier, PLAN_ASSETS, "Task 5: Editable skin text")
    if "timing-visualizer" in identifier or "timingvisualizer" in identifier:
        return missing(identifier, PLAN_VISUALIZERS, "Task 3: Timing visualizer")
    if "hit-error" in identifier or "hiterrorvisualizer" in identifier:
        return missing(identifier, PLAN_VISUALIZERS, "Task 4: Hit-error visualizer")
    if "judge-graph" in identifier or "judgegraph" in identifier or "note-distribution" in identifier:
        return missing(identifier, PLAN_VISUALIZERS, "Task 2: Judgement and note-distribution graph")
    if "bpm-graph" in identifier or "bpmgraph" in identifier:
        return missing(identifier, PLAN_VISUALIZERS, "Task 6: BPM/scroll/stop graph")
    if "gauge-graph" in identifier or "gaugegraph" in identifier:
        return missing(identifier, PLAN_VISUALIZERS, "Task 7: Gauge-history graph")
    if "timing-distribution" in identifier or "timingdistributiongraph" in identifier:
        return source_defined_noop(identifier, TIMING_DISTRIBUTION_NOOP_SOURCE)
    if identifier in {
        "lua.object-field.skin-songlist",
        "lua.object-field.skin-skin-select",
    }:
        return source_defined_noop(identifier, feature["source"])
    if identifier.startswith("lua.object-field.skin-"):
        root_fields = {
            "type", "name", "author", "w", "h", "fadeout", "input", "scene",
            "close", "loadend", "playstart", "judgetimer", "finishmargin", "category",
            "property", "filepath", "offset", "image", "imageset", "value", "floatvalue",
            "text", "slider", "graph", "gauge", "hidden-cover", "lift-cover", "bga",
            "skinpreview", "judge", "note", "custom-events", "custom-timers", "destination",
        }
        field_name = identifier.removeprefix("lua.object-field.skin-")
        if field_name in root_fields:
            return implemented(identifier)
    if identifier.startswith(IMPLEMENTED_LUA_OBJECT_PREFIXES):
        return implemented(identifier)
    raise RuntimeError(f"no evidence-based ledger classification for {identifier}")


def make_ledger(surface: dict) -> dict:
    return {
        "schemaVersion": SCHEMA_VERSION,
        "pinnedCommit": PINNED_COMMIT,
        "features": [ledger_feature(feature) for feature in surface["features"]],
    }


def json_text(value: dict) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def verify_commit(root: Path) -> None:
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"cannot read Beatoraja commit: {completed.stderr.strip()}")
    actual = completed.stdout.strip()
    if actual != PINNED_COMMIT:
        raise RuntimeError(f"expected pinned commit {PINNED_COMMIT}, got {actual}")


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise RuntimeError(f"cannot read committed contract {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise RuntimeError(f"invalid JSON in {path}: {error}") from error


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--beatoraja-root", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--write", action="store_true", help="refresh both committed JSON contracts")
    arguments = parser.parse_args()
    if arguments.check and arguments.write:
        parser.error("--check and --write cannot be used together")

    root = arguments.beatoraja_root.resolve()
    try:
        verify_commit(root)
        surface = extract(root)
        ledger = make_ledger(surface)
        if arguments.write:
            SOURCE_SURFACE_PATH.write_text(json_text(surface), encoding="utf-8")
            LEDGER_PATH.write_text(json_text(ledger), encoding="utf-8")
        elif arguments.check:
            if load_json(SOURCE_SURFACE_PATH) != surface:
                raise RuntimeError("committed source-surface snapshot differs from pinned source")
        else:
            sys.stdout.write(json_text(surface))
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
