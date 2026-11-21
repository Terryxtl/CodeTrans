// Top-level build file. Module configuration lives in app/build.gradle.kts.
// Kotlin is compiled by AGP 9's built-in Kotlin support (no kotlin-android plugin);
// the classpath entry below only raises the KGP version AGP uses internally.
buildscript {
    repositories {
        google()
        mavenCentral()
    }
    dependencies {
        classpath(libs.kotlin.gradle.plugin)
    }
}

plugins {
    alias(libs.plugins.android.application) apply false
}
