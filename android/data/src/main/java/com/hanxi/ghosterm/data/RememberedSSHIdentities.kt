package com.hanxi.ghosterm.data

import android.content.SharedPreferences
import com.hanxi.ghosterm.bridge.PreparedSSH
import org.json.JSONArray
import java.security.MessageDigest

/** Authentication preference only: UUIDs, never key material or passphrases. */
class RememberedSSHIdentities(private val preferences: SharedPreferences) {
    private fun key(target: PreparedSSH, username: String): String {
        // Include the evaluated identity chain: editing SSH identities invalidates the old choice.
        val scope = JSONArray().put(target.alias).put(target.host).put(target.port).put(username)
            .put(JSONArray(target.identityFiles)).toString().toByteArray(Charsets.UTF_8)
        return "identity-" + MessageDigest.getInstance("SHA-256").digest(scope)
            .joinToString("") { "%02x".format(it) }
    }

    fun find(target: PreparedSSH, username: String, available: List<StoredIdentity>): StoredIdentity? {
        val id = preferences.getString(key(target, username), null) ?: return null
        return available.firstOrNull { it.id == id }
    }

    /** Caller must have observed successful authentication for this exact connection. */
    fun record(target: PreparedSSH, username: String, identityId: String, remember: Boolean) {
        IdentityReferences.encode(identityId) // Refuse malformed identifiers, including names/paths.
        val editor = preferences.edit()
        if (remember) editor.putString(key(target, username), identityId)
        else editor.remove(key(target, username))
        check(editor.commit()) { "无法保存上次使用的 SSH 身份" }
    }
}
