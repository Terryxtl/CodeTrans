package com.codetrans

import android.app.Application
import android.os.Build
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat

class CodeTransApp : Application() {
    override fun onCreate() {
        super.onCreate()
        // No history: results of a previous process are discarded before any receiver exists.
        ResultStore.clearAll(this)
        // Below API 33 AppCompat keeps the app locale in memory only, so it is set on every start,
        // before the first Activity (the documented exception for API < 33).
        if (Build.VERSION.SDK_INT < 33) forceAppLocale()
    }

    companion object {
        /** The UI is Simplified Chinese only, whatever the system language (library strings too). */
        private const val APP_LOCALE = "zh-CN"

        /**
         * API 33+: the system stores the per-app locale, so this only changes something on the very
         * first start (the Activity is recreated once). Call it after Activity.onCreate there.
         */
        fun forceAppLocale() {
            if (AppCompatDelegate.getApplicationLocales().toLanguageTags() != APP_LOCALE) {
                AppCompatDelegate.setApplicationLocales(LocaleListCompat.forLanguageTags(APP_LOCALE))
            }
        }
    }
}
