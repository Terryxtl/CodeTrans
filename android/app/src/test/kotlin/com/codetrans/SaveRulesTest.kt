package com.codetrans

import com.google.common.truth.Truth.assertThat
import org.junit.Test
import java.io.File

class SaveRulesTest {
    private fun file(name: String, mime: String) = ReceivedFile(File("cache/rx/1/$name"), name, 10L, mime)

    private fun result(kind: ResultKind, files: List<ReceivedFile>, text: String? = null, plain: Boolean = false) =
        ReceivedResult(
            id = 1L, kind = kind, plain = plain, text = text, textBytes = text?.length?.toLong() ?: 0L,
            textTruncated = false, mime = "*/*", files = files, dir = null,
        )

    @Test
    fun nothingShownNothingToSave() {
        assertThat(SaveRules.plan(null)).isEqualTo(SavePlan.None)
        assertThat(SaveRules.plan(result(ResultKind.FILES, emptyList()))).isEqualTo(SavePlan.None)
        assertThat(SaveRules.plan(result(ResultKind.TEXT, emptyList(), text = null))).isEqualTo(SavePlan.None)
    }

    @Test
    fun textIsSavedFromItsFile() {
        val f = file("text.txt", "text/plain")
        val plan = SaveRules.plan(result(ResultKind.TEXT, listOf(f), text = "prefix of a long text"))
        assertThat(plan).isEqualTo(SavePlan.One("text.txt", "text/plain", f.file, null))
    }

    @Test
    fun plainQrTextHasNoFile() {
        val plan = SaveRules.plan(result(ResultKind.TEXT, emptyList(), text = "https://example.com", plain = true))
        assertThat(plan).isEqualTo(SavePlan.One("text.txt", "text/plain", null, "https://example.com"))
    }

    @Test
    fun oneFileKeepsNameAndMime() {
        val img = file("photo.png", "image/png")
        assertThat(SaveRules.plan(result(ResultKind.IMAGE, listOf(img))))
            .isEqualTo(SavePlan.One("photo.png", "image/png", img.file, null))
        val pdf = file("报告.pdf", "application/pdf")
        assertThat(SaveRules.plan(result(ResultKind.FILES, listOf(pdf))))
            .isEqualTo(SavePlan.One("报告.pdf", "application/pdf", pdf.file, null))
    }

    @Test
    fun severalFilesGoToAFolder() {
        val files = listOf(file("a.txt", "text/plain"), file("b.bin", "application/octet-stream"))
        assertThat(SaveRules.plan(result(ResultKind.FILES, files))).isEqualTo(SavePlan.Many(files))
    }
}
