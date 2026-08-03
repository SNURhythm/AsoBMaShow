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
    "lib7zip": ["lib7zip"],
}
extra_static_dependencies = {
    "ffmpeg": ["libx264"],
}
package_specs = {
    "ffmpeg": "ffmpeg[gpl,x264]",
    "libarchive": "libarchive[core,bzip2,lz4,lzma,zstd]",
    "lib7zip": "7zip",
}
triplets = ["arm64-ios", "arm64-ios-simulator"]

# 1. ffmpeg, libsndfile from vcpkg with both arm64-ios and arm64-ios-simulator triplets
def install_package(package_name):
    # join like package_name:triplet1 package_name:triplet2
    package_spec = package_specs.get(package_name, package_name)
    joined_triplets = [f"{package_spec}:{triplet}" for triplet in triplets]
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
    subprocess.run(["cp", "-r", f"{tmp_path}/{output_name}.xcframework", f"{current_path}/ios/Xcode/AsoBMaShow/lib"], check=True)

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

# ffmpeg (libavformat, libavcodec, libavutil, libswresample, libswscale, libavdevice, libavfilter)
install_package("ffmpeg")
merge_all_dependents("ffmpeg")
generate_xcframework("ffmpeg", True)
copy_xcframework("ffmpeg")
[copy_includes(name, True) for name in dependency["ffmpeg"]]
copy_license("ffmpeg", "ffmpeg.txt")
copy_license("x264", "x264.txt")

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

install_package("lib7zip")
merge_all_dependents("lib7zip")
generate_xcframework("lib7zip", True)
copy_xcframework("lib7zip")
copy_includes("7zip", True)
copy_license("7zip", "7zip.txt")

install_package("utf8proc")
generate_xcframework("utf8proc", False, "libutf8proc", "libutf8proc")
copy_xcframework("utf8proc", "libutf8proc")
copy_includes("utf8proc.h")
copy_license("utf8proc", "utf8proc.txt")
# remove tmp
shutil.rmtree(tmp_path)
