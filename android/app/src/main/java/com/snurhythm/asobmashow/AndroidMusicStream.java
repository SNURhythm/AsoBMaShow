package com.snurhythm.asobmashow;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.media.PlaybackParams;

/** Android/JNI edges for the music-only PCM feeder. */
final class AndroidMusicStream {
    static final int SAMPLE_RATE = 44100;
    static native int read(long handle, long firstFrame, short[] samples);
    static native long duration(long handle);
    static native String error(long handle);
    static native void enable(long handle, boolean value);
    static native void seek(long handle, long firstFrame);
    static native void release(long handle);
    static native void reportError(String message);

    static NativePcmMusicPlayer create(long handle, NativePcmMusicPlayer.Listener listener) {
        TrackOutput output = new TrackOutput();
        try {
            return new NativePcmMusicPlayer(new NativePcmMusicPlayer.Source() {
                public int read(long frame, short[] samples) { return AndroidMusicStream.read(handle, frame, samples); }
                public long durationFrames() { return duration(handle); }
                public String error() { return AndroidMusicStream.error(handle); }
                public void enable(boolean value) { AndroidMusicStream.enable(handle, value); }
                public void seek(long frame) { AndroidMusicStream.seek(handle, frame); }
                public void close() { release(handle); }
            }, output, listener);
        } catch (RuntimeException e) {
            output.close();
            throw e;
        }
    }

    private static final class TrackOutput implements NativePcmMusicPlayer.Output {
        private final AudioTrack track;
        private final short[] drainPadding;
        private int paddingOffset;
        private boolean draining;

        TrackOutput() {
            int minimum = AudioTrack.getMinBufferSize(SAMPLE_RATE,
                    AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0) throw new IllegalStateException("PCM output is unavailable.");
            int bytes = Math.max(minimum * 4, 16384);
            track = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                    .setAudioFormat(new AudioFormat.Builder().setSampleRate(SAMPLE_RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(bytes).build();
            if (track.getState() != AudioTrack.STATE_INITIALIZED) {
                track.release();
                throw new IllegalStateException("Could not initialize PCM output.");
            }
            drainPadding = new short[bytes / 2];
        }
        public int write(short[] samples, int offset, int count) {
            return track.write(samples, offset, count, AudioTrack.WRITE_NON_BLOCKING);
        }
        public long headFrames() {
            // A very short final buffer may be below AudioTrack's start threshold.
            // Padding wakes the hardware; the player counts only real source frames.
            if (draining && paddingOffset < drainPadding.length) {
                int written = write(drainPadding, paddingOffset, drainPadding.length - paddingOffset);
                if (written < 0) throw new IllegalStateException("Could not drain PCM output: " + written);
                paddingOffset += written;
            }
            return Integer.toUnsignedLong(track.getPlaybackHeadPosition());
        }
        public void play() { track.play(); }
        public void pause() { track.pause(); }
        public void flush() {
            track.flush();
            draining = false;
            paddingOffset = 0;
        }
        public void rate(float speed, boolean stretch) {
            track.setPlaybackParams(new PlaybackParams().allowDefaults()
                    .setSpeed(speed).setPitch(stretch ? 1.0f : speed));
        }
        public void end() { draining = true; }
        public void close() { track.release(); }
    }
}
