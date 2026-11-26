import java.io.File
import java.util.Properties

plugins {
    kotlin("jvm") version "2.3.20"
}

group = "com.dexlink"
version = "1.0.0"

repositories {
    google()
    mavenCentral()
}

fun findAndroidSdkDir(): File {
    val envHome = System.getenv("ANDROID_HOME") ?: System.getenv("ANDROID_SDK_ROOT")
    if (!envHome.isNullOrBlank()) {
        val f = File(envHome)
        if (f.exists()) return f
    }
    val localProps = File(rootDir, "local.properties")
    if (localProps.exists()) {
        val props = Properties()
        localProps.inputStream().use { props.load(it) }
        val sdkDir = props.getProperty("sdk.dir")
        if (!sdkDir.isNullOrBlank()) return File(sdkDir)
    }
    val defaultWindows = File(System.getProperty("user.home"), "AppData/Local/Android/Sdk")
    if (defaultWindows.exists()) return defaultWindows
    throw GradleException("Android SDK not found. Set ANDROID_HOME.")
}

val sdkDir = findAndroidSdkDir()
val platformsDir = File(sdkDir, "platforms")
val androidPlatform = platformsDir.listFiles()
    ?.filter { it.isDirectory && it.name.startsWith("android-") && File(it, "android.jar").exists() }
    ?.sortedByDescending { it.name.removePrefix("android-").substringBefore('.').toIntOrNull() ?: 0 }
    ?.firstOrNull()
    ?: throw GradleException("No android.jar found in $platformsDir")

val androidJar = File(androidPlatform, "android.jar")

val buildToolsDir = File(sdkDir, "build-tools")
val latestBuildTools = buildToolsDir.listFiles()
    ?.filter { it.isDirectory && (File(it, "d8.bat").exists() || File(it, "d8").exists()) }
    ?.sortedByDescending { it.name }
    ?.firstOrNull()
    ?: throw GradleException("No d8 found in $buildToolsDir")

val d8Tool = if (System.getProperty("os.name").lowercase().contains("win")) {
    File(latestBuildTools, "d8.bat")
} else {
    File(latestBuildTools, "d8")
}

kotlin {
    jvmToolchain(17)
}

sourceSets {
    main {
        kotlin.srcDir("../protocol/kotlin")
    }
}

dependencies {
    compileOnly(files(androidJar))
    testImplementation(kotlin("test"))
}

tasks.test {
    useJUnitPlatform()
}

tasks.register<Exec>("buildServerJar") {
    group = "build"
    description = "Compile and DEX-package dexlink-server.jar for adb shell app_process"
    dependsOn(tasks.jar)

    val outJar = layout.buildDirectory.file("dexlink-server.jar")
    inputs.files(tasks.jar.flatMap { it.archiveFile }, configurations.runtimeClasspath)
    outputs.file(outJar)

    doFirst {
        val jarFile = tasks.jar.get().archiveFile.get().asFile
        val runtimeJars = configurations.runtimeClasspath.get().files.filter { it.name.endsWith(".jar") }
        val outputFile = outJar.get().asFile
        outputFile.parentFile.mkdirs()
        if (outputFile.exists()) outputFile.delete()

        val cmd = mutableListOf(
            d8Tool.absolutePath,
            "--min-api", "30",
            "--lib", androidJar.absolutePath,
            "--output", outputFile.absolutePath,
            jarFile.absolutePath
        )
        runtimeJars.forEach { cmd.add(it.absolutePath) }
        commandLine(cmd)
    }
}
