import java.io.FileInputStream
import java.util.Properties

plugins {
    id("com.android.application")
    id("kotlin-android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

// Release signing config. Looked up from android/key.properties (which is
// gitignored). When the file is absent we fall back to the debug keys so a
// fresh-clone dev build still works; release CI / publish paths put the
// keystore + this file in place.
val keystorePropertiesFile = rootProject.file("key.properties")
val keystoreProperties = Properties().apply {
    if (keystorePropertiesFile.exists()) {
        FileInputStream(keystorePropertiesFile).use { load(it) }
    }
}

android {
    namespace = "com.example.bopwire_player"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = "28.2.13676358"

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_17.toString()
    }

    defaultConfig {
        // TODO: Specify your own unique Application ID (https://developer.android.com/studio/build/application-id.html).
        applicationId = "com.example.bopwire_player"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = flutter.minSdkVersion
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName
        ndk {
            // We only ship a vanilla librats build for arm64-v8a (paired with
            // the android-openssl arm64 prebuilt). Adding armeabi-v7a or
            // x86_64 would require rebuilding libmc_rats.so + OpenSSL for
            // those ABIs, and the only test device is arm64.
            abiFilters += listOf("arm64-v8a")
        }
        // Restrict the CMake/ninja external native build to the same ABI
        // — without this, ninja tries to link our new chromaprint_jni.so
        // against the (nonexistent) armeabi-v7a / x86_64 prebuilt
        // libchromaprint.so and the build dies before packaging.
        externalNativeBuild {
            cmake {
                abiFilters += listOf("arm64-v8a")
            }
        }
    }

    signingConfigs {
        create("release") {
            val storeFilePath = keystoreProperties.getProperty("storeFile")
            if (storeFilePath != null) {
                storeFile = file(storeFilePath)
            }
            storePassword = keystoreProperties.getProperty("storePassword")
            keyAlias      = keystoreProperties.getProperty("keyAlias")
            keyPassword   = keystoreProperties.getProperty("keyPassword")
        }
    }

    buildTypes {
        release {
            // Use the release keystore when the operator dropped a
            // key.properties + matching .jks into android/. Otherwise
            // fall through to debug so `flutter run --release` still
            // builds on a fresh clone without a release identity.
            signingConfig = if (keystorePropertiesFile.exists()) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
    }
}

flutter {
    source = "../.."
}
