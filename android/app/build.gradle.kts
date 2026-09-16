plugins {
    id("com.android.application")
}

android {
    namespace = "org.wiicompiled.mkwvr"
    // Quest 2, Quest Pro, Quest 3 and Quest 3S all run Android 12L (API 32).
    // Nothing older is a supported device for this port, so the minimum is the
    // same as the target rather than a lower value the code never exercises.
    compileSdk = 34

    defaultConfig {
        applicationId = "org.wiicompiled.mkwvr"
        minSdk = 32
        targetSdk = 32
        versionCode = 1
        versionName = "1.0-quest"

        // The statically recompiled game is arm64 only; there is no 32-bit
        // Quest and no reason to carry another ABI's stub.
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    // libmkwquest.so is produced by Launcher/Build-Quest.sh and copied into
    // src/main/jniLibs/arm64-v8a. See settings.gradle.kts for why Gradle does
    // not drive the native build itself.
    sourceSets {
        getByName("main") {
            jniLibs.srcDirs("src/main/jniLibs")
            assets.srcDirs("src/main/assets")
        }
    }

    packaging {
        jniLibs {
            // The Quest OpenXR loader and the Android 15+ linker both require
            // the shared objects to be loadable straight from the APK.
            useLegacyPackaging = false
        }
    }

    buildTypes {
        release {
            // The native side carries the cost here; there is no Java/Kotlin
            // code of consequence to shrink, and leaving this off keeps the
            // NativeActivity entry point reachable without a keep rule.
            isMinifyEnabled = false
            isJniDebuggable = false
        }
        debug {
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    // The Khronos OpenXR loader for Android. This is the loader that is
    // actually initialised through XR_KHR_loader_init_android in
    // runtime/src/vr/openxr_android.cpp; on a Quest it resolves to the Meta
    // runtime installed on the device.
    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.41")
}
