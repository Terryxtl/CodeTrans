package com.codetrans

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.util.Size
import androidx.preference.PreferenceManager

/** Typed access to the settings of PLAN §9 (keys match res/xml/preferences.xml). */
class Prefs(context: Context) {
    val sp: SharedPreferences = PreferenceManager.getDefaultSharedPreferences(context.applicationContext)

    /** Analysis stream size (sensor orientation, landscape), e.g. 1920x1080. */
    val analysisSize: Size get() = parseResolution(sp.getString(KEY_RESOLUTION, DEFAULT_RESOLUTION))
    /** Camera frame rate; 0 = auto (highest the camera and the analysis size allow, up to 60). */
    val fps: Int get() = sp.getString(KEY_FPS, "0")?.toIntOrNull()?.coerceAtLeast(0) ?: 0
    val exposureEv: Float get() = sp.getString(KEY_EXPOSURE, "0")?.toFloatOrNull() ?: 0f
    /** 0 = auto. */
    val threads: Int get() = sp.getString(KEY_THREADS, "0")?.toIntOrNull()?.coerceIn(0, 6) ?: 0
    val decimen: Boolean get() = sp.getBoolean(KEY_DECIMEN, true)
    val plainQr: Boolean get() = sp.getBoolean(KEY_PLAIN, true)
    val autoCopy: Boolean get() = sp.getBoolean(KEY_AUTO_COPY, true)
    val autoSave: Boolean get() = Build.VERSION.SDK_INT >= 29 && sp.getBoolean(KEY_AUTO_SAVE, false)
    val vibrate: Boolean get() = sp.getBoolean(KEY_VIBRATE, true)
    val sound: Boolean get() = sp.getBoolean(KEY_SOUND, false)
    val keepScreenOn: Boolean get() = sp.getBoolean(KEY_KEEP_SCREEN_ON, true)
    val dumpFrames: Boolean get() = BuildConfig.DEBUG && sp.getBoolean(KEY_DUMP_FRAMES, false)

    companion object {
        const val KEY_RESOLUTION = "resolution"
        const val KEY_FPS = "fps"
        const val KEY_EXPOSURE = "exposure_ev"
        const val KEY_THREADS = "threads"
        const val KEY_DECIMEN = "decimen"
        const val KEY_PLAIN = "plain_qr"
        const val KEY_AUTO_COPY = "auto_copy"
        const val KEY_AUTO_SAVE = "auto_save"
        const val KEY_VIBRATE = "vibrate"
        const val KEY_SOUND = "sound"
        const val KEY_KEEP_SCREEN_ON = "keep_screen_on"
        const val KEY_DUMP_FRAMES = "debug_dump_frames"
        const val KEY_VERSION = "version"
        const val KEY_LICENSES = "licenses"
        const val CATEGORY_DEBUG = "cat_debug"

        const val DEFAULT_RESOLUTION = "1920x1080"

        fun resolutionValue(w: Int, h: Int) = "${w}x$h"

        /** "WxH" (current format) or a legacy "p" value ("720" / "1080" / "1440" / "2160"). */
        fun parseResolution(v: String?): Size =
            Tuning.parseSize(v)?.let { Size(it.first, it.second) }
                ?: v?.toIntOrNull()?.let(::analysisSize)
                ?: Size(1920, 1080)

        /** Landscape (sensor-oriented) target size for a "p" value, 16:9. */
        fun analysisSize(p: Int): Size = when (p) {
            720 -> Size(1280, 720)
            1440 -> Size(2560, 1440)
            2160 -> Size(3840, 2160)
            else -> Size(1920, 1080)
        }
    }
}
