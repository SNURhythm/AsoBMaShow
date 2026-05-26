ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"
cd bgfx
mkdir build
cd build
cmake -GXcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos ..
cd "${ROOT_DIR}/ios/Xcode/AsoBMaShow"
bundle install
bundle exec pod repo update
bundle exec pod install