plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}
val releaseStore = providers.environmentVariable("ANDROID_RELEASE_STORE_FILE")
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
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    kotlinOptions { jvmTarget = "17" }
    // Compress native libraries in the APK; Android extracts them at installation.
    packaging { jniLibs.useLegacyPackaging = true }
}
dependencies {
    implementation(project(":bridge"))
    implementation(project(":data"))
    implementation(project(":domain"))
    implementation(project(":terminal"))
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.material3)
    implementation(libs.activity.compose)
    implementation(libs.lifecycle.viewmodel.compose)
    androidTestImplementation(libs.android.test.junit)
    androidTestImplementation(libs.android.test.runner)
    // Match the Espresso version already used at runtime by the Compose tests.
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.0")
    androidTestImplementation(platform(libs.compose.bom))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
