target_sources(main PRIVATE AudioBackend.cpp AudioDeviceManager.cpp AudioMix.cpp
                            AudioWrapper.cpp ChartAudioRenderer.cpp
                            MusicPreparation.cpp ClubBeat.cpp decoder.cpp DecodedAssetCache.cpp
                            GameplayBgaMissStateTracker.cpp Jukebox.cpp
                            MusicPlayerService.cpp MusicPlaylist.cpp
                            NativeMusicPlayer.cpp PrepMetronomeSound.cpp
                            ProgressiveMusicSource.cpp PreviewStreamPlayer.cpp
                            SkinSystemSoundService.cpp)

if(IOS)
    target_sources(main PRIVATE IOSMusicStream.mm)
endif()
