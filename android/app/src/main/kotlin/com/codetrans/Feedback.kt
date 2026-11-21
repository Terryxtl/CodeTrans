package com.codetrans

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.media.AudioManager
import android.media.ToneGenerator
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import androidx.core.content.getSystemService

/** Completion feedback: vibration, tone, clipboard. */
class Feedback(context: Context) {
    private val app = context.applicationContext

    fun vibrate() {
        val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
            app.getSystemService<VibratorManager>()?.defaultVibrator
        } else {
            app.getSystemService<Vibrator>()
        }
        if (vibrator?.hasVibrator() != true) return
        try {
            if (Build.VERSION.SDK_INT >= 26) {
                vibrator.vibrate(VibrationEffect.createOneShot(60, VibrationEffect.DEFAULT_AMPLITUDE))
            } else {
                @Suppress("DEPRECATION")
                vibrator.vibrate(60)
            }
        } catch (e: SecurityException) {
            Log.w("CodeTrans", "vibrate failed", e)
        }
    }

    fun beep() {
        val tg = runCatching { ToneGenerator(AudioManager.STREAM_NOTIFICATION, 80) }.getOrNull() ?: return
        tg.startTone(ToneGenerator.TONE_PROP_ACK, 150)
        Handler(Looper.getMainLooper()).postDelayed({ tg.release() }, 400)
    }

    /** Returns true when copied; Android 13+ shows its own confirmation. */
    fun copyText(text: String): Boolean = try {
        app.getSystemService<ClipboardManager>()
            ?.setPrimaryClip(ClipData.newPlainText(app.getString(R.string.app_name), text))
        true
    } catch (e: RuntimeException) {
        Log.w("CodeTrans", "clipboard failed", e)
        false
    }
}
