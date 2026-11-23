import java.io.File

plugins { alias(libs.plugins.android.library); alias(libs.plugins.kotlin.android) }
val pluginLuaValue = providers.gradleProperty("ghostermPluginLua").orNull
require(pluginLuaValue == null || pluginLuaValue == "true" || pluginLuaValue == "false") {
    "ghostermPluginLua must be explicit true/false"
}
val pluginLuaEnabled = pluginLuaValue == "true"
val luaArchive = providers.gradleProperty("ghostermLuaArchive").orNull
require(!pluginLuaEnabled || (luaArchive != null && File(luaArchive).isAbsolute && file(luaArchive).isFile)) {
    "ghostermLuaArchive must point to the pinned Lua 5.5.1 archive"
}
android {
    namespace = "com.hanxi.ghosterm.bridge"
    compileSdk = 36
    ndkVersion = "28.2.13676358"
    defaultConfig {
        minSdk = 29
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DGHOSTERM_WITH_PLUGIN_LUA=${if (pluginLuaEnabled) "ON" else "OFF"}",
                )
                if (pluginLuaEnabled) arguments += "-DGHOSTERM_LUA_ARCHIVE=$luaArchive"
            }
        }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}

dependencies { implementation(project(":terminal")); implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0") }
