plugins { alias(libs.plugins.android.library); alias(libs.plugins.kotlin.android) }
val pluginJsValue = providers.gradleProperty("ghostermPluginJs").orNull
require(pluginJsValue == null || pluginJsValue == "true" || pluginJsValue == "false") { "ghostermPluginJs must be explicit true/false" }
val pluginJsEnabled = pluginJsValue == "true"
android {
    namespace = "com.hanxi.ghosterm.bridge"
    compileSdk = 36
    ndkVersion = "28.2.13676358"
    defaultConfig {
        minSdk = 29
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { arguments += listOf("-DANDROID_STL=c++_static", "-DGHOSTERM_WITH_PLUGIN_JS=${if (pluginJsEnabled) "ON" else "OFF"}") } }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
}

dependencies { implementation(project(":terminal")); implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0") }
