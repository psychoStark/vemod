/*
 * Copyright (C) 2026 Vemod Project (psychoStark)
 *
 * Clean-Room Interoperability Shim for Polarbit Fuse Engine (ABI Compatibility).
 *
 * This file is an independent, clean-room reimplementation designed strictly
 * for runtime binary (ABI) interoperability with 32-bit ARM guest game engines
 * (e.g. Polarbit Reckless Racing Lite).
 *
 * It contains no proprietary source code or intellectual property from Polarbit AB.
 * All trademarks, package names, and brand identifiers are the property of
 * their respective owners and are used solely under nominative fair use for
 * binary execution within the container virtualization layer.
 *
 * Licensed under the GNU General Public License v3 (or later).
 */

package com.vemod.container.shims;

import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.os.Process;
import android.util.Log;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

/**
 * Clean-room audio engine shim for Polarbit 3D games.
 *
 * Manages low-latency AudioTrack output, normalizes legacy parameter inversions,
 * and executes a dedicated urgent-priority audio pumping thread interfacing
 * with the native JNI mixer.
 */
public class PolarbitFuseAudio implements AudioTrack.OnPlaybackPositionUpdateListener {
    private static final String TAG = "PolarbitFuseAudio";

    // Public fields preserved for ABI/reflective compatibility with legacy callers
    public AudioManager m_am;
    public boolean m_started;

    private int bytesPerFrame;
    private boolean isInitialized;
    private byte[] pcmTransferBuffer;
    private int lastFramePosition;
    private ByteBuffer directMixBuffer;
    private AudioTrack audioTrack;
    private int trackBufferSize;
    private Method audioMixMethod;
    private Thread audioPumpThread;

    public PolarbitFuseAudio(Context context) {
        m_am = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        try {
            Class<?> jniClass = context.getClassLoader().loadClass("com.polarbit.fuse.Jni");
            audioMixMethod = jniClass.getMethod("AudioMix", ByteBuffer.class, int.class);
            Log.i(TAG, "PolarbitFuseAudio: successfully resolved Jni.AudioMix reflection handle");
        } catch (Throwable t) {
            Log.w(TAG, "PolarbitFuseAudio: could not resolve Jni.AudioMix: " + t.getMessage());
        }
    }

    /**
     * Initializes the audio track.
     *
     * Handles legacy parameter packing/inversion from Polarbit native bridge:
     * (sampleRate, channels, audioFormat, in_bufsize)
     */
    public int AudioCreate(int arg0, int arg1, int arg2, int arg3) {
        m_started = false;
        isInitialized = false;

        int sampleRate = arg0;
        int channels = arg1;
        int audioFormat = arg2;
        int requestedBufferSize = arg3;

        // Detect inverted arguments from legacy native caller where sampleRate and bufferSize were swapped
        if (sampleRate < 4000 && requestedBufferSize >= 4000) {
            int swap = sampleRate;
            sampleRate = requestedBufferSize;
            requestedBufferSize = swap;
        }

        // Clamp sample rate within standard Android audio hardware constraints
        if (sampleRate < 4000 || sampleRate > 192000) {
            sampleRate = 22050;
        }

        int channelCount = (channels == 1) ? 1 : 2;
        int channelConfig = (channelCount == 1) ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
        int encoding = (audioFormat == 8) ? AudioFormat.ENCODING_PCM_8BIT : AudioFormat.ENCODING_PCM_16BIT;
        int bytesPerSample = (audioFormat == 8) ? 1 : 2;

        bytesPerFrame = channelCount * bytesPerSample;
        int minBuffer = AudioTrack.getMinBufferSize(sampleRate, channelConfig, encoding);
        trackBufferSize = Math.max(minBuffer * 2, (requestedBufferSize > 0 && requestedBufferSize < 65536) ? requestedBufferSize : 8192);
        if (trackBufferSize <= 0) {
            trackBufferSize = 8192;
        }

        try {
            audioTrack = new AudioTrack(
                AudioManager.STREAM_MUSIC,
                sampleRate,
                channelConfig,
                encoding,
                trackBufferSize,
                AudioTrack.MODE_STREAM
            );
        } catch (Throwable t) {
            Log.e(TAG, "PolarbitFuseAudio: Failed to instantiate AudioTrack, falling back to standard stereo", t);
            audioTrack = new AudioTrack(
                AudioManager.STREAM_MUSIC,
                22050,
                AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT,
                8192,
                AudioTrack.MODE_STREAM
            );
        }

        directMixBuffer = ByteBuffer.allocateDirect(trackBufferSize);
        pcmTransferBuffer = new byte[trackBufferSize];
        AudioSetVolume();

        Log.i(TAG, "AudioCreate: rate=" + sampleRate + ", ch=" + channelCount +
                   ", bits=" + (bytesPerSample * 8) + ", bufsize=" + trackBufferSize +
                   ", bytesperframe=" + bytesPerFrame);
        return 0;
    }

    public int AudioGetVolume() {
        return 1;
    }

    void AudioPlay() {
        if (!m_started && audioTrack != null) {
            audioTrack.play();
            m_started = true;
        }
    }

    void AudioSetVolume() {
        if (audioTrack != null) {
            try {
                audioTrack.setStereoVolume(1.0f, 1.0f);
            } catch (Throwable ignored) {}
        }
    }

    public void AudioSetVolume(int left, int right) {
        if (audioTrack != null) {
            try {
                float l = Math.max(0.0f, Math.min(1.0f, left / 100.0f));
                float r = Math.max(0.0f, Math.min(1.0f, right / 100.0f));
                if (l == 0f && r == 0f && (left > 0 || right > 0)) {
                    l = 1.0f;
                    r = 1.0f;
                }
                audioTrack.setStereoVolume(l, r);
            } catch (Throwable ignored) {}
        }
    }

    public int AudioStart() {
        if (m_started) return 0;
        m_started = true;
        isInitialized = true;
        if (audioTrack == null) return 0;

        int period = trackBufferSize / (bytesPerFrame * 2);
        if (period <= 0) period = 512;
        if (period > 2048) period = 1024;
        lastFramePosition = 0;

        try {
            audioTrack.setPositionNotificationPeriod(period);
            audioTrack.play();
            audioTrack.write(pcmTransferBuffer, 0, trackBufferSize);
        } catch (Throwable t) {
            Log.w(TAG, "AudioStart: play error", t);
        }

        if (audioPumpThread != null) {
            try {
                audioPumpThread.interrupt();
            } catch (Throwable ignored) {}
        }

        final int periodicFrames = period;
        final int writeBytes = bytesPerFrame * periodicFrames;
        final Method mixFunction = audioMixMethod;

        audioPumpThread = new Thread("PolarbitFuseAudioPump") {
            @Override
            public void run() {
                Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);
                while (m_started && audioTrack != null) {
                    try {
                        if (directMixBuffer != null && pcmTransferBuffer != null && mixFunction != null) {
                            directMixBuffer.position(0);
                            directMixBuffer.limit(writeBytes);
                            mixFunction.invoke(null, directMixBuffer, periodicFrames);
                            directMixBuffer.position(0);
                            int toRead = Math.min(writeBytes, directMixBuffer.remaining());
                            directMixBuffer.get(pcmTransferBuffer, 0, toRead);
                            directMixBuffer.position(0);
                            int written = audioTrack.write(pcmTransferBuffer, 0, toRead);
                            if (written < 0) {
                                Thread.sleep(10);
                            }
                        } else {
                            Thread.sleep(10);
                        }
                    } catch (InterruptedException ie) {
                        break;
                    } catch (Throwable t) {
                        Log.w(TAG, "PolarbitFuseAudioPump suppressed error: " + t.getMessage());
                        try {
                            Thread.sleep(20);
                        } catch (Throwable ignored) {}
                    }
                }
            }
        };
        audioPumpThread.start();
        Log.i(TAG, "AudioStart: pump thread started with period=" + periodicFrames + ", writeBytes=" + writeBytes);
        return 1;
    }

    public void AudioStop() {
        m_started = false;
        if (audioPumpThread != null) {
            audioPumpThread.interrupt();
            audioPumpThread = null;
        }
        if (audioTrack != null) {
            try {
                audioTrack.flush();
                audioTrack.stop();
            } catch (Throwable ignored) {}
        }
    }

    boolean IsStarted() {
        return m_started;
    }

    @Override
    public void onMarkerReached(AudioTrack track) {}

    @Override
    public void onPeriodicNotification(AudioTrack track) {}
}
