package com.codetrans

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import androidx.activity.result.contract.ActivityResultContract
import androidx.annotation.WorkerThread
import java.io.File

/** What the Save button writes, and where the user is asked to put it. Pure, JVM-testable. */
sealed interface SavePlan {
    /**
     * One document: the system file picker lets the user choose folder and name (ACTION_CREATE_DOCUMENT).
     * The bytes come from [file], or from [text] when the result has no file (ordinary QR code).
     */
    data class One(val name: String, val mime: String, val file: File?, val text: String?) : SavePlan

    /** Several files: the system picker lets the user choose a folder to put them all into. */
    data class Many(val files: List<ReceivedFile>) : SavePlan

    data object None : SavePlan
}

object SaveRules {
    private const val TEXT_NAME = "text.txt"

    fun plan(r: ReceivedResult?): SavePlan {
        if (r == null) return SavePlan.None
        return when (r.kind) {
            // The file holds the complete text; r.text may be a prefix of it.
            ResultKind.TEXT -> {
                val f = r.files.firstOrNull()
                when {
                    f != null -> SavePlan.One(f.name, "text/plain", f.file, null)
                    r.text != null -> SavePlan.One(TEXT_NAME, "text/plain", null, r.text)
                    else -> SavePlan.None
                }
            }
            ResultKind.IMAGE, ResultKind.FILES -> when (r.files.size) {
                0 -> SavePlan.None
                1 -> r.files[0].let { SavePlan.One(it.name, it.mime, it.file, null) }
                else -> SavePlan.Many(r.files)
            }
        }
    }
}

/**
 * [Intent.ACTION_CREATE_DOCUMENT] with the MIME type and suggested name of each launch
 * ([androidx.activity.result.contract.ActivityResultContracts.CreateDocument] fixes the type
 * when the launcher is registered). Result: the Uri of the new document, null if cancelled.
 */
class CreateTypedDocument : ActivityResultContract<SavePlan.One, Uri?>() {
    override fun createIntent(context: Context, input: SavePlan.One): Intent =
        Intent(Intent.ACTION_CREATE_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType(input.mime)
            .putExtra(Intent.EXTRA_TITLE, input.name)

    override fun parseResult(resultCode: Int, intent: Intent?): Uri? =
        if (resultCode == Activity.RESULT_OK) intent?.data else null
}

/** Writes received content to places the user picked through the Storage Access Framework. */
object SaveHelper {
    private const val TAG = "CodeTrans"
    private const val BUFFER = 64 * 1024

    /** Fills the document [uri] (just created by the picker); an empty leftover is removed on failure. */
    @WorkerThread
    fun write(context: Context, uri: Uri, plan: SavePlan.One): Boolean {
        val resolver = context.contentResolver
        return try {
            // "wt": truncate, in case the user picked an existing, longer file to overwrite.
            val out = resolver.openOutputStream(uri, "wt") ?: error("no output stream")
            out.use {
                when {
                    plan.file != null -> plan.file.inputStream().use { input -> input.copyTo(it, BUFFER) }
                    plan.text != null -> it.write(plan.text.toByteArray(Charsets.UTF_8))
                    else -> error("nothing to save")
                }
            }
            true
        } catch (e: Exception) {
            Log.w(TAG, "save failed", e)
            runCatching { DocumentsContract.deleteDocument(resolver, uri) }
            false
        }
    }

    /** Writes every file into the folder [tree] (from ACTION_OPEN_DOCUMENT_TREE). Returns how many were saved. */
    @WorkerThread
    fun writeAll(context: Context, tree: Uri, files: List<ReceivedFile>): Int {
        val resolver = context.contentResolver
        val parent = runCatching {
            DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        }.getOrNull() ?: return 0
        var saved = 0
        for (f in files) {
            // The provider renames on a clash ("name (1).ext"), so nothing is overwritten.
            val doc = runCatching { DocumentsContract.createDocument(resolver, parent, f.mime, f.name) }
                .onFailure { Log.w(TAG, "cannot create document", it) }
                .getOrNull() ?: continue
            try {
                val out = resolver.openOutputStream(doc, "wt") ?: error("no output stream")
                out.use { o -> f.file.inputStream().use { it.copyTo(o, BUFFER) } }
                saved++
            } catch (e: Exception) {
                Log.w(TAG, "save failed", e)
                runCatching { DocumentsContract.deleteDocument(resolver, doc) }
            }
        }
        return saved
    }
}
