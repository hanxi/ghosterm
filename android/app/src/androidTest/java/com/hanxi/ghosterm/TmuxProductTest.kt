package com.hanxi.ghosterm

import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.data.IdentityVault
import com.hanxi.ghosterm.data.StoredIdentity
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.util.UUID

/** Production Activity, credential dialog, Core tmux and the actual Surface. */
class TmuxProductTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val fixture by lazy { sshFixture() }
    private val tmux get()=fixture.getJSONObject("tmux")
    private fun model()=ViewModelProvider(ui.activity)[TmuxViewModel::class.java]
    private fun sessions()=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun hosts()=ViewModelProvider(ui.activity)[HostsViewModel::class.java]
    private lateinit var config: File
    private lateinit var known: File
    private lateinit var vault: IdentityVault
    private lateinit var identity: StoredIdentity
    private var oldConfig: ByteArray?=null
    private var oldKnown: ByteArray?=null
    private val alias="tmux-ui-"+UUID.randomUUID().toString().take(8)
    @Before fun setup() {
        config=File(ui.activity.filesDir,"home/.ssh/config")
        known=File(ui.activity.filesDir,"home/.ssh/known_hosts")
        oldConfig=config.takeIf { it.exists() }?.readBytes();oldKnown=known.takeIf { it.exists() }?.readBytes()
        known.parentFile!!.mkdirs()
        known.writeText("[127.0.0.1]:"+fixture.getInt("port")+" "+fixture.getString("public"))
        config.writeText("Host "+alias+"\n HostName 127.0.0.1\n User "+fixture.getString("user")+"\n Port "+fixture.getInt("port")+"\n")
        vault=IdentityVault(File(ui.activity.filesDir,"identities"))
        identity=vault.import(alias,tmux.getString("pem").toByteArray())
        ui.runOnIdle { hosts().reload() }
        ui.waitUntil(15000) { !hosts().busy && hosts().snapshot?.hosts?.any { it.alias==alias }==true }
    }
    private fun owners(type: String): Int {
        val cls=Class.forName("com.hanxi.ghosterm.bridge."+type)
        return cls.getDeclaredMethod("ownerCount").invoke(cls.getField("INSTANCE").get(null)) as Int
    }
    @After fun cleanup() {
        ui.runOnIdle {
            model().leave()
            sessions().connecting?.let { sessions().cancelConnection(it.id) }
            sessions().sessions.map { it.id }.forEach(sessions()::close)
        }
        ui.waitUntil(15000) { owners("NativeTmux")==0 && owners("NativeSession")==0 }
        if(::identity.isInitialized)vault.remove(identity)
        if(::config.isInitialized) {
            oldConfig?.let(config::writeBytes) ?: config.delete()
            oldKnown?.let(known::writeBytes) ?: known.delete()
        }
    }
    private fun login() {
        ui.waitUntil(15000) {
            val attempt=sessions().connecting
            assertFalse(attempt?.message,attempt?.failed==true)
            attempt?.auth?.let { it.phase==SSHAuthPhase.CREDENTIALS && it.request>0 }==true
        }
        ui.onNodeWithTag("ssh-key-"+identity.id).performScrollTo().performClick()
        // Lifecycle and unknown-outcome checks intentionally require fresh manual authentication.
        ui.onNodeWithTag("ssh-remember-identity").performScrollTo().performClick()
        ui.onNodeWithText("认证并连接").performClick()
        ui.waitUntil(15000) { sessions().connecting==null }
    }
    private fun open() {
        ui.onNodeWithTag("host-menu:"+alias).performScrollTo().performClick();ui.onNodeWithTag("host-background-"+alias).performScrollTo().performClick()
        login()
        ui.waitUntil(15000) { !model().busy && model().selectedPath!=null && model().server!=null }
        assertNull(model().error)
        ui.onNodeWithTag("tmux-page").assertIsDisplayed()
    }
    private fun create(name: String) {
        ui.onNodeWithTag("tmux-page").performScrollToNode(hasTestTag("tmux-create"))
        ui.onNodeWithTag("tmux-create").performClick()
        ui.onNodeWithTag("tmux-name").performTextReplacement(name)
        ui.onNodeWithTag("tmux-name-confirm").performClick()
    }
    private fun row(name: String) = model().entries.single { it.name==name }
    private fun showRow(name: String) {
        val tag="tmux-entry-"+row(name).id
        ui.onNodeWithTag("tmux-page").performScrollToNode(hasTestTag(tag))
    }
    private fun terminal(view: View): TerminalView? {
        if(view is TerminalView)return view
        if(view is ViewGroup)for(i in 0 until view.childCount)terminal(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun end(name: String) {
        showRow(name)
        ui.onNode(hasText("结束会话") and hasAnyAncestor(hasTestTag("tmux-entry-"+row(name).id))).performClick()
        ui.onNodeWithTag("tmux-terminate-confirm").performClick()
        ui.waitUntil(15000) { !model().busy && model().entries.none { it.name==name } }
        assertNull(model().error)
    }
    @Test fun tappingRealTmuxStatusSwitchesWindowsWithoutOpeningTheKeyboard() {
        open()
        val name = "mouse-" + UUID.randomUUID().toString().take(8)
        create(name)
        ui.waitUntil(15000) { !model().busy && model().entries.any { it.name == name } }
        showRow(name); ui.onNodeWithTag("tmux-attach-" + row(name).id).performClick(); login()
        ui.waitUntil(15000) { sessions().visible && sessions().active?.state?.connected == true }
        ui.waitUntil(10000) { terminal(ui.activity.window.decorView)?.currentFrame != null }
        val owner = sessions().active!!.connection
        ui.runOnIdle {
            val t = terminal(ui.activity.window.decorView)!!
            t.hideKeyboard()
            t.commitText("tmux set-option -t '$name' mouse on; tmux set-option -t '$name' status-left ''; tmux set-option -t '$name' status-right ''; tmux set-option -t '$name' status-justify left; tmux set-window-option automatic-rename off; tmux rename-window ONE; tmux set-window-option window-status-format ' #W '; tmux set-window-option window-status-current-format '[#W]'; tmux new-window -d -t '$name' -n TWO \"printf '\\\\033[2J\\\\033[HTWO_BODY'; exec sleep 600\"; printf '\\033[2J\\033[HONE_BODY\\r\\n'\r")
        }
        ui.waitUntil(10000) {
            terminal(ui.activity.window.decorView)?.let { t ->
                t.currentFrame?.let { f -> f.line(0).contains("ONE_BODY") && f.line(f.rows - 1).contains("TWO") } == true &&
                    androidx.core.view.ViewCompat.getRootWindowInsets(t)?.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime()) == false
            } == true
        }
        val coordinates = androidx.test.espresso.action.CoordinatesProvider { view ->
            val t = view as TerminalView; val f = t.currentFrame!!
            val column = f.line(f.rows - 1).indexOf("TWO")
            assertTrue(column >= 0)
            val location = IntArray(2); t.getLocationOnScreen(location)
            floatArrayOf(location[0] + (column + 1.5f) * (t.width / f.cols), location[1] + (f.rows - .5f) * (t.height / f.rows))
        }
        androidx.test.espresso.Espresso.onView(androidx.test.espresso.matcher.ViewMatchers.isAssignableFrom(TerminalView::class.java))
            .perform(androidx.test.espresso.action.GeneralClickAction(androidx.test.espresso.action.Tap.SINGLE, coordinates, androidx.test.espresso.action.Press.FINGER))
        ui.waitUntil(10000) { terminal(ui.activity.window.decorView)?.currentFrame?.line(0)?.contains("TWO_BODY") == true }
        ui.runOnIdle {
            assertSame(owner, sessions().active!!.connection)
            assertTrue(sessions().active!!.state.connected)
            assertFalse(androidx.core.view.ViewCompat.getRootWindowInsets(terminal(ui.activity.window.decorView)!!)!!
                .isVisible(androidx.core.view.WindowInsetsCompat.Type.ime()))
        }
        ui.onNodeWithTag("terminal-menu").performClick(); ui.onNodeWithText("关闭会话").performClick(); ui.onNodeWithText("关闭会话").performClick()
        ui.waitUntil(10000) { !sessions().visible }
        ui.onNodeWithTag("tmux-page").performScrollToNode(hasText("刷新列表"))
        ui.onNodeWithText("刷新列表").performClick()
        ui.waitUntil(15000) { !model().busy && model().entries.any { it.name == name } }
        end(name)
    }
    @Test fun realPageManagesRotatesAttachesAndClosingOnlyDisconnectsTheClient() {
        open()
        val name="ui-"+UUID.randomUUID().toString().take(8)
        create(name)
        ui.waitUntil(15000) { !model().busy && model().entries.any { it.name==name } }
        val before=row(name);val ownerCount=owners("NativeTmux")
        ui.activityRule.scenario.recreate()
        ui.waitUntil(15000) { model().visible && model().entries.any { it.name==name } }
        ui.runOnIdle { assertSame(before,row(name));assertEquals(ownerCount,owners("NativeTmux")) }
        showRow(name);ui.onNode(hasText("重命名") and hasAnyAncestor(hasTestTag("tmux-entry-"+row(name).id))).performClick()
        ui.onNodeWithTag("tmux-name").performTextReplacement(name+"-renamed")
        ui.onNodeWithTag("tmux-name-confirm").performClick()
        val renamed=name+"-renamed"
        ui.waitUntil(15000) { !model().busy && model().entries.any { it.name==renamed } }
        showRow(renamed)
        ui.onNodeWithTag("tmux-attach-"+row(renamed).id).performClick()
        login()
        ui.waitUntil(15000) { sessions().visible && sessions().active?.state?.connected==true }
        assertEquals(renamed,sessions().active!!.backgroundSession)
        ui.waitUntil(10000) { terminal(ui.activity.window.decorView)?.currentFrame!=null }
        ui.runOnIdle { terminal(ui.activity.window.decorView)!!.commitText("printf '\\033[2J\\033[HTMUX_UI_READY\\r\\n'\r") }
        ui.waitUntil(10000) { terminal(ui.activity.window.decorView)?.currentFrame?.line(0)=="TMUX_UI_READY" }
        ui.onNodeWithTag("terminal-menu").performClick();ui.onNodeWithText("关闭会话").performClick()
        ui.onNodeWithText("仅断开当前 tmux 客户端",substring=true).assertIsDisplayed()
        ui.onNodeWithText("关闭会话").performClick()
        ui.waitUntil(10000) { !sessions().visible }
        ui.onNodeWithTag("tmux-page").performScrollToNode(hasText("刷新列表"))
        ui.onNodeWithText("刷新列表").performClick()
        ui.waitUntil(15000) { !model().busy && model().entries.any { it.name==renamed } }
        showRow(renamed);ui.onNode(hasText("结束会话") and hasAnyAncestor(hasTestTag("tmux-entry-"+row(renamed).id))).performClick()
        ui.onNodeWithText("取消").performClick()
        assertTrue(model().entries.any { it.name==renamed })
        end(renamed)
    }
    @Test fun normalHostEntryCreatesAnOrdinaryTerminalInsteadOfReusingABackgroundClient() {
        open()
        val name="ordinary-"+UUID.randomUUID().toString().take(8)
        create(name)
        ui.waitUntil(15000) { !model().busy && model().entries.any { it.name==name } }
        showRow(name)
        ui.onNodeWithTag("tmux-attach-"+row(name).id).performClick();login()
        ui.waitUntil(15000) { sessions().visible && sessions().active?.state?.connected==true }
        val background=sessions().active!!.id
        ui.runOnIdle { sessions().home();model().leave() }
        ui.onNodeWithText(alias).performClick()
        ui.waitUntil(15000) { sessions().connecting!=null || sessions().visible }
        assertNotNull("normal entry must start its own SSH attempt",sessions().connecting)
        login()
        ui.waitUntil(15000) { sessions().visible && sessions().active?.state?.connected==true }
        ui.runOnIdle {
            assertNull(sessions().active!!.backgroundSession)
            assertNotEquals(background,sessions().active!!.id)
            assertTrue(sessions().sessions.any { it.id==background && it.state.connected })
        }
        val firstOrdinary=sessions().active!!.id
        ui.runOnIdle { sessions().selectNewRemote() }
        ui.onNodeWithText(alias).performClick();login()
        ui.waitUntil(15000) { sessions().visible && sessions().active?.state?.connected==true && sessions().active!!.id!=firstOrdinary }
        val secondOrdinary=sessions().active!!.id
        ui.runOnIdle { sessions().home() }
        ui.onNodeWithText(alias).performClick()
        ui.waitUntil(15000) { sessions().choosingRemote==alias }
        ui.onNodeWithTag("ssh-session-"+background).assertDoesNotExist()
        ui.onNodeWithTag("ssh-session-"+firstOrdinary).assertIsDisplayed()
        ui.onNodeWithTag("ssh-session-"+secondOrdinary).assertIsDisplayed()
    }
    @Test fun unknownCreateIsVisibleAndReconnectionDoesNotReplayIt() {
        vault.remove(identity);identity=vault.import(alias+"-delay",tmux.getString("delayPem").toByteArray())
        open()
        val name="uncertain-"+UUID.randomUUID().toString().take(8)
        val attempts=sshFixture().getJSONObject("tmux").getInt("createAttempts")
        val native=Class.forName("com.hanxi.ghosterm.bridge.NativeTmux")
        fun requests()=native.getDeclaredMethod("createRequestCount").invoke(native.getField("INSTANCE").get(null)) as Long
        val enqueued=requests()
        create(name)
        ui.waitUntil(20000) { !model().busy && model().error!=null }
        assertTrue("unknown outcome must not be presented as a refusal",model().error!!.contains("结果无法确认"))
        // A timed-out SSH channel may reject a replay before the server sees it.
        // Observe actual owner admission as well as the independent server log.
        assertEquals("unknown create must not enqueue an automatic retry",enqueued+1,requests())
        ui.onNodeWithTag("tmux-page").performScrollToNode(hasTestTag("tmux-error"))
        ui.onNodeWithTag("tmux-error").assertTextContains("不会自动重试",substring=true)
        ui.onNodeWithTag("tmux-create").assertIsNotEnabled()
        ui.onNodeWithText("重新连接").performClick()
        login()
        ui.waitUntil(15000) { !model().busy && model().entries.any { it.name==name } }
        assertEquals(1,model().entries.count { it.name==name })
        assertEquals("unknown create must not be replayed",attempts+1,sshFixture().getJSONObject("tmux").getInt("createAttempts"))
        end(name)
    }
}
