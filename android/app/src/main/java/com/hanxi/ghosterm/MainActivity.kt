package com.hanxi.ghosterm

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { GhostermApp() }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun GhostermApp(model: RuntimeInfoViewModel = viewModel()) {
    var settings by rememberSaveable { mutableStateOf(false) }
    val runtime by model.state.collectAsState()
    BackHandler(settings) { settings = false }
    val colors = if (isSystemInDarkTheme()) darkColorScheme(primary = Color(0xFFAFC6FF))
        else lightColorScheme(primary = Color(0xFF365E9D), background = Color(0xFFF7F8FC))
    MaterialTheme(colorScheme = colors) {
        Scaffold(topBar = {
            TopAppBar(title = { Text(if (settings) "设置" else "主机") },
                navigationIcon = {
                    if (settings) TextButton(onClick = { settings = false }) { Text("返回") }
                }, actions = {
                    if (!settings) TextButton(onClick = { settings = true }) { Text("设置") }
                })
        }) { padding ->
            Column(Modifier.fillMaxSize().padding(padding).padding(24.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp)) {
                if (settings) {
                    Text("关于 Ghosterm", style = MaterialTheme.typography.headlineSmall)
                    Text("版本 ${BuildConfig.VERSION_NAME}")
                    when (val info = runtime) {
                        RuntimeInfo.Loading -> Text("正在加载终端组件…")
                        RuntimeInfo.Failed -> Text("终端组件加载失败", color = MaterialTheme.colorScheme.error)
                        is RuntimeInfo.Ready -> {
                            Text("Core ${info.core}")
                            Text("libghostty-vt ${info.terminal}")
                        }
                    }
                    Text("原生 Android 客户端 · MIT License")
                } else {
                    Spacer(Modifier.weight(1f))
                    Text("你的主机，从这里开始", style = MaterialTheme.typography.headlineSmall,
                        modifier = Modifier.align(Alignment.CenterHorizontally))
                    Text("当前为原生构建验证版本。主机管理、终端和文件功能正在接入。",
                        style = MaterialTheme.typography.bodyLarge)
                    Spacer(Modifier.weight(1f))
                }
            }
        }
    }
}
