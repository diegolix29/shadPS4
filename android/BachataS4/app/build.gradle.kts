import java.util.Properties
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.compose.compiler)
    alias(libs.plugins.hilt)
    alias(libs.plugins.ksp)
}

android {
    namespace = "com.bachatas4.android"
    compileSdk = 37

    val localProperties = Properties()
    val localPropertiesFile = rootProject.file("local.properties")
    if (localPropertiesFile.exists()) {
        localPropertiesFile.inputStream().use {
            localProperties.load(it)
        }
    }

    val versionProperties = Properties()
    val versionPropertiesFile = rootProject.file("version.properties")
    if (versionPropertiesFile.exists()) {
        versionPropertiesFile.inputStream().use {
            versionProperties.load(it)
        }
    }

    val releaseKeystoreName = localProperties.getProperty("signing.storeFile")
    val releaseKeystoreFile =
        if (releaseKeystoreName != null) rootProject.file(releaseKeystoreName) else null
    val hasReleaseKeystore = releaseKeystoreFile != null && releaseKeystoreFile.exists()
    if (hasReleaseKeystore) {
        signingConfigs {
            create("release") {
                storeFile = releaseKeystoreFile
                storePassword = localProperties.getProperty("signing.storePassword")
                keyAlias = localProperties.getProperty("signing.keyAlias")
                keyPassword = localProperties.getProperty("signing.keyPassword")
            }
        }
    }

    defaultConfig {
        applicationId = "com.bachatas4.android"
        minSdk = 31
        targetSdk = 37
        // Priority: CLI -P only > version.properties > date-based dev defaults
        // (do not read VERSION_* from gradle.properties — that fights AutoUpdate)
        val cliVersionCode = gradle.startParameter.projectProperties["VERSION_CODE"]
        val cliVersionName = gradle.startParameter.projectProperties["VERSION_NAME"]
        versionCode = cliVersionCode?.toIntOrNull()
            ?: versionProperties.getProperty("VERSION_CODE")?.toIntOrNull()
            ?: SimpleDateFormat("yyMMddHH").format(Date()).toInt()
        versionName = cliVersionName
            ?: versionProperties.getProperty("VERSION_NAME")
            ?: ("0.1.0-dev-" + SimpleDateFormat("yyyyMMdd-HHmm").format(Date()))
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            // Monorepo subdir builds do not embed VCS consistently; omit so local
            // and F-Droid APKs both lack version-control-info rather than diverge.
            vcsInfo.include = false
            // Only attach signing when a local release keystore is configured.
            // F-Droid builds strip signing config and must remain unsigned here.
            signingConfigs.findByName("release")?.let { signingConfig = it }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        buildConfig = true
        compose = true
    }

    flavorDimensions += "distribution"
    productFlavors {
        create("playstore") {
            dimension = "distribution"
            buildConfigField("Boolean", "DOWNLOAD_RUNTIME", "false")
            // Play: bundled Turnip only — skip driver picker in setup/settings.
            buildConfigField("Boolean", "SHOW_DRIVER_SELECTION", "false")
        }
        create("fdroid") {
            dimension = "distribution"
            buildConfigField("Boolean", "DOWNLOAD_RUNTIME", "false")
            buildConfigField("Boolean", "SHOW_DRIVER_SELECTION", "true")
        }
    }
    androidResources {
        noCompress += listOf("zip", "json")
    }
    packaging {
        jniLibs {
            useLegacyPackaging = true
            // Keep unstripped .so files so F-Droid vs developer strip steps cannot diverge.
            keepDebugSymbols += "**/*.so"
        }
    }
}

// F-Droid reproducible builds: baseline.prof / baseline.profm are often non-deterministic.
// https://f-droid.org/docs/Reproducible_Builds/#bug-baselineprof-not-deterministic
tasks.whenTaskAdded {
    if (name.contains("ArtProfile")) {
        enabled = false
    }
}

// Post-assemble gate: fail the build if the freshly built JNI libraries lack the
// Bachata S4 runtime fixes (unlockHardwareBuffer export, abstract-socket handling,
// robust vortek server). These markers are lost when CI vendors pristine upstream
// sources, so verify the actual APK output, not just the sources.
tasks.register("verifyNativeRuntimeFixes") {
    group = "verification"
    description = "Checks the assembled APK for the Bachata S4 native runtime fixes."
    doLast {
        val apk = layout.buildDirectory.dir("outputs/apk").get().asFile.walkTopDown()
            .filter { it.isFile && it.extension == "apk" }
            .maxByOrNull { it.lastModified() }
            ?: error("No APK found under ${layout.buildDirectory.get()}/outputs/apk")
        val script = File(rootProject.projectDir, "../../runtime/tests/verify-native-fixes.mjs")
        val process = ProcessBuilder("node", script.absolutePath, apk.absolutePath)
            .inheritIO()
            .start()
        val exit = process.waitFor()
        if (exit != 0) throw GradleException("verifyNativeRuntimeFixes failed (exit $exit): native runtime fixes missing from APK")
    }
}
tasks.matching { it.name.startsWith("assemble") }.configureEach {
    finalizedBy("verifyNativeRuntimeFixes")
}

androidComponents {
    beforeVariants { variantBuilder ->
        val startParameterTasks = gradle.startParameter.taskNames
        val hasPlaystoreExplicitly = startParameterTasks.any { it.contains("playstore", ignoreCase = true) }
        val hasGenericAssemble = startParameterTasks.any { 
            it.endsWith("assemble") || 
            it.endsWith("assembleDebug") || 
            it.endsWith("assembleRelease") || 
            it.endsWith("build") 
        }
        
        if (variantBuilder.flavorName == "playstore" && hasGenericAssemble && !hasPlaystoreExplicitly) {
            variantBuilder.enable = false
        }
    }
}

dependencies {
    implementation(project(":core:designsystem"))
    implementation(project(":core:data"))
    implementation(project(":core:runtime"))
    implementation(project(":feature:setup"))
    implementation(project(":feature:library"))
    implementation(project(":feature:session"))
    implementation(project(":feature:settings"))
    implementation(project(":feature:drivers"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.hilt.navigation.compose)
    implementation("androidx.documentfile:documentfile:1.1.0")
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.material3)
    implementation(libs.hilt.android)
    ksp(libs.hilt.compiler)
    ksp(libs.kotlin.metadata.jvm)
    androidTestImplementation(libs.androidx.test.runner)
    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
}
