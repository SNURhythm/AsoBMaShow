# 0. detect vcpkg path (priority: env var VCPKG_ROOT, then default location)
# 1. install ffmpeg, libsndfile from vcpkg with both arm64-ios and arm64-ios-simulator triplets
# 2. generate xcframeworks with xcodebuild -create-xcframework to merge iOS Device and iOS Simulator triplets
# 3. copy them to ios/Xcode/AsoBMaShow/lib

import os
import subprocess
import shutil

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
}
package_specs = {
    "libarchive": "libarchive[core,bzip2,lz4,lzma,zstd]",
}
triplets = ["arm64-ios", "arm64-ios-simulator"]

# 1. ffmpeg, libsndfile from vcpkg with both arm64-ios and arm64-ios-simulator triplets
def install_package(package_name):
    # join like package_name:triplet1 package_name:triplet2
    package_spec = package_specs.get(package_name, package_name)
    joined_triplets = [f"{package_spec}:{triplet}" for triplet in triplets]
    subprocess.run([f"{vcpkg_root}/vcpkg", "install", "--classic", *joined_triplets, "--overlay-ports", f"{current_path}/vcpkg-overlays", "--overlay-triplets", triplet_overlay_path], check=True)


# 2. generate xcframeworks with xcodebuild -create-xcframework to merge iOS Device and iOS Simulator triplets
def generate_xcframework(package_name, is_fat):
    libs = []
    if is_fat:
        for triplet in triplets:
            libs += (["-library", f"{tmp_path}/{package_name}-{triplet}.a"])
    else:
        for triplet in triplets:
            libs += (["-library", f"{vcpkg_root}/installed/{triplet}/lib/{package_name}.a"])
    print(libs)
    subprocess.run(["xcodebuild", "-create-xcframework", *libs, "-output", f"{tmp_path}/{package_name}.xcframework"], check=True)


# 3. copy them to ios/Xcode/AsoBMaShow/lib
def copy_xcframework(package_name):
    subprocess.run(["cp", "-r", f"{tmp_path}/{package_name}.xcframework", f"{current_path}/ios/Xcode/AsoBMaShow/lib"], check=True)

def copy_includes(package_name, is_dir=False):
    subprocess.run(["cp", "-r" if is_dir else "-f", f"{vcpkg_root}/installed/arm64-ios/include/{package_name}", f"{current_path}/ios/Xcode/AsoBMaShow/include"], check=True)

def merge_all_dependents(package_name):
    for triplet in triplets:
        libtool_merge_list= [f"{vcpkg_root}/installed/{triplet}/lib/{dep}.a" for dep in dependency[package_name]]
        subprocess.run(["libtool", "-static", "-o", f"{tmp_path}/{package_name}-{triplet}.a", *libtool_merge_list], check=True)

# ffmpeg (libavformat, libavcodec, libavutil, libswresample, libswscale, libavdevice, libavfilter)
install_package("ffmpeg")
merge_all_dependents("ffmpeg")
generate_xcframework("ffmpeg", True)
copy_xcframework("ffmpeg")
[copy_includes(name, True) for name in dependency["ffmpeg"]]

install_package("libsndfile")
merge_all_dependents("libsndfile")
generate_xcframework("libsndfile", True)
copy_xcframework("libsndfile")
copy_includes("sndfile.h")

install_package("libarchive")
merge_all_dependents("libarchive")
generate_xcframework("libarchive", True)
copy_xcframework("libarchive")
copy_includes("archive.h")
copy_includes("archive_entry.h")
# remove tmp
shutil.rmtree(tmp_path)
