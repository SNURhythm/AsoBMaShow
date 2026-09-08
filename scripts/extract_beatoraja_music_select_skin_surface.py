#!/usr/bin/env python3
"""Extract the pinned Beatoraja Lua music-select skin source surface."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

try:
    from scripts.extract_beatoraja_gameplay_skin_surface import (
        add_feature,
        add_lua_exports,
        find_closing_brace,
        normalized_name,
        read_source,
        reachable_json_classes,
        source_path,
        top_level_public_fields,
    )
except ModuleNotFoundError:
    from extract_beatoraja_gameplay_skin_surface import (
        add_feature,
        add_lua_exports,
        find_closing_brace,
        normalized_name,
        read_source,
        reachable_json_classes,
        source_path,
        top_level_public_fields,
    )


PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
SCHEMA_VERSION = 1
REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SOURCE_SURFACE_PATH = (
    REPOSITORY_ROOT
    / "docs/skin-compat/beatoraja-music-select-source-surface-v1.json"
)
LEDGER_PATH = (
    REPOSITORY_ROOT
    / "docs/skin-compat/beatoraja-music-select-feature-ledger-v1.json"
)
SKIN_ROOT = Path("src/bms/player/beatoraja/skin")
SELECT_ROOT = Path("src/bms/player/beatoraja/select")
CORE_PLAN = "docs/superpowers/plans/2026-09-01-beatoraja-type5-compatibility-core.md"
RUNTIME_PLAN = "docs/superpowers/plans/2026-09-01-beatoraja-music-select-runtime.md"

SELECT_SOURCES = (
    "MusicSelector.java",
    "MusicSelectSkin.java",
    "SkinBar.java",
    "SkinDistributionGraph.java",
    "BarRenderer.java",
    "MusicSelectInputProcessor.java",
    "MusicSelectKeyProperty.java",
)
PROPERTY_FACTORIES = (
    ("BooleanPropertyFactory.java", "boolean"),
    ("IntegerPropertyFactory.java", "integer"),
    ("FloatPropertyFactory.java", "float"),
    ("StringPropertyFactory.java", "string"),
    ("TimerPropertyFactory.java", "timer"),
    ("EventFactory.java", "event"),
    ("FloatWriter.java", "writer"),
)


def _split_top_level(value: str, delimiter: str) -> list[str]:
    """Split Java syntax while ignoring nested delimiters and quoted text."""
    result: list[str] = []
    start = 0
    braces = parentheses = brackets = 0
    quote = ""
    escaped = False
    index = 0
    while index < len(value):
        char = value[index]
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = ""
            index += 1
            continue
        if value.startswith("//", index):
            newline = value.find("\n", index + 2)
            index = len(value) if newline < 0 else newline + 1
            continue
        if value.startswith("/*", index):
            closing = value.find("*/", index + 2)
            index = len(value) if closing < 0 else closing + 2
            continue
        if char in {'"', "'"}:
            quote = char
        elif char == "{":
            braces += 1
        elif char == "}":
            braces -= 1
        elif char == "(":
            parentheses += 1
        elif char == ")":
            parentheses -= 1
        elif char == "[":
            brackets += 1
        elif char == "]":
            brackets -= 1
        elif (
            char == delimiter
            and braces == 0
            and parentheses == 0
            and brackets == 0
        ):
            result.append(value[start:index])
            start = index + 1
        index += 1
    result.append(value[start:])
    return result


def _enum_constant_segments(source: str) -> list[tuple[str, str, str]]:
    result: list[tuple[str, str, str]] = []
    for match in re.finditer(r"\benum\s+(\w+)\b[^\{]*\{", source):
        enum_name = match.group(1)
        opening = source.find("{", match.start(), match.end())
        body = source[opening + 1 : find_closing_brace(source, opening)]
        constant_region = _split_top_level(body, ";")[0]
        for segment in _split_top_level(constant_region, ","):
            without_comments = re.sub(r"/\*.*?\*/|//[^\n]*", " ", segment,
                                      flags=re.DOTALL)
            constant = re.match(r"\s*([A-Za-z_]\w*)\s*(?:\(|\{|$)",
                                without_comments)
            if constant:
                result.append((enum_name, constant.group(1), segment))
    return result


def _method_bodies(source: str) -> list[tuple[str, str]]:
    pattern = re.compile(
        r"\b(?:public|protected|private)\s+"
        r"(?:(?:static|final|synchronized|abstract|default)\s+)*"
        r"[\w<>?,.\[\] ]+\s+(\w+)\s*\([^;{}]*\)\s*\{"
    )
    result: list[tuple[str, str]] = []
    for match in pattern.finditer(source):
        opening = source.find("{", match.start(), match.end())
        result.append(
            (match.group(1), source[opening + 1 : find_closing_brace(source, opening)])
        )
    return result


def _public_methods(source: str) -> list[tuple[str, int]]:
    pattern = re.compile(
        r"\bpublic\s+(?:(?:static|final|synchronized|abstract|default)\s+)*"
        r"[\w<>?,.\[\] ]+\s+(\w+)\s*\(([^;{}]*)\)\s*(?:\{|;)"
    )
    methods: list[tuple[str, int]] = []
    for match in pattern.finditer(source):
        parameters = match.group(2).strip()
        arity = 0 if not parameters else len(_split_top_level(parameters, ","))
        methods.append((match.group(1), arity))
    return methods


def _public_static_final_fields(source: str) -> list[str]:
    return re.findall(
        r"^\s*public\s+static\s+final\s+[\w<>?,.\[\] ]+\s+(\w+)\s*=",
        source,
        flags=re.MULTILINE,
    )


def _add_select_json_and_lua_objects(features: dict[str, dict], root: Path) -> None:
    json_relative = SKIN_ROOT / "json/JsonSkin.java"
    select_loader = SKIN_ROOT / "json/JsonSelectSkinObjectLoader.java"
    base_loader = SKIN_ROOT / "json/JsonSkinObjectLoader.java"
    select_loader_source = read_source(root, select_loader)
    if not re.search(r"\bsk\.songlist\b", select_loader_source):
        raise RuntimeError("JsonSelectSkinObjectLoader does not reach Skin.songlist")
    classes, reachable = reachable_json_classes(
        read_source(root, json_relative),
        select_loader_source + "\nJsonSkin.SongList",
        read_source(root, base_loader),
    )
    for class_name in sorted(reachable):
        for _, field_name in top_level_public_fields(classes[class_name]):
            suffix = f"{normalized_name(class_name)}-{normalized_name(field_name)}"
            add_feature(
                features,
                f"json.field.{suffix}",
                source_path(json_relative),
                f"JsonSkin.{class_name}.{field_name}",
            )
            add_feature(
                features,
                f"lua.object-field.{suffix}",
                source_path(SKIN_ROOT / "lua/LuaSkinLoader.java"),
                f"LuaSkinLoader.fromLuaValue(JsonSkin.{class_name}.{field_name})",
            )

    for method, arity in _public_methods(select_loader_source):
        add_feature(
            features,
            f"select.loader.json-select.{normalized_name(method)}-{arity}",
            source_path(select_loader),
            f"JsonSelectSkinObjectLoader.{method}/{arity}",
        )


def _add_select_runtime(features: dict[str, dict], root: Path) -> None:
    for filename in SELECT_SOURCES:
        relative = SELECT_ROOT / filename
        source = read_source(root, relative)
        class_name = relative.stem
        for method, arity in _public_methods(source):
            add_feature(
                features,
                f"select.behavior.{normalized_name(class_name)}."
                f"{normalized_name(method)}-{arity}",
                source_path(relative),
                f"{class_name}.{method}/{arity}",
            )
        for field in _public_static_final_fields(source):
            prefix = "select.skin-bar" if class_name == "SkinBar" else (
                f"select.constant.{normalized_name(class_name)}"
            )
            add_feature(
                features,
                f"{prefix}.{normalized_name(field)}",
                source_path(relative),
                f"{class_name}.{field}",
            )
        for enum_name, constant, _ in _enum_constant_segments(source):
            add_feature(
                features,
                f"select.input.{normalized_name(enum_name)}."
                f"{normalized_name(constant)}",
                source_path(relative),
                f"{enum_name}.{constant}",
            )

    input_relative = SELECT_ROOT / "MusicSelectInputProcessor.java"
    add_feature(
        features,
        "select.input.music-select-input-processor",
        source_path(input_relative),
        "MusicSelectInputProcessor.input",
    )

    bar_root = SELECT_ROOT / "bar"
    for path in sorted((root / bar_root).glob("*.java")):
        relative = bar_root / path.name
        source = read_source(root, relative)
        class_name = path.stem
        add_feature(
            features,
            f"select.bar-class.{normalized_name(class_name)}",
            source_path(relative),
            class_name,
        )
        for method, arity in _public_methods(source):
            add_feature(
                features,
                f"select.bar-behavior.{normalized_name(class_name)}."
                f"{normalized_name(method)}-{arity}",
                source_path(relative),
                f"{class_name}.{method}/{arity}",
            )


def _add_property_factory_surface(features: dict[str, dict], root: Path) -> None:
    property_root = SKIN_ROOT / "property"
    for filename, category in PROPERTY_FACTORIES:
        relative = property_root / filename
        source = read_source(root, relative)
        class_name = relative.stem
        branches: dict[str, str] = {}
        for enum_name, constant, segment in _enum_constant_segments(source):
            if "instanceof MusicSelector" not in segment:
                continue
            branch = normalized_name(constant)
            branches[f"enum-{normalized_name(enum_name)}-{branch}"] = (
                f"{enum_name}.{constant}"
            )
            if category == "float" and re.search(r"\(\s*state\s*,\s*value\s*\)", segment):
                add_feature(
                    features,
                    f"select.property.writer.{branch}",
                    source_path(relative),
                    f"{enum_name}.{constant}.writer",
                )
        for method, body in _method_bodies(source):
            if "instanceof MusicSelector" in body:
                branches[f"method-{normalized_name(method)}"] = f"{class_name}.{method}"
        for branch, symbol in sorted(branches.items()):
            add_feature(
                features,
                f"select.property.{category}.{branch}",
                source_path(relative),
                symbol,
            )
        if branches:
            add_feature(
                features,
                f"select.property.{category}.music-selector",
                source_path(relative),
                f"{class_name}(instanceof MusicSelector)",
            )

        if category == "timer" and "state.timer" in source:
            add_feature(
                features,
                "select.property.timer.main-state-timer",
                source_path(relative),
                "TimerPropertyFactory.getTimerProperty",
            )
        if category == "writer" and "void set(MainState state, float value)" in source:
            add_feature(
                features,
                "select.property.writer.float-writer-contract",
                source_path(relative),
                "FloatWriter.set",
            )


def _add_loader_surface(features: dict[str, dict], root: Path) -> None:
    lua_relative = SKIN_ROOT / "lua/LuaSkinLoader.java"
    lua_source = read_source(root, lua_relative)
    for method, arity in _public_methods(lua_source):
        add_feature(
            features,
            f"select.lua-loader.{normalized_name(method)}-{arity}",
            source_path(lua_relative),
            f"LuaSkinLoader.{method}/{arity}",
        )
    json_loader_relative = SKIN_ROOT / "json/JSONSkinLoader.java"
    json_loader = read_source(root, json_loader_relative)
    if not re.search(
        r"case\s+MUSIC_SELECT\s*->\s*new\s+JsonSelectSkinObjectLoader\b",
        json_loader,
    ):
        raise RuntimeError("JSONSkinLoader has no MUSIC_SELECT dispatch")
    add_feature(
        features,
        "select.loader.music-select-dispatch",
        source_path(json_loader_relative),
        "JSONSkinLoader.case MUSIC_SELECT",
    )


def _add_target_surface(features: dict[str, dict], root: Path) -> None:
    relative = SKIN_ROOT / "SkinType.java"
    source = read_source(root, relative)
    if not re.search(r"\bMUSIC_SELECT\s*\(\s*5\s*,", source):
        raise RuntimeError("SkinType.MUSIC_SELECT is not numeric type 5")
    add_feature(
        features,
        "select.target.music-select",
        source_path(relative),
        "SkinType.MUSIC_SELECT(5)",
    )


def extract(root: Path) -> dict:
    features: dict[str, dict] = {}
    _add_select_json_and_lua_objects(features, root)
    add_lua_exports(features, root)
    _add_target_surface(features, root)
    _add_loader_surface(features, root)
    _add_select_runtime(features, root)
    _add_property_factory_surface(features, root)
    return {
        "schemaVersion": SCHEMA_VERSION,
        "pinnedCommit": PINNED_COMMIT,
        "features": [features[identifier] for identifier in sorted(features)],
    }


def _owning_task(identifier: str) -> tuple[str, str]:
    if identifier.startswith(("json.field.", "lua.object-field.", "select.loader.",
                              "select.lua-loader.")):
        return CORE_PLAN, "Task 3: Exact type-5 Lua table decoder"
    if identifier.startswith(("select.skin-bar.", "select.behavior.skin-bar.",
                              "select.behavior.skin-distribution-graph.",
                              "select.loader.json-select.")):
        return CORE_PLAN, "Task 4: SkinBar canonical expansion"
    if identifier.startswith(("select.property.",)):
        return RUNTIME_PLAN, "Task 4: Complete property, timer, and writer projection"
    if identifier.startswith(("select.input.",)):
        return RUNTIME_PLAN, "Task 3: Music-select input processor"
    if identifier.startswith(("select.bar-class.", "select.bar-behavior.",
                              "select.behavior.bar-renderer.")):
        return RUNTIME_PLAN, "Task 2: Bar manager and renderer model"
    if identifier.startswith("select.behavior.music-selector."):
        return RUNTIME_PLAN, "Task 5: Complete source event and command controller"
    return CORE_PLAN, "Task 5: Music-select state bridge and session"


def make_ledger(surface: dict) -> dict:
    rows = []
    for feature in surface["features"]:
        if feature["id"] == "select.target.music-select":
            rows.append(
                {
                    "id": feature["id"],
                    "status": "implemented",
                    "implementation": "src/skin/SkinTargetTraits.h",
                    "tests": "tests/gameplay_skin_traits_tests.cpp",
                    "assertion": {"runner": "gameplay_skin_traits_tests"},
                }
            )
            continue
        plan, task = _owning_task(feature["id"])
        rows.append(
            {
                "id": feature["id"],
                "status": "missing",
                "plan": plan,
                "task": task,
            }
        )
    return {
        "schemaVersion": SCHEMA_VERSION,
        "pinnedCommit": PINNED_COMMIT,
        "features": rows,
    }


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


def _json_text(value: dict) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2) + "\n"


def _load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read committed contract {path}: {error}") from error


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--beatoraja-root", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--write", action="store_true")
    arguments = parser.parse_args()
    if arguments.check and arguments.write:
        parser.error("--check and --write cannot be used together")
    root = arguments.beatoraja_root.resolve()
    try:
        verify_commit(root)
        surface = extract(root)
        if arguments.write:
            SOURCE_SURFACE_PATH.write_text(_json_text(surface), encoding="utf-8")
            LEDGER_PATH.write_text(_json_text(make_ledger(surface)), encoding="utf-8")
        elif arguments.check:
            if _load_json(SOURCE_SURFACE_PATH) != surface:
                raise RuntimeError("committed source surface differs from pinned source")
        else:
            sys.stdout.write(_json_text(surface))
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
