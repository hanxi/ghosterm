package com.hanxi.ghosterm

import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import java.net.Socket

/** The isolated runner serves ephemeral material in memory; never write PEM to device files. */
internal fun sshFixture(): JSONObject = JSONObject(sshFixtureRequest())

internal fun resetSSHFixtureConnection() {
    check(sshFixtureRequest(":reset") == "OK") { "Fixture did not reset an active SSH link" }
}

internal fun pauseSSHFixtureConnection() {
    check(sshFixtureRequest(":blackhole") == "OK") { "Fixture did not pause an active SSH link" }
}

private fun sshFixtureRequest(suffix: String = ""): String {
    val args = InstrumentationRegistry.getArguments()
    val port = requireNotNull(args.getString("fixturePort")) { "Run scripts/test-android-ssh.sh" }.toInt()
    return Socket("127.0.0.1",port).use { socket ->
        socket.soTimeout = 5000
        socket.getOutputStream().write((args.getString("fixtureToken") + suffix + "\n").toByteArray())
        checkNotNull(socket.getInputStream().bufferedReader().readLine())
    }
}
