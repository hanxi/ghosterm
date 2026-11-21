import java.net.URI
import java.util.Base64

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}
abstract class GenerateQuickJsLicense : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val license: RegularFileProperty

    @get:OutputDirectory
    abstract val assets: DirectoryProperty

    @TaskAction
    fun generate() {
        val output = assets.file("licenses/QuickJSNG-MIT.txt").get().asFile
        output.parentFile.mkdirs()
        license.get().asFile.copyTo(output, overwrite = true)
    }
}

val releaseStore = providers.environmentVariable("ANDROID_RELEASE_STORE_FILE")
val pluginJsEnabled = providers.gradleProperty("ghostermPluginJs").orNull == "true"
val channelProcessFixture = providers.gradleProperty("ghostermChannelProcessFixture").orNull == "true"
val pluginChannelName = providers.gradleProperty("ghostermPluginChannel").orNull.orEmpty()
val pluginChannelRoot = providers.gradleProperty("ghostermPluginChannelRoot").orNull.orEmpty()
val pluginChannelUrl = providers.gradleProperty("ghostermPluginChannelUrl").orNull.orEmpty()
if (listOf(pluginChannelName, pluginChannelRoot, pluginChannelUrl).any { it.isNotEmpty() }) {
    require(pluginChannelName.matches(Regex("[a-z0-9][a-z0-9.-]{0,63}"))) { "Invalid ghostermPluginChannel" }
    val root = Base64.getDecoder().decode(pluginChannelRoot)
    require(root.size == 32 && Base64.getEncoder().encodeToString(root) == pluginChannelRoot) {
        "Expected a 32-byte channel root in canonical Base64"
    }
    val endpoint = URI(pluginChannelUrl)
    require(endpoint.scheme == "https" && endpoint.host != null && endpoint.userInfo == null &&
        endpoint.rawFragment == null && endpoint.rawQuery == null &&
        !endpoint.rawAuthority.endsWith(":") &&
        (endpoint.port == -1 || endpoint.port in 1..65535)) { "Expected an HTTPS channel directory" }
}
val runtimeSdkDirectory = providers.gradleProperty("ghostermRuntimeSdk").orNull
fun sourceGit(vararg arguments: String): String? = try {
    val command = providers.exec {
        commandLine(listOf("git", "-C", rootProject.projectDir.parentFile.absolutePath) + arguments)
        isIgnoreExitValue = true
    }
    if (command.result.get().exitValue == 0) command.standardOutput.asText.get().trim() else null
} catch (_: Exception) { null }
val sourceCommit = sourceGit("rev-parse", "--verify", "HEAD")
    ?.takeIf { it.matches(Regex("[0-9a-f]{40}|[0-9a-f]{64}")) }
val sourceChanges = sourceGit("status", "--porcelain", "--untracked-files=normal")
val sourceState = when {
    sourceCommit == null || sourceChanges == null -> "unknown"
    sourceChanges.isEmpty() -> "clean"
    else -> "dirty"
}
val sourceAssets = layout.buildDirectory.dir("generated/source-metadata-assets")
val writeSourceMetadata = tasks.register("writeSourceMetadata") {
    inputs.property("gitCommit", sourceCommit ?: "unknown")
    inputs.property("sourceState", sourceState)
    outputs.dir(sourceAssets)
    doLast {
        val output = sourceAssets.get().file("ghosterm-build.json").asFile
        output.parentFile.mkdirs()
        val commit = sourceCommit?.let { "\"$it\"" } ?: "null"
        output.writeText("{\"schema\":1,\"git_commit\":$commit,\"source_state\":\"$sourceState\"}\n")
    }
}
android {
    namespace = "com.hanxi.ghosterm"
    compileSdk = 36
    ndkVersion = "28.2.13676358"
    defaultConfig {
        applicationId = "com.hanxi.ghosterm"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0-dev"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        buildConfigField("boolean", "PLUGIN_JS_ENABLED", pluginJsEnabled.toString())
        buildConfigField("String", "PLUGIN_CHANNEL_NAME", "\"$pluginChannelName\"")
        buildConfigField("String", "PLUGIN_CHANNEL_ROOT", "\"$pluginChannelRoot\"")
        buildConfigField("String", "PLUGIN_CHANNEL_URL", "\"$pluginChannelUrl\"")
        ndk { abiFilters += "arm64-v8a" }
    }
    buildFeatures { compose = true; buildConfig = true }
    signingConfigs {
        if (releaseStore.isPresent) {
            create("release") {
                storeFile = file(releaseStore.get())
                storePassword = providers.environmentVariable("ANDROID_RELEASE_STORE_PASSWORD").get()
                keyAlias = providers.environmentVariable("ANDROID_RELEASE_KEY_ALIAS").get()
                keyPassword = providers.environmentVariable("ANDROID_RELEASE_KEY_PASSWORD").get()
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            ndk { debugSymbolLevel = "SYMBOL_TABLE" }
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            if (releaseStore.isPresent) signingConfig = signingConfigs.getByName("release")
        }
    }
    sourceSets["main"].assets.srcDir("../../build/android-native/assets")
    sourceSets["main"].assets.srcDir(sourceAssets)
    if (channelProcessFixture) {
        sourceSets["debug"].manifest.srcFile("src/channelProcessFixture/AndroidManifest.xml")
        sourceSets["debug"].java.srcDir("src/channelProcessFixture/java")
    }
    if (pluginJsEnabled) {
        sourceSets["androidTest"].java.srcDir("src/pluginRuntimeTest/java")
        if (runtimeSdkDirectory != null) sourceSets["androidTest"].assets.srcDir(runtimeSdkDirectory)
    }
    sourceSets["main"].res.srcDir("../../assets/icons/android")
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
    // Compress native libraries in the APK; Android extracts them at installation.
    packaging { jniLibs.useLegacyPackaging = true }
}
tasks.named("preBuild").configure { dependsOn(writeSourceMetadata) }
dependencies {
    implementation(libs.commonmark)
    implementation(project(":bridge"))
    implementation(project(":data"))
    implementation(project(":domain"))
    implementation(project(":terminal"))
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.material3)
    implementation(libs.activity.compose)
    implementation(libs.lifecycle.viewmodel.compose)
    implementation(libs.coroutines.android)
    androidTestImplementation(libs.android.test.junit)
    androidTestImplementation(libs.android.test.runner)
    // Match the Espresso version already used at runtime by the Compose tests.
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.0")
    androidTestImplementation(platform(libs.compose.bom))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}


// This is a child executable for instrumentation tests, packaged only in the
// test APK. The release application has no terminal test binary or hook.
val testPtyLibraries = layout.buildDirectory.dir("generated/test-pty/jniLibs")
val buildTestPty = tasks.register<Exec>("buildTestPty") {
    val source = layout.projectDirectory.file("src/androidTest/cpp/pty_fixture.c")
    val output = testPtyLibraries.map { it.file("arm64-v8a/libghosterm_test_pty.so") }
    inputs.file(source)
    inputs.property("ndkVersion", requireNotNull(android.ndkVersion))
    outputs.file(output)
    doFirst {
        val host = when {
            System.getProperty("os.name").startsWith("Mac") -> "darwin-x86_64"
            System.getProperty("os.name").startsWith("Linux") -> "linux-x86_64"
            else -> error("Unsupported host for instrumentation PTY executable")
        }
        val compiler = android.sdkDirectory.resolve("ndk/${android.ndkVersion}/toolchains/llvm/prebuilt/$host/bin/aarch64-linux-android29-clang")
        check(compiler.isFile) { "Missing pinned Android compiler: $compiler" }
        val temporary = rootProject.projectDir.parentFile.resolve("tmp/android-test-pty").apply { mkdirs() }
        val binary = output.get().asFile.apply { parentFile.mkdirs() }
        environment("TMPDIR", temporary.absolutePath)
        commandLine(compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fPIE", "-pie",
            "-Wl,-z,max-page-size=16384", source.asFile, "-o", binary)
    }
}
android.sourceSets.getByName("androidTest").jniLibs.srcDir(testPtyLibraries)
tasks.matching { it.name == "preDebugAndroidTestBuild" }.configureEach { dependsOn(buildTestPty) }

// Standalone transport child: the production APK never includes this executable.
val processTestLibraries = layout.buildDirectory.dir("generated/test-process-channel/jniLibs")
val buildProcessChannelChild = tasks.register<Exec>("buildProcessChannelChild") {
    val source = rootProject.file("../core/tests/process_channel_child.c")
    val output = processTestLibraries.map { it.file("arm64-v8a/libghosterm_test_process_channel.so") }
    inputs.file(source); inputs.property("ndkVersion", requireNotNull(android.ndkVersion)); outputs.file(output)
    doFirst {
        val host = when {
            System.getProperty("os.name").startsWith("Mac") -> "darwin-x86_64"
            System.getProperty("os.name").startsWith("Linux") -> "linux-x86_64"
            else -> error("Unsupported instrumentation compiler host")
        }
        val compiler = android.sdkDirectory.resolve("ndk/${android.ndkVersion}/toolchains/llvm/prebuilt/$host/bin/aarch64-linux-android29-clang")
        check(compiler.isFile)
        val temporary = rootProject.projectDir.parentFile.resolve("tmp/android-test-process-channel").apply { mkdirs() }
        val binary = output.get().asFile.apply { parentFile.mkdirs() }
        environment("TMPDIR", temporary.absolutePath)
        commandLine(compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DGT_PROCESS_CHANNEL_STANDALONE=1",
            "-fPIE", "-pie", "-Wl,-z,max-page-size=16384", source, "-o", binary)
    }
}
android.sourceSets.getByName("androidTest").jniLibs.srcDir(processTestLibraries)
tasks.matching { it.name == "preDebugAndroidTestBuild" }.configureEach { dependsOn(buildProcessChannelChild) }

if (pluginJsEnabled) {
    androidComponents.onVariants { variant ->
        val generateLicense = tasks.register<GenerateQuickJsLicense>(
            "generate${variant.name.replaceFirstChar { it.uppercaseChar() }}QuickJsLicense"
        ) {
            license.set(rootProject.layout.projectDirectory.file("../vendor/quickjs-ng/LICENSE"))
        }
        variant.sources.assets?.addGeneratedSourceDirectory(generateLicense, GenerateQuickJsLicense::assets)
    }
}
