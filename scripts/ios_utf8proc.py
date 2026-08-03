#!/usr/bin/env python3
"""Prepare and verify the generated iOS utf8proc dependency.

The Xcode project intentionally references a generated XCFramework and header.
This helper makes that contract deterministic for clean checkouts, validates
both platform slices before publishing them, and keeps only one keyed cache
entry.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


CACHE_SCHEMA = 1
DEPLOYMENT_TARGET = "14.0"
LICENSE_SHA256 = "3b510150d34f248a221bb88e1d811238d6c6c18b51231822c42974c39bb07256"
TRIPLETS = ("arm64-ios", "arm64-ios-simulator")
LIBRARY_IDENTIFIERS = {
    "ios-arm64": {"platform": "IOS", "variant": None},
    "ios-arm64-simulator": {"platform": "IOSSIMULATOR", "variant": "simulator"},
}


class ArtifactError(RuntimeError):
    pass


def run(command: list[str], *, cwd: Path | None = None) -> str:
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            check=True,
            text=True,
            capture_output=True,
        )
    except subprocess.CalledProcessError as error:
        details = (error.stderr or error.stdout or "").strip()
        raise ArtifactError(
            f"command failed ({' '.join(command)}): {details}"
        ) from error
    return result.stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def version_tuple(version: str) -> tuple[int, ...]:
    try:
        return tuple(int(component) for component in version.split("."))
    except ValueError as error:
        raise ArtifactError(f"invalid Mach-O minimum OS version: {version}") from error


def verify_license(path: Path) -> None:
    if not path.is_file():
        raise ArtifactError(f"utf8proc license is missing: {path}")
    if sha256(path) != LICENSE_SHA256:
        raise ArtifactError("utf8proc license digest does not match the reviewed text")
    text = path.read_text(encoding="utf-8")
    required = (
        "Original utf8proc license",
        "Unicode data license",
        "Permission is hereby granted",
        "THE DATA FILES AND SOFTWARE ARE PROVIDED",
    )
    for phrase in required:
        if phrase not in text:
            raise ArtifactError(f"utf8proc license is incomplete: missing {phrase!r}")


def verify_triplets(repository_root: Path) -> None:
    for triplet in TRIPLETS:
        path = repository_root / "vcpkg-triplets" / f"{triplet}.cmake"
        if not path.is_file():
            raise ArtifactError(f"utf8proc triplet is missing: {path}")
        declaration = re.findall(
            r"set\(VCPKG_OSX_DEPLOYMENT_TARGET\s+([^\s\)]+)\)",
            path.read_text(encoding="utf-8"),
        )
        if declaration != [DEPLOYMENT_TARGET]:
            raise ArtifactError(
                f"{triplet} must set VCPKG_OSX_DEPLOYMENT_TARGET "
                f"to {DEPLOYMENT_TARGET}"
            )


def artifact_files(framework: Path, header: Path) -> dict[str, Path]:
    files = {"include/utf8proc.h": header}
    if framework.is_dir():
        for path in sorted(framework.rglob("*")):
            if path.is_file():
                files[f"lib/libutf8proc.xcframework/{path.relative_to(framework)}"] = path
    return files


def verify_manifest(manifest_path: Path, framework: Path, header: Path) -> None:
    if not manifest_path.is_file():
        raise ArtifactError(f"utf8proc cache manifest is missing: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as error:
        raise ArtifactError("utf8proc cache manifest is invalid") from error
    if manifest.get("schemaVersion") != CACHE_SCHEMA:
        raise ArtifactError("utf8proc cache manifest schema is unsupported")
    expected = manifest.get("files")
    if not isinstance(expected, dict) or not expected:
        raise ArtifactError("utf8proc cache manifest has no file digests")
    actual_files = artifact_files(framework, header)
    if set(actual_files) != set(expected):
        missing = sorted(set(expected) - set(actual_files))
        extra = sorted(set(actual_files) - set(expected))
        raise ArtifactError(
            f"utf8proc artifact file set differs from manifest; "
            f"missing={missing}, extra={extra}"
        )
    for relative, path in actual_files.items():
        if sha256(path) != expected[relative]:
            raise ArtifactError(f"utf8proc artifact digest mismatch: {relative}")


def archive_build_versions(archive: Path) -> list[tuple[str, str]]:
    with tempfile.TemporaryDirectory(prefix="asobmashow-utf8proc-inspect-") as temp:
        extracted = Path(temp)
        run(["ar", "-x", str(archive)], cwd=extracted)
        objects = sorted(
            path
            for path in extracted.iterdir()
            if path.is_file() and not path.name.startswith("__.SYMDEF")
        )
        if not objects:
            raise ArtifactError(f"utf8proc archive contains no objects: {archive}")
        versions: list[tuple[str, str]] = []
        for object_path in objects:
            details = run(["xcrun", "vtool", "-show-build", str(object_path)])
            platform = re.search(r"^\s*platform (\S+)$", details, re.MULTILINE)
            minimum = re.search(r"^\s*minos (\S+)$", details, re.MULTILINE)
            if platform is None or minimum is None:
                raise ArtifactError(
                    f"utf8proc object has no LC_BUILD_VERSION: {object_path.name}"
                )
            versions.append((platform.group(1), minimum.group(1)))
        return versions


def compile_header(header: Path, sdk: str, target: str) -> None:
    sdk_path = run(["xcrun", "--sdk", sdk, "--show-sdk-path"])
    run(
        [
            "xcrun",
            "--sdk",
            sdk,
            "clang++",
            "-target",
            target,
            "-isysroot",
            sdk_path,
            "-std=c++20",
            "-fsyntax-only",
            "-x",
            "c++",
            "-I",
            str(header.parent),
            "-include",
            header.name,
            "/dev/null",
        ]
    )


def verify_artifacts(
    framework: Path,
    header: Path,
    license_path: Path,
    manifest: Path | None = None,
) -> None:
    verify_license(license_path)
    if not header.is_file():
        raise ArtifactError(f"utf8proc header is missing: {header}")
    try:
        with (framework / "Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException) as error:
        raise ArtifactError(f"utf8proc XCFramework Info.plist is invalid: {framework}") from error

    libraries = info.get("AvailableLibraries")
    if not isinstance(libraries, list):
        raise ArtifactError("utf8proc XCFramework has no AvailableLibraries")
    by_identifier = {
        library.get("LibraryIdentifier"): library
        for library in libraries
        if isinstance(library, dict)
    }
    if set(by_identifier) != set(LIBRARY_IDENTIFIERS):
        raise ArtifactError(
            "utf8proc XCFramework must contain exactly ios-arm64 and "
            "ios-arm64-simulator"
        )

    maximum = version_tuple(DEPLOYMENT_TARGET)
    for identifier, contract in LIBRARY_IDENTIFIERS.items():
        library = by_identifier[identifier]
        if library.get("SupportedArchitectures") != ["arm64"]:
            raise ArtifactError(f"{identifier} must contain only arm64")
        if library.get("SupportedPlatform") != "ios":
            raise ArtifactError(f"{identifier} must target iOS")
        if library.get("SupportedPlatformVariant") != contract["variant"]:
            raise ArtifactError(f"{identifier} has the wrong platform variant")
        if library.get("LibraryPath") != "libutf8proc.a":
            raise ArtifactError(f"{identifier} has an unexpected library path")
        archive = framework / identifier / "libutf8proc.a"
        if not archive.is_file():
            raise ArtifactError(f"utf8proc slice is missing: {identifier}")
        if run(["xcrun", "lipo", "-archs", str(archive)]) != "arm64":
            raise ArtifactError(f"{identifier} archive is not arm64-only")
        for platform, minimum in archive_build_versions(archive):
            if platform != contract["platform"]:
                raise ArtifactError(
                    f"{identifier} has Mach-O platform {platform}, "
                    f"expected {contract['platform']}"
                )
            if version_tuple(minimum) > maximum:
                raise ArtifactError(
                    f"{identifier} minimum OS {minimum} exceeds {DEPLOYMENT_TARGET}"
                )

    compile_header(header, "iphoneos", "arm64-apple-ios14.0")
    compile_header(
        header, "iphonesimulator", "arm64-apple-ios14.0-simulator"
    )
    if manifest is not None:
        verify_manifest(manifest, framework, header)


def cache_key(repository_root: Path, vcpkg_root: Path) -> str:
    digest = hashlib.sha256()
    inputs = (
        Path(__file__).resolve(),
        repository_root / "vcpkg.json",
        *(repository_root / "vcpkg-triplets" / f"{triplet}.cmake" for triplet in TRIPLETS),
        vcpkg_root / "vcpkg",
    )
    for path in inputs:
        if not path.is_file():
            raise ArtifactError(f"utf8proc cache-key input is missing: {path}")
        digest.update(str(path.name).encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    digest.update(run(["xcodebuild", "-version"]).encode("utf-8"))
    return digest.hexdigest()[:24]


def write_manifest(root: Path, key: str) -> Path:
    framework = root / "lib/libutf8proc.xcframework"
    header = root / "include/utf8proc.h"
    manifest = {
        "schemaVersion": CACHE_SCHEMA,
        "cacheKey": key,
        "deploymentTarget": DEPLOYMENT_TARGET,
        "files": {
            relative: sha256(path)
            for relative, path in artifact_files(framework, header).items()
        },
    }
    path = root / "manifest.json"
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def build_cache_entry(
    repository_root: Path,
    vcpkg_root: Path,
    cache_parent: Path,
    key: str,
    license_path: Path,
) -> Path:
    vcpkg = vcpkg_root / "vcpkg"
    staging = Path(tempfile.mkdtemp(prefix=".staging-", dir=cache_parent))
    try:
        installed = staging / "installed"
        run(
            [
                str(vcpkg),
                "install",
                "--classic",
                "--recurse",
                *(f"utf8proc:{triplet}" for triplet in TRIPLETS),
                "--overlay-triplets",
                str(repository_root / "vcpkg-triplets"),
                f"--x-install-root={installed}",
            ],
            cwd=vcpkg_root,
        )
        installed_license = installed / "arm64-ios/share/utf8proc/copyright"
        verify_license(installed_license)
        if installed_license.read_bytes() != license_path.read_bytes():
            raise ArtifactError(
                "installed utf8proc license differs from assets/legal/utf8proc.txt"
            )

        framework = staging / "lib/libutf8proc.xcframework"
        framework.parent.mkdir(parents=True)
        run(
            [
                "xcodebuild",
                "-create-xcframework",
                "-library",
                str(installed / "arm64-ios/lib/libutf8proc.a"),
                "-library",
                str(installed / "arm64-ios-simulator/lib/libutf8proc.a"),
                "-output",
                str(framework),
            ]
        )
        header = staging / "include/utf8proc.h"
        header.parent.mkdir(parents=True)
        shutil.copy2(installed / "arm64-ios/include/utf8proc.h", header)
        verify_artifacts(framework, header, license_path)
        shutil.rmtree(installed)
        manifest = write_manifest(staging, key)
        verify_manifest(manifest, framework, header)

        destination = cache_parent / key
        if destination.exists():
            shutil.rmtree(destination)
        os.replace(staging, destination)
        return destination
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def publish_output(cache_entry: Path, output_root: Path, license_path: Path) -> None:
    cached_framework = cache_entry / "lib/libutf8proc.xcframework"
    cached_header = cache_entry / "include/utf8proc.h"
    manifest = cache_entry / "manifest.json"
    target_framework = output_root / "lib/libutf8proc.xcframework"
    target_header = output_root / "include/utf8proc.h"

    try:
        verify_artifacts(
            target_framework, target_header, license_path, manifest
        )
        return
    except ArtifactError:
        pass

    target_framework.parent.mkdir(parents=True, exist_ok=True)
    target_header.parent.mkdir(parents=True, exist_ok=True)
    staged_framework = Path(
        tempfile.mkdtemp(prefix=".libutf8proc-", dir=target_framework.parent)
    ) / "libutf8proc.xcframework"
    staged_header = target_header.with_name(f".{target_header.name}.staging")
    backup_framework = target_framework.with_name(
        f".{target_framework.name}.previous"
    )
    try:
        shutil.copytree(cached_framework, staged_framework)
        shutil.copy2(cached_header, staged_header)
        if backup_framework.exists():
            shutil.rmtree(backup_framework)
        if target_framework.exists():
            os.replace(target_framework, backup_framework)
        os.replace(staged_framework, target_framework)
        os.replace(staged_header, target_header)
        verify_artifacts(target_framework, target_header, license_path, manifest)
        shutil.rmtree(backup_framework, ignore_errors=True)
    except Exception:
        if target_framework.exists():
            shutil.rmtree(target_framework)
        if backup_framework.exists():
            os.replace(backup_framework, target_framework)
        raise
    finally:
        shutil.rmtree(staged_framework.parent, ignore_errors=True)
        staged_header.unlink(missing_ok=True)


def prune_cache(cache_parent: Path, current: Path) -> None:
    for path in cache_parent.iterdir():
        if path == current or path.name == ".lock":
            continue
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink()


def ensure(args: argparse.Namespace) -> None:
    repository_root = args.repository_root.resolve()
    vcpkg_root = args.vcpkg_root.resolve()
    cache_root = args.cache_root.resolve()
    output_root = args.output_root.resolve()
    license_path = repository_root / "assets/legal/utf8proc.txt"
    verify_license(license_path)
    verify_triplets(repository_root)
    key = cache_key(repository_root, vcpkg_root)
    cache_parent = cache_root / "utf8proc"
    cache_parent.mkdir(parents=True, exist_ok=True)
    lock_path = cache_parent / ".lock"
    with lock_path.open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        entry = cache_parent / key
        try:
            verify_artifacts(
                entry / "lib/libutf8proc.xcframework",
                entry / "include/utf8proc.h",
                license_path,
                entry / "manifest.json",
            )
        except ArtifactError:
            if entry.exists():
                shutil.rmtree(entry)
            entry = build_cache_entry(
                repository_root, vcpkg_root, cache_parent, key, license_path
            )
        publish_output(entry, output_root, license_path)
        prune_cache(cache_parent, entry)
    print(f"Verified iOS utf8proc artifacts: {output_root}")


def parser() -> argparse.ArgumentParser:
    repository_root = Path(__file__).resolve().parents[1]
    default_vcpkg = Path(os.environ.get("VCPKG_ROOT", Path.home() / "vcpkg"))
    deploy_cache = Path(
        os.environ.get(
            "IOS_DEPLOY_CACHE_ROOT",
            Path.home() / "Library/Caches/AsoBMaShow/ios-deploy",
        )
    )
    default_cache = Path(
        os.environ.get("IOS_UTF8PROC_CACHE_ROOT", deploy_cache / "native")
    )
    default_output = Path(
        os.environ.get(
            "IOS_UTF8PROC_OUTPUT_ROOT",
            repository_root / "ios/Xcode/AsoBMaShow",
        )
    )

    result = argparse.ArgumentParser()
    commands = result.add_subparsers(dest="command", required=True)
    ensure_parser = commands.add_parser("ensure")
    ensure_parser.add_argument("--repository-root", type=Path, default=repository_root)
    ensure_parser.add_argument("--vcpkg-root", type=Path, default=default_vcpkg)
    ensure_parser.add_argument("--cache-root", type=Path, default=default_cache)
    ensure_parser.add_argument("--output-root", type=Path, default=default_output)

    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--framework", type=Path, required=True)
    verify_parser.add_argument("--header", type=Path, required=True)
    verify_parser.add_argument("--license", type=Path, required=True)
    verify_parser.add_argument("--manifest", type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "ensure":
            ensure(args)
        else:
            verify_artifacts(
                args.framework.resolve(),
                args.header.resolve(),
                args.license.resolve(),
                args.manifest.resolve() if args.manifest else None,
            )
    except ArtifactError as error:
        print(f"iOS utf8proc artifact error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
