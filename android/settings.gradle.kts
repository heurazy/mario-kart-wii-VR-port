// Gradle project for the standalone Meta Quest package.
//
// It deliberately does NOT drive the native build. Compiling the statically
// recompiled game is a multi-hour job that produces several gigabytes of
// objects from a disc image the user supplies, and it is orchestrated by
// Launcher/Build-Quest.sh. Gradle's job here is only to wrap the resulting
// libmkwquest.so and the runtime assets into an installable APK.

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "MarioKartWiiVR"
include(":app")
