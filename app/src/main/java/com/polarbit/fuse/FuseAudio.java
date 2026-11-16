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

    public int AudioCreate(int streamType, int sampleRateInHz, int channelConfig, int audioFormat) {
        m_started = false;
        m_initialized = false;
        int channel = (channelConfig == 1) ? 2 : 3;
        int format = (audioFormat == 16) ? 2 : 3;
        int minBuf = AudioTrack.getMinBufferSize(sampleRateInHz, channel, format);
        m_trackbuf_size = minBuf * 2;
        if (m_trackbuf_size <= 0) m_trackbuf_size = 8192;
        m_track = new AudioTrack(streamType, sampleRateInHz, channel, format, m_trackbuf_size, 1);
        m_bytesperframe = (channelConfig * audioFormat) / 8;
        if (m_bytesperframe <= 0) m_bytesperframe = 4;
        m_mixbuf = ByteBuffer.allocateDirect(m_trackbuf_size);
        m_intbuf = new byte[m_trackbuf_size];
        AudioSetVolume();
        Log.i(LOG_TAG, "AudioCreate: rate=" + sampleRateInHz + ", ch=" + channelConfig + ", bufsize=" + m_trackbuf_size);
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
        if (m_track != null && m_am != null) {
            try {
                float v = (float)(m_am.getStreamVolume(3) | 1);
                m_track.setStereoVolume(v, v);
            } catch (Throwable ignored) {}
        }
    }

    public void AudioSetVolume(int left, int right) {
        if (m_track != null) {
            try {
                float max = AudioTrack.getMaxVolume();
                float min = AudioTrack.getMinVolume();
                float range = max - min;
                float l = min + (left * range / 100.0f);
                float r = min + (right * range / 100.0f);
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
        if (period <= 0) period = 1024;
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
                            mixFn.invoke(null, m_mixbuf, periodicFrames);
                            m_mixbuf.get(m_intbuf, 0, writeBytes);
                            m_mixbuf.position(0);
                            int written = m_track.write(m_intbuf, 0, writeBytes);
                            if (written < 0) {
                                Thread.sleep(10);
                            }
                        } else {
                            Thread.sleep(10);
                        }
                    } catch (InterruptedException ie) {
                        break;
                    } catch (Throwable t) {
                        // ignore and continue or break on fatal
                        break;
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
