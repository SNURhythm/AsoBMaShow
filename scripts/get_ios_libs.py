# 0. detect vcpkg path (priority: env var VCPKG_ROOT, then default location)
# 1. install ffmpeg, libsndfile from vcpkg with both arm64-ios and arm64-ios-simulator triplets
# 2. generate xcframeworks with xcodebuild -create-xcframework to merge iOS Device and iOS Simulator triplets
# 3. copy them to ios/Xcode/AsoBMaShow/lib

import argparse
import os
import subprocess
import shutil
import sys

parser = argparse.ArgumentParser(description="Rebuild checked-in iOS native libraries")
parser.add_argument(
    "--packages", nargs="+",
    choices=["ffmpeg", "libsndfile", "libarchive", "lib7zip", "luajit", "utf8proc"],
    default=["ffmpeg", "libsndfile", "libarchive", "lib7zip", "luajit", "utf8proc"],
    help="Rebuild only the selected libraries (default: all)",
)
args = parser.parse_args()

# 0. detect vcpkg path (priority: env var VCPKG_ROOT, then default location)
vcpkg_root = os.getenv("VCPKG_ROOT")
if vcpkg_root is None:
    vcpkg_root = os.path.join(os.path.expanduser("~"), "vcpkg")
# copy current path
current_path = f"{os.path.dirname(os.path.realpath(__file__))}/.."
tmp_path = os.path.join(current_path, "tmp")
triplet_overlay_path = os.path.join(current_path, "vcpkg-triplets")
shutil.rmtree(tmp_path, ignore_errors=True)
# set pwd to vcpkg_root
os.chdir(vcpkg_root)
print(f"VCPKG_ROOT: {vcpkg_root}")
os.makedirs(tmp_path, exist_ok=True)
dependency = {
    "ffmpeg": ["libavformat", "libavcodec", "libavutil", "libswresample", "libswscale", "libavdevice", "libavfilter"],
    "libsndfile": ["libsndfile", "libFLAC", "libFLAC++", "libvorbis", "libvorbisenc", "libvorbisfile", "libmp3lame", "libmpg123", "libsyn123", "libout123", "libopus", "libogg"],
    "libarchive": ["libarchive", "libz", "libbz2", "liblz4", "liblzma", "libzstd"],
    "lib7zip": ["lib7zip"],
}
extra_static_dependencies = {
    "ffmpeg": ["libx264", "libdav1d"],
}
package_specs = {
    "ffmpeg": "ffmpeg[dav1d,gpl,x264]",
    "libarchive": "libarchive[core,bzip2,lz4,lzma,zstd]",
    "lib7zip": "7zip",
}
triplets = ["arm64-ios", "arm64-ios-simulator"]

# 1. ffmpeg, libsndfile from vcpkg with both arm64-ios and arm64-ios-simulator triplets
def install_package(package_name):
    # join like package_name:triplet1 package_name:triplet2
    package_spec = package_specs.get(package_name, package_name)
    joined_triplets = [f"{package_spec}:{triplet}" for triplet in triplets]
    # Classic install treats an installed package as current even when its
    # overlay changed. Remove the selected packages first; the subsequent
    # install recomputes their ABI and can still reuse matching binary caches.
    port_name = package_spec.split("[", 1)[0]
    subprocess.run([f"{vcpkg_root}/vcpkg", "remove", "--classic", "--recurse",
                    *[f"{port_name}:{triplet}" for triplet in triplets]], check=True)
    subprocess.run([f"{vcpkg_root}/vcpkg", "install", "--classic", "--recurse", *joined_triplets, "--overlay-ports", f"{current_path}/vcpkg-overlays", "--overlay-triplets", triplet_overlay_path], check=True)


# 2. generate xcframeworks with xcodebuild -create-xcframework to merge iOS Device and iOS Simulator triplets
def generate_xcframework(package_name, is_fat, library_name=None, output_name=None):
    library_name = package_name if library_name is None else library_name
    output_name = package_name if output_name is None else output_name
    libs = []
    if is_fat:
        for triplet in triplets:
            libs += (["-library", f"{tmp_path}/{package_name}-{triplet}.a"])
    else:
        for triplet in triplets:
            libs += (["-library", f"{vcpkg_root}/installed/{triplet}/lib/{library_name}.a"])
    print(libs)
    subprocess.run(["xcodebuild", "-create-xcframework", *libs, "-output", f"{tmp_path}/{output_name}.xcframework"], check=True)


# 3. copy them to ios/Xcode/AsoBMaShow/lib
def copy_xcframework(package_name, output_name=None):
    output_name = package_name if output_name is None else output_name
    destination = f"{current_path}/ios/Xcode/AsoBMaShow/lib/{output_name}.xcframework"
    # Slice names may change with a port update; never retain old archives.
    shutil.rmtree(destination, ignore_errors=True)
    shutil.copytree(f"{tmp_path}/{output_name}.xcframework", destination)

def copy_includes(package_name, is_dir=False):
    subprocess.run(["cp", "-r" if is_dir else "-f", f"{vcpkg_root}/installed/arm64-ios/include/{package_name}", f"{current_path}/ios/Xcode/AsoBMaShow/include"], check=True)

def copy_license(package_name, output_name):
    output_dir = f"{current_path}/assets/legal"
    os.makedirs(output_dir, exist_ok=True)
    shutil.copyfile(f"{vcpkg_root}/installed/arm64-ios/share/{package_name}/copyright", f"{output_dir}/{output_name}")

def merge_all_dependents(package_name):
    for triplet in triplets:
        libtool_merge_list= [
            f"{vcpkg_root}/installed/{triplet}/lib/{dep}.a"
            for dep in dependency[package_name] + extra_static_dependencies.get(package_name, [])
        ]
        subprocess.run(["libtool", "-static", "-o", f"{tmp_path}/{package_name}-{triplet}.a", *libtool_merge_list], check=True)

for package in args.packages:
    if package == "utf8proc":
        subprocess.run(
            [sys.executable, f"{current_path}/scripts/ios_utf8proc.py", "ensure",
             "--repository-root", current_path, "--vcpkg-root", vcpkg_root],
            check=True,
        )
        continue

    install_package(package)
    if package == "luajit":
        generate_xcframework("luajit", False, "libluajit-5.1", "libluajit")
        copy_xcframework("luajit", "libluajit")
        for header in ["lua.h", "lauxlib.h", "lualib.h", "luaconf.h", "luajit.h", "lua.hpp"]:
            copy_includes(f"luajit-2.1/{header}")
        continue

    merge_all_dependents(package)
    generate_xcframework(package, True)
    copy_xcframework(package)
    if package == "ffmpeg":
        for name in dependency["ffmpeg"]:
            copy_includes(name, True)
        for name in ["ffmpeg", "x264", "dav1d"]:
            copy_license(name, f"{name}.txt")
    elif package == "libsndfile":
        copy_includes("sndfile.h")
    elif package == "libarchive":
        copy_includes("archive.h")
        copy_includes("archive_entry.h")
    elif package == "lib7zip":
        copy_includes("7zip", True)
        copy_license("7zip", "7zip.txt")

shutil.rmtree(tmp_path)
