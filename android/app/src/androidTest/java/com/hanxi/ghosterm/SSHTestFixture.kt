package com.hanxi.ghosterm

import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import java.net.Socket

/** The isolated runner serves ephemeral material in memory; never write PEM to device files. */
internal fun sshFixture(): JSONObject {
    val args = InstrumentationRegistry.getArguments()
    val port = requireNotNull(args.getString("fixturePort")) { "Run scripts/test-android-ssh.sh" }.toInt()
    return Socket("127.0.0.1",port).use { socket ->
        socket.soTimeout = 5000
        socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray())
        JSONObject(socket.getInputStream().bufferedReader().readLine())
    }
}
