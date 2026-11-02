package com.hanxi.ghosterm

import android.content.Context
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.PreparedSSH
import com.hanxi.ghosterm.data.RememberedSSHIdentities
import com.hanxi.ghosterm.data.StoredIdentity
import org.junit.Assert.*
import org.junit.Test
import java.util.UUID

class RememberedSSHIdentityTest {
    @Test fun rememberedChoiceIsDurableScopedAndNeverMatchesAReplacementByName() {
        val context=InstrumentationRegistry.getInstrumentation().targetContext
        val name="remember-test-${UUID.randomUUID()}"
        val prefs=context.getSharedPreferences(name,Context.MODE_PRIVATE)
        try {
            val target=PreparedSSH("server","example.test","alice",22,emptyList())
            val id=UUID.randomUUID().toString()
            val key=StoredIdentity(id,"same name",false)
            val store=RememberedSSHIdentities(prefs)
            assertNull(store.find(target,"alice",listOf(key)))
            store.record(target,"alice",id,true)
            val reopened=RememberedSSHIdentities(context.getSharedPreferences(name,Context.MODE_PRIVATE))
            assertEquals(id,reopened.find(target,"alice",listOf(key))?.id)
            assertTrue("Successful choice must reach disk, not just a cached preference",
                java.io.File(context.applicationInfo.dataDir,"shared_prefs/$name.xml").readText().contains(id))
            for(other in listOf(target.copy(alias="other"),target.copy(host="other.test"),target.copy(port=2222),
                target.copy(identityFiles=listOf("none")))) assertNull(reopened.find(other,"alice",listOf(key)))
            assertNull(reopened.find(target,"bob",listOf(key)))
            assertNull(reopened.find(target,"alice",listOf(key.copy(id=UUID.randomUUID().toString()))))
            assertEquals(id,reopened.find(target,"alice",listOf(key.copy(needsPassphrase=true)))?.id)
            assertEquals("Only an identity UUID is persisted",listOf(id),prefs.all.values.toList())
            reopened.record(target,"alice",id,false)
            assertNull(store.find(target,"alice",listOf(key)))
            assertTrue(prefs.all.isEmpty())
        } finally { context.deleteSharedPreferences(name) }
    }
}
