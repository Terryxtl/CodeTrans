package com.codetrans

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction

/** Decodes the raw bytes of an ordinary QR code: strict UTF-8 first, then GB18030. */
object PlainText {
    private val GB18030: Charset? = runCatching { Charset.forName("GB18030") }.getOrNull()

    fun decode(bytes: ByteArray): String {
        strict(bytes, Charsets.UTF_8)?.let { return it.removePrefix(0xFEFF.toChar().toString()) }
        GB18030?.let { cs -> strict(bytes, cs)?.let { return it } }
        return String(bytes, Charsets.UTF_8) // lenient: U+FFFD for bad sequences
    }

    private fun strict(bytes: ByteArray, cs: Charset): String? = try {
        cs.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes))
            .toString()
    } catch (_: CharacterCodingException) {
        null
    }
}
