package com.bachatas4.android.data

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class PkgKeyStoreTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    private lateinit var dir: File
    private lateinit var store: PkgKeyStore

    @Before
    fun setUp() {
        dir = temporaryFolder.root
        store = PkgKeyStore(dir)
    }

    @Test
    fun putAndGetRoundTrip() {
        assertNull(store.getPasscode("EP0001-CUSA00000_00-TEST000000000000"))
        store.putPasscode("EP0001-CUSA00000_00-TEST000000000000", "0123456789abcdef0123456789abcdef")
        assertEquals(
            "0123456789abcdef0123456789abcdef",
            store.getPasscode("EP0001-CUSA00000_00-TEST000000000000"),
        )
        val reloaded = PkgKeyStore(dir)
        assertEquals(
            "0123456789abcdef0123456789abcdef",
            reloaded.getPasscode("EP0001-CUSA00000_00-TEST000000000000"),
        )
    }

    @Test
    fun clearWipesFile() {
        store.putPasscode("EP0001-CUSA00000_00-TEST000000000000", "0123456789abcdef0123456789abcdef")
        store.clear()
        assertNull(store.getPasscode("EP0001-CUSA00000_00-TEST000000000000"))
        assertFalse(File(dir, "pkg_keydb.json").exists())
    }
}
