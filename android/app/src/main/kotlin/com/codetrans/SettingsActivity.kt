package com.codetrans

import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.view.View
import androidx.activity.SystemBarStyle
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import com.codetrans.databinding.ActivitySettingsBinding
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlin.math.abs

/** PLAN §9. Changes take effect when the user returns to the main screen (MainActivity.onStart). */
class SettingsActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)
        val b = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(b.root)
        b.toolbar.setNavigationOnClickListener { finish() }
        ViewCompat.setOnApplyWindowInsetsListener(b.root) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            b.toolbar.updatePadding(left = bars.left, top = bars.top, right = bars.right)
            b.settingsContainer.updatePadding(left = bars.left, right = bars.right)
            insets
        }
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction()
                .replace(R.id.settingsContainer, SettingsFragment())
                .commit()
        }
    }

    class SettingsFragment : PreferenceFragmentCompat() {
        override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
            setPreferencesFromResource(R.xml.preferences, rootKey)
            val ctx = requireContext()

            setupCameraPrefs(CameraCaps.backCamera(ctx))
            findPreference<Preference>(Prefs.KEY_AUTO_SAVE)?.isVisible = Build.VERSION.SDK_INT >= 29
            findPreference<Preference>(Prefs.CATEGORY_DEBUG)?.isVisible = BuildConfig.DEBUG

            findPreference<ListPreference>(Prefs.KEY_EXPOSURE)?.summaryProvider =
                Preference.SummaryProvider<ListPreference> { p ->
                    getString(R.string.pref_exposure_summary, p.entry ?: "0 EV")
                }
            val n = Runtime.getRuntime().availableProcessors()
            val auto = Tuning.autoThreads(Tuning.readCpuMaxFreqs(n), n)
            findPreference<ListPreference>(Prefs.KEY_THREADS)?.summaryProvider =
                Preference.SummaryProvider<ListPreference> { p ->
                    if (p.value == null || p.value == "0") getString(R.string.threads_auto_summary, auto) else p.entry
                }

            findPreference<Preference>(Prefs.KEY_VERSION)?.summary = BuildConfig.VERSION_NAME
            findPreference<Preference>(Prefs.KEY_LICENSES)?.setOnPreferenceClickListener {
                MaterialAlertDialogBuilder(ctx)
                    .setTitle(R.string.pref_licenses)
                    .setMessage(R.string.licenses_body)
                    .setPositiveButton(R.string.ok, null)
                    .show()
                true
            }
        }

        /**
         * Resolution and frame-rate lists come from the camera itself (Camera2 stream configuration
         * and AE fps ranges); the static arrays in arrays.xml are only the fallback when the camera
         * cannot be queried.
         */
        private fun setupCameraPrefs(caps: CameraCaps.Caps?) {
            findPreference<ListPreference>(Prefs.KEY_RESOLUTION)?.let { res ->
                val sizes = caps?.yuvSizes.orEmpty().filter { minOf(it.width, it.height) >= MIN_SHORT_SIDE }
                if (sizes.isNotEmpty()) {
                    res.entries = sizes.map {
                        val ratio = Tuning.ratioLabel(it.width, it.height)
                        if (it.maxFps > 0) getString(R.string.resolution_entry, it.width, it.height, ratio, it.maxFps)
                        else getString(R.string.resolution_entry_nofps, it.width, it.height, ratio)
                    }.toTypedArray()
                    res.entryValues = sizes.map { Prefs.resolutionValue(it.width, it.height) }.toTypedArray()
                }
                // Legacy "1080"-style value, or a size this camera doesn't have: use the closest one.
                val cur = Prefs.parseResolution(res.value)
                val exact = Prefs.resolutionValue(cur.width, cur.height)
                val values = res.entryValues.map { it.toString() }
                val pick = values.firstOrNull { it == exact } ?: values.minByOrNull { v ->
                    val s = Prefs.parseResolution(v)
                    abs(s.width.toLong() * s.height - cur.width.toLong() * cur.height)
                }
                if (pick != null && pick != res.value) res.value = pick
            }
            findPreference<ListPreference>(Prefs.KEY_FPS)?.let { fps ->
                val choices = Tuning.fpsChoices(caps?.fpsRanges.orEmpty())
                if (choices.isNotEmpty()) {
                    fps.entries = (listOf(getString(R.string.fps_auto)) + choices.map { "$it fps" }).toTypedArray()
                    fps.entryValues = (listOf("0") + choices.map { it.toString() }).toTypedArray()
                }
                if (fps.findIndexOfValue(fps.value) < 0) fps.value = "0"
            }
        }

        override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
            super.onViewCreated(view, savedInstanceState)
            // Let the last item scroll above the navigation bar.
            listView.clipToPadding = false
            ViewCompat.setOnApplyWindowInsetsListener(listView) { v, insets ->
                v.updatePadding(bottom = insets.getInsets(WindowInsetsCompat.Type.systemBars()).bottom)
                insets
            }
        }

        private companion object {
            /** Smaller analysis frames can't resolve the modules of a large code on screen. */
            const val MIN_SHORT_SIDE = 480
        }
    }
}
