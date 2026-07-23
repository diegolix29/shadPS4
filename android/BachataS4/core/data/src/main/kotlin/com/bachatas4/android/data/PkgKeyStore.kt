package com.bachatas4.android.data

import java.io.File

/**
 * Minimal passcode vault for PKG import (content_id → 32-char passcode).
 * File: [filesDir]/pkg_keydb.json
 *
 * Uses a tiny hand-rolled JSON subset so unit tests do not need org.json mocks
 * or the kotlinx.serialization compiler plugin.
 */
class PkgKeyStore(
    private val filesDir: File,
) {
    private val lock = Any()
    private val file: File get() = File(filesDir, FILE_NAME)

    fun getPasscode(contentId: String): String? {
        val id = contentId.trim()
        if (id.isEmpty()) return null
        synchronized(lock) {
            return loadPasscodes()[id]
        }
    }

    fun putPasscode(contentId: String, passcode: String) {
        val id = contentId.trim()
        require(id.isNotEmpty()) { "contentId must not be empty" }
        val code = passcode.trim()
        require(code.isNotEmpty()) { "passcode must not be empty" }
        synchronized(lock) {
            val passcodes = loadPasscodes().toMutableMap()
            passcodes[id] = code
            savePasscodes(passcodes)
        }
    }

    fun clear() {
        synchronized(lock) {
            val target = file
            if (target.exists()) {
                target.delete()
            }
        }
    }

    private fun loadPasscodes(): Map<String, String> {
        val target = file
        if (!target.isFile) return emptyMap()
        val text = runCatching { target.readText() }.getOrNull() ?: return emptyMap()
        return parsePasscodes(text)
    }

    private fun savePasscodes(passcodes: Map<String, String>) {
        filesDir.mkdirs()
        val temp = File(filesDir, "$FILE_NAME.tmp")
        temp.writeText(encodePasscodes(passcodes))
        if (!temp.renameTo(file)) {
            file.delete()
            if (!temp.renameTo(file)) {
                temp.copyTo(file, overwrite = true)
                temp.delete()
            }
        }
    }

    companion object {
        const val FILE_NAME = "pkg_keydb.json"

        internal fun encodePasscodes(passcodes: Map<String, String>): String {
            val entries = passcodes.entries.joinToString(",") { (k, v) ->
                "\"${escape(k)}\":\"${escape(v)}\""
            }
            return "{\"passcodes\":{$entries}}"
        }

        /**
         * Parses `{"passcodes":{"id":"code",...}}` with minimal string scanning.
         * Malformed input yields an empty map.
         */
        internal fun parsePasscodes(text: String): Map<String, String> {
            val marker = "\"passcodes\""
            val markerIndex = text.indexOf(marker)
            if (markerIndex < 0) return emptyMap()
            val braceStart = text.indexOf('{', markerIndex + marker.length)
            if (braceStart < 0) return emptyMap()
            val body = extractObjectBody(text, braceStart) ?: return emptyMap()
            return parseStringMap(body)
        }

        private fun extractObjectBody(text: String, openBrace: Int): String? {
            var depth = 0
            var inString = false
            var escape = false
            for (i in openBrace until text.length) {
                val c = text[i]
                if (escape) {
                    escape = false
                    continue
                }
                if (c == '\\' && inString) {
                    escape = true
                    continue
                }
                if (c == '"') {
                    inString = !inString
                    continue
                }
                if (inString) continue
                when (c) {
                    '{' -> depth++
                    '}' -> {
                        depth--
                        if (depth == 0) {
                            return text.substring(openBrace + 1, i)
                        }
                    }
                }
            }
            return null
        }

        private fun parseStringMap(body: String): Map<String, String> {
            val result = linkedMapOf<String, String>()
            var i = 0
            while (i < body.length) {
                while (i < body.length && (body[i].isWhitespace() || body[i] == ',')) i++
                if (i >= body.length) break
                val key = readJsonString(body, i) ?: break
                i = key.second
                while (i < body.length && body[i].isWhitespace()) i++
                if (i >= body.length || body[i] != ':') break
                i++
                while (i < body.length && body[i].isWhitespace()) i++
                val value = readJsonString(body, i) ?: break
                i = value.second
                if (key.first.isNotEmpty() && value.first.isNotEmpty()) {
                    result[key.first] = value.first
                }
            }
            return result
        }

        private fun readJsonString(text: String, start: Int): Pair<String, Int>? {
            var i = start
            while (i < text.length && text[i].isWhitespace()) i++
            if (i >= text.length || text[i] != '"') return null
            i++
            val sb = StringBuilder()
            while (i < text.length) {
                val c = text[i]
                when {
                    c == '\\' && i + 1 < text.length -> {
                        val next = text[i + 1]
                        sb.append(
                            when (next) {
                                '"', '\\', '/' -> next
                                'n' -> '\n'
                                'r' -> '\r'
                                't' -> '\t'
                                else -> next
                            },
                        )
                        i += 2
                    }
                    c == '"' -> return sb.toString() to (i + 1)
                    else -> {
                        sb.append(c)
                        i++
                    }
                }
            }
            return null
        }

        private fun escape(value: String): String =
            buildString(value.length + 8) {
                value.forEach { c ->
                    when (c) {
                        '\\' -> append("\\\\")
                        '"' -> append("\\\"")
                        '\n' -> append("\\n")
                        '\r' -> append("\\r")
                        '\t' -> append("\\t")
                        else -> append(c)
                    }
                }
            }
    }
}
