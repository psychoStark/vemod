package com.polarbit.fuse;

import android.content.Context;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

public class FuseAudio implements AudioTrack.OnPlaybackPositionUpdateListener {
    private static final String LOG_TAG = "FuseAudio";
    private static boolean mDebug = false;

    public AudioManager m_am;
    private int m_bytesperframe;
    private boolean m_initialized;
    private byte[] m_intbuf;
    private int m_lastframe;
    private ByteBuffer m_mixbuf;
    public boolean m_started;
    private AudioTrack m_track;
    private int m_trackbuf_size;
    private Method m_audioMixMethod;
    private Thread m_pumpThread;

    public FuseAudio(Context context) {
        m_am = (AudioManager) context.getSystemService("audio");
        try {
            Class<?> jniCls = context.getClassLoader().loadClass("com.polarbit.fuse.Jni");
            m_audioMixMethod = jniCls.getMethod("AudioMix", ByteBuffer.class, int.class);
            Log.i(LOG_TAG, "FuseAudio: resolved Jni.AudioMix method");
        } catch (Throwable t) {
            Log.w(LOG_TAG, "FuseAudio: could not resolve Jni.AudioMix: " + t.getMessage());
        }
    }

    public int AudioCreate(int arg0, int arg1, int arg2, int arg3) {
        m_started = false;
        m_initialized = false;

        // Polarbit signature: AudioCreate(sampleRate, channels, audioFormat, in_bufsize)
        // E.g. (22050, 2, 16, 14176)
        int sampleRate = arg0;
        int channels = arg1;
        int audioFormat = arg2;
        int in_bufsize = arg3;

        // Handle swapped cases if any
        if (sampleRate < 4000 && in_bufsize >= 4000) {
            int tmp = sampleRate;
            sampleRate = in_bufsize;
            in_bufsize = tmp;
        }
        if (sampleRate < 4000 || sampleRate > 192000) {
            sampleRate = 22050;
        }

        int numChannels = (channels == 1) ? 1 : 2;
        int channelConfig = (numChannels == 1) ? android.media.AudioFormat.CHANNEL_OUT_MONO : android.media.AudioFormat.CHANNEL_OUT_STEREO;
        int encoding = (audioFormat == 8) ? android.media.AudioFormat.ENCODING_PCM_8BIT : android.media.AudioFormat.ENCODING_PCM_16BIT;
        int bytesPerSample = (audioFormat == 8) ? 1 : 2;

        m_bytesperframe = numChannels * bytesPerSample;
        int minBuf = AudioTrack.getMinBufferSize(sampleRate, channelConfig, encoding);
        m_trackbuf_size = Math.max(minBuf * 2, (in_bufsize > 0 && in_bufsize < 65536) ? in_bufsize : 8192);
        if (m_trackbuf_size <= 0) m_trackbuf_size = 8192;

        try {
            m_track = new AudioTrack(AudioManager.STREAM_MUSIC, sampleRate, channelConfig, encoding, m_trackbuf_size, AudioTrack.MODE_STREAM);
        } catch (Throwable t) {
            Log.e(LOG_TAG, "FuseAudio: Failed to instantiate AudioTrack", t);
            m_track = new AudioTrack(AudioManager.STREAM_MUSIC, 22050, android.media.AudioFormat.CHANNEL_OUT_STEREO, android.media.AudioFormat.ENCODING_PCM_16BIT, 8192, AudioTrack.MODE_STREAM);
        }

        m_mixbuf = ByteBuffer.allocateDirect(m_trackbuf_size);
        m_intbuf = new byte[m_trackbuf_size];
        AudioSetVolume();
        Log.i(LOG_TAG, "AudioCreate: rate=" + sampleRate + ", ch=" + numChannels + ", bits=" + (bytesPerSample * 8) + ", bufsize=" + m_trackbuf_size + ", bytesperframe=" + m_bytesperframe);
        return 0;
    }

    public int AudioGetVolume() {
        return 1;
    }

    void AudioPlay() {
        if (!m_started && m_track != null) {
            m_track.play();
            m_started = true;
        }
    }

    void AudioSetVolume() {
        if (m_track != null) {
            try {
                m_track.setStereoVolume(1.0f, 1.0f);
            } catch (Throwable ignored) {}
        }
    }

    public void AudioSetVolume(int left, int right) {
        if (m_track != null) {
            try {
                float l = Math.max(0.0f, Math.min(1.0f, left / 100.0f));
                float r = Math.max(0.0f, Math.min(1.0f, right / 100.0f));
                if (l == 0f && r == 0f && (left > 0 || right > 0)) {
                    l = 1.0f;
                    r = 1.0f;
                }
                m_track.setStereoVolume(l, r);
            } catch (Throwable ignored) {}
        }
    }

    public int AudioStart() {
        if (m_started) return 0;
        m_started = true;
        m_initialized = true;
        if (m_track == null) return 0;

        int period = m_trackbuf_size / (m_bytesperframe * 2);
        if (period <= 0) period = 512;
        if (period > 2048) period = 1024;
        m_lastframe = 0;
        try {
            m_track.setPositionNotificationPeriod(period);
            m_track.play();
            m_track.write(m_intbuf, 0, m_trackbuf_size);
        } catch (Throwable t) {
            Log.w(LOG_TAG, "AudioStart: play error", t);
        }

        if (m_pumpThread != null) {
            try { m_pumpThread.interrupt(); } catch (Throwable ignored) {}
        }
        final int periodicFrames = period;
        final int writeBytes = m_bytesperframe * periodicFrames;
        final Method mixFn = m_audioMixMethod;
        m_pumpThread = new Thread("FuseAudioPump") {
            @Override
            public void run() {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO);
                while (m_started && m_track != null) {
                    try {
                        if (m_mixbuf != null && m_intbuf != null && mixFn != null) {
                            m_mixbuf.position(0);
                            m_mixbuf.limit(writeBytes);
                            mixFn.invoke(null, m_mixbuf, periodicFrames);
                            m_mixbuf.position(0);
                            int toRead = Math.min(writeBytes, m_mixbuf.remaining());
                            m_mixbuf.get(m_intbuf, 0, toRead);
                            m_mixbuf.position(0);
                            int written = m_track.write(m_intbuf, 0, toRead);
                            if (written < 0) {
                                Thread.sleep(10);
                            }
                        } else {
                            Thread.sleep(10);
                        }
                    } catch (InterruptedException ie) {
                        break;
                    } catch (Throwable t) {
                        Log.w(LOG_TAG, "FuseAudioPump suppressed error: " + t.getMessage());
                        try { Thread.sleep(20); } catch (Throwable ignored) {}
                    }
                }
            }
        };
        m_pumpThread.start();
        Log.i(LOG_TAG, "AudioStart: pump thread started with period=" + periodicFrames + ", writeBytes=" + writeBytes);
        return 1;
    }

    public void AudioStop() {
        m_started = false;
        if (m_pumpThread != null) {
            m_pumpThread.interrupt();
            m_pumpThread = null;
        }
        if (m_track != null) {
            try {
                m_track.flush();
                m_track.stop();
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
