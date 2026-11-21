package com.codetrans

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.core.content.FileProvider
import java.io.File

/** What to put into the share Intent (PLAN §10 table). Pure, JVM-testable. */
sealed interface SharePlan {
    /** ACTION_SEND + EXTRA_TEXT. */
    data class Text(val text: String) : SharePlan

    /** ACTION_SEND (one file) or ACTION_SEND_MULTIPLE with content:// URIs. */
    data class Files(val files: List<ReceivedFile>, val mime: String) : SharePlan {
        val multiple: Boolean get() = files.size > 1
    }

    data object None : SharePlan
}

object ShareRules {
    /** Binder transactions top out around 1 MB; keep inline text (and clipboard) well below. */
    const val MAX_INLINE_TEXT_BYTES = 100L * 1024

    fun fitsInline(r: ReceivedResult): Boolean =
        r.kind == ResultKind.TEXT && r.text != null && !r.textTruncated && r.textBytes <= MAX_INLINE_TEXT_BYTES

    /**
     * @param textFile supplies a .txt file for large text that has none on disk (plain QR);
     *                 only called when needed.
     */
    fun plan(r: ReceivedResult?, textFile: (ReceivedResult) -> ReceivedFile? = { null }): SharePlan {
        if (r == null) return SharePlan.None
        return when (r.kind) {
            ResultKind.TEXT -> when {
                fitsInline(r) -> SharePlan.Text(r.text!!)
                else -> {
                    val f = r.files.firstOrNull() ?: textFile(r)
                    if (f != null) SharePlan.Files(listOf(f), "text/plain") else SharePlan.None
                }
            }
            ResultKind.IMAGE, ResultKind.FILES ->
                if (r.files.isEmpty()) SharePlan.None
                else SharePlan.Files(r.files, commonMime(r.files.map { it.mime }))
        }
    }

    /** Same MIME for all -> that MIME; otherwise wildcard. */
    fun commonMime(mimes: List<String>): String {
        if (mimes.isEmpty()) return "*/*"
        val first = mimes.first()
        return if (mimes.all { it.equals(first, ignoreCase = true) }) first else "*/*"
    }
}

/** Builds share Intents; always goes through the system chooser. */
object ShareHelper {
    private const val TAG = "CodeTrans"

    fun authority(context: Context) = "${context.packageName}.fileprovider"

    fun share(context: Context, result: ReceivedResult?): Boolean {
        val plan = ShareRules.plan(result) { writeTextFile(context, it) }
        val target = when (plan) {
            SharePlan.None -> return false
            is SharePlan.Text -> Intent(Intent.ACTION_SEND).apply {
                type = "text/plain"
                putExtra(Intent.EXTRA_TEXT, plan.text)
            }
            is SharePlan.Files -> filesIntent(context, plan) ?: return false
        }
        val chooser = Intent.createChooser(target, context.getString(R.string.share_title))
        return try {
            context.startActivity(chooser)
            true
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "no share target", e)
            false
        }
    }

    private fun filesIntent(context: Context, plan: SharePlan.Files): Intent? {
        val uris = plan.files.mapNotNull { f ->
            runCatching { FileProvider.getUriForFile(context, authority(context), f.file, f.name) }
                .onFailure { Log.w(TAG, "not shareable", it) }
                .getOrNull()?.let { it to f }
        }
        if (uris.isEmpty()) return null
        val clip = ClipData.newUri(context.contentResolver, uris[0].second.name, uris[0].first)
        for (i in 1 until uris.size) clip.addItem(ClipData.Item(uris[i].first))

        val intent = if (uris.size == 1) {
            Intent(Intent.ACTION_SEND).putExtra(Intent.EXTRA_STREAM, uris[0].first)
        } else {
            Intent(Intent.ACTION_SEND_MULTIPLE)
                .putParcelableArrayListExtra(Intent.EXTRA_STREAM, ArrayList<Uri>(uris.map { it.first }))
        }
        return intent.apply {
            type = plan.mime
            clipData = clip
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
    }

    /** Large plain text without a backing file: write cache/rx/share/text.txt. */
    private fun writeTextFile(context: Context, r: ReceivedResult): ReceivedFile? = runCatching {
        val dir = File(ResultStore.rxRoot(context), "share").apply { mkdirs() }
        val f = File(dir, "text.txt")
        f.writeText(r.text.orEmpty())
        ReceivedFile(f, "text.txt", f.length(), "text/plain")
    }.getOrNull()
}
