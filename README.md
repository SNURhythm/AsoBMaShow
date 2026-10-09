# TestFlight is now available!
[Join TestFlight](https://testflight.apple.com/join/D2UfeQVW) to get the latest beta version of AsoBMaShow for iOS.

# AsoBMaShow
![iOS Build Status](https://github.com/SNURhythm/AsoBMaShow/actions/workflows/mobile-beta-deploy.yml/badge.svg)
![Build Status](https://github.com/SNURhythm/AsoBMaShow/actions/workflows/macos-build.yml/badge.svg)

AsoBMaShow \[asobimashou: Let's play\] is a crossplatform BMS player which depends on open source libraries only

## Download
You can download AsoBMaShow from [Releases](https://github.com/SNURhythm/AsoBMaShow/releases/latest)

## Development
### Setup for macOS/iOS

```bash
git clone https://github.com/SNURhythm/AsoBMaShow.git
cd AsoBMaShow
git submodule update --init --recursive
./scripts/ios_init.sh # initialize bgfx xcodeproj & cocoapods for iOS
./scripts/macos_init.sh
```

### Setup for Windows
```bash
git clone https://github.com/SNURhythm/AsoBMaShow.git
cd AsoBMaShow
git submodule update --init --recursive
./scripts/windows_init.sh
```

### Current Progress 
- [x] BGA playback
- [x] Gameplay
- [x] Course mode
- [x] Integrate Bokutachi IR
- [x] Chart viewer
- [ ] Support PMS
- [ ] Portrait mode (mobile)
- [x] Support skin (Lua gameplay skins are enabled on desktop builds)
- [ ] Use responsive layouts for menus and dialogs based on available window space and density-independent sizing. Keep text and touch targets readable, adapt columns to screen size and orientation, and preserve the fixed gameplay canvas.

### Repository guide

See the [repository guide](docs/repository-guide.md) for the current codebase
map and [feature documentation](docs/features/README.md) for subsystem intent,
ownership, and focused test locations.

## Dependency

- SDL3 + bgfx
- FFmpeg (for BGA rendering)
- SQLite3
- PortAudio (for desktop) + miniaudio (for mobile)
- libsndfile
- 7-Zip SDK (archive reading; see [third-party notices](THIRD_PARTY_NOTICES.md))
- [bms-parser-cpp](https://github.com/SNURhythm/bms-parser-cpp) for fast BMS parsing
