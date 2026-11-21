package com.codetrans

/** File names come from scanned codes and are untrusted (PLAN §10). */
object FileNames {
    const val MAX_LEN = 120
    private const val FALLBACK = "file"
    private val UNSAFE = setOf('/', '\\', ':', '*', '?', '"', '<', '>', '|')

    fun sanitize(name: String): String {
        val sb = StringBuilder(name.length)
        for (c in name) {
            when {
                c.code < 0x20 || c.code == 0x7F || c.code in 0x80..0x9F -> Unit // control chars
                c in UNSAFE -> sb.append('_')
                else -> sb.append(c)
            }
        }
        var s = sb.toString()
        while (s.contains("..")) s = s.replace("..", ".")
        s = s.trim().trim('.', ' ')
        if (s.isEmpty()) s = FALLBACK
        return truncate(s)
    }

    /** Keeps the extension when shortening to [MAX_LEN] UTF-16 units. */
    private fun truncate(s: String): String {
        if (s.length <= MAX_LEN) return s
        val dot = s.lastIndexOf('.')
        val ext = if (dot > 0 && s.length - dot <= 16) s.substring(dot) else ""
        var base = s.substring(0, MAX_LEN - ext.length)
        if (base.isNotEmpty() && base.last().isHighSurrogate()) base = base.dropLast(1)
        return base + ext
    }

    /** "a.txt", "a.txt" -> "a.txt", "a (1).txt" (case-insensitive). */
    fun dedupe(names: List<String>): List<String> {
        val used = HashSet<String>()
        return names.map { n ->
            var candidate = n
            var i = 1
            while (!used.add(candidate.lowercase())) {
                val dot = n.lastIndexOf('.')
                candidate = if (dot > 0) "${n.substring(0, dot)} ($i)${n.substring(dot)}" else "$n ($i)"
                i++
            }
            candidate
        }
    }
}
