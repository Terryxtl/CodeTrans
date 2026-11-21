package com.codetrans

import android.content.ContentValues
import android.content.Context
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import android.webkit.MimeTypeMap
import androidx.annotation.RequiresApi
import androidx.annotation.WorkerThread
import java.io.File
import java.util.concurrent.atomic.AtomicLong

/**
 * Holds the single current result (PLAN §10). Results live in <cacheDir>/rx/<session>/;
 * the previous result's directory is deleted when a new one arrives.
 */
class ResultStore(private val context: Context) {
    val rxRoot: File = rxRoot(context)
    private val ids = AtomicLong(0)

    @Volatile
    var current: ReceivedResult? = null
        private set

    /** Maps a native result, validating that every path lies inside cache/rx/. */
    @WorkerThread
    @Synchronized
    fun accept(n: NativeResult): ReceivedResult? {
        val root = rxRoot.canonicalFile
        val raw = ArrayList<Pair<File, Int>>()
        for (i in n.paths.indices) {
            val f = runCatching { File(n.paths[i]).canonicalFile }.getOrNull() ?: continue
            if (!f.path.startsWith(root.path + File.separator) || !f.isFile) {
                Log.w(TAG, "ignoring result path outside cache/rx")
                continue
            }
            raw += f to i
        }
        val kind = when (n.kind) {
            NativeCore.KIND_TEXT -> ResultKind.TEXT
            NativeCore.KIND_IMAGE -> ResultKind.IMAGE
            NativeCore.KIND_FILES -> ResultKind.FILES
            else -> return null
        }
        if (kind != ResultKind.TEXT && raw.isEmpty()) return null

        val names = FileNames.dedupe(raw.map { (f, i) -> FileNames.sanitize(n.names.getOrNull(i) ?: f.name) })
        val files = raw.mapIndexed { idx, (f, i) ->
            val size = n.sizes.getOrNull(i)?.takeIf { it >= 0 } ?: f.length()
            val mime = when {
                kind == ResultKind.TEXT -> "text/plain"
                kind == ResultKind.IMAGE && n.mime.startsWith("image/") -> n.mime
                else -> mimeFor(names[idx])
            }
            ReceivedFile(f, names[idx], size, mime)
        }
        val text = if (kind == ResultKind.TEXT) n.text.orEmpty() else null
        val dir = files.firstOrNull()?.file?.let { sessionDirOf(it, root) }
        val result = ReceivedResult(
            id = ids.incrementAndGet(),
            kind = kind,
            plain = false,
            text = text,
            textTruncated = kind == ResultKind.TEXT && n.textTruncated,
            textBytes = if (kind == ResultKind.TEXT) {
                files.firstOrNull()?.size ?: text!!.toByteArray().size.toLong()
            } else 0L,
            mime = if (kind == ResultKind.FILES) ShareRules.commonMime(files.map { it.mime }) else n.mime,
            files = files,
            dir = dir,
        )
        replace(result)
        return result
    }

    /** An ordinary QR code shown as text (no file on disk). */
    @Synchronized
    fun acceptPlain(text: String): ReceivedResult {
        val result = ReceivedResult(
            id = ids.incrementAndGet(),
            kind = ResultKind.TEXT,
            plain = true,
            text = text,
            textTruncated = false,
            textBytes = text.toByteArray().size.toLong(),
            mime = "text/plain",
            files = emptyList(),
            dir = null,
        )
        replace(result)
        return result
    }

    /** Debug / demo results written under cache/rx/demo-*. */
    @Synchronized
    fun acceptDemo(kind: ResultKind, files: List<ReceivedFile>, text: String?, dir: File): ReceivedResult {
        val result = ReceivedResult(
            id = ids.incrementAndGet(), kind = kind, plain = false, text = text, textTruncated = false,
            textBytes = text?.toByteArray()?.size?.toLong() ?: 0L,
            mime = ShareRules.commonMime(files.map { it.mime }), files = files, dir = dir,
        )
        replace(result)
        return result
    }

    private fun replace(new: ReceivedResult) {
        val old = current
        current = new
        val oldDir = old?.dir
        if (oldDir != null && oldDir != new.dir) {
            if (!oldDir.deleteRecursively()) Log.w(TAG, "could not delete previous result")
        }
    }

    /**
     * Copies IMAGE / FILES results to Download/CodeTrans via MediaStore (no storage permission).
     * Returns the number of files saved.
     */
    @RequiresApi(29)
    @WorkerThread
    fun saveToDownloads(r: ReceivedResult): Int {
        if (r.kind == ResultKind.TEXT) return 0
        val resolver = context.contentResolver
        var saved = 0
        for (f in r.files) {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, f.name)
                put(MediaStore.Downloads.MIME_TYPE, f.mime)
                put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/CodeTrans")
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = runCatching {
                resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            }.getOrNull() ?: continue
            try {
                resolver.openOutputStream(uri)?.use { out ->
                    f.file.inputStream().use { it.copyTo(out, 64 * 1024) }
                } ?: error("no output stream")
                values.clear()
                values.put(MediaStore.Downloads.IS_PENDING, 0)
                resolver.update(uri, values, null, null)
                saved++
            } catch (e: Exception) {
                Log.w(TAG, "auto-save failed", e)
                runCatching { resolver.delete(uri, null, null) }
            }
        }
        return saved
    }

    companion object {
        private const val TAG = "CodeTrans"

        fun rxRoot(context: Context) = File(context.cacheDir, "rx")

        /** Called once per process start: no history is kept across launches. */
        fun clearAll(context: Context) {
            val root = rxRoot(context)
            root.deleteRecursively()
            root.mkdirs()
        }

        /** <rx>/<session>/... -> <rx>/<session> */
        private fun sessionDirOf(f: File, root: File): File? {
            var cur: File? = f
            while (cur != null && cur.parentFile != root) cur = cur.parentFile
            return cur?.takeIf { it.isDirectory }
        }

        fun mimeFor(name: String): String {
            val ext = name.substringAfterLast('.', "").lowercase()
            if (ext.isEmpty()) return "application/octet-stream"
            return MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext) ?: "application/octet-stream"
        }
    }
}
