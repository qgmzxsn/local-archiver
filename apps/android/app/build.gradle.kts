plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "app.localarchiver"
    compileSdk = 34
    defaultConfig {
        applicationId = "app.localarchiver"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
        externalNativeBuild {
            cmake {
                arguments += "-DSEVENZIP_SRC=${rootDir}/../../third_party/7zip/src"
            }
        }
    }
    buildFeatures {
        buildConfig = false
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
}
