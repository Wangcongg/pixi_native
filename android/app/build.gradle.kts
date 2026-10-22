import groovy.json.JsonSlurper
import groovy.json.JsonOutput
plugins { id("com.android.application") }
val source=file("../../generated/app.json")
require(source.isFile) { "Run node tools/kit.mjs prepare <config> first" }
@Suppress("UNCHECKED_CAST") val config=JsonSlurper().parse(source) as Map<String,Any>
@Suppress("UNCHECKED_CAST") val app=config["app"] as Map<String,Any>
@Suppress("UNCHECKED_CAST") val androidConfig=config["android"] as Map<String,Any>
android {
 namespace="org.pixinative.runtime"; compileSdk=36; ndkVersion="27.2.12479018"
 defaultConfig {
  applicationId=app["id"].toString(); minSdk=26; targetSdk=36
  versionCode=(app["build"] as Number).toInt(); versionName=app["version"].toString()
  @Suppress("UNCHECKED_CAST") val abis=androidConfig["abis"] as List<String>
  ndk { abiFilters.addAll(abis) }
  externalNativeBuild.cmake { cppFlags+=listOf("-std=c++17"); arguments+=listOf("-DANDROID_STL=c++_shared") }
  manifestPlaceholders["screenOrientation"]=if(config["orientation"]=="portrait")"sensorPortrait" else "sensorLandscape"
  buildConfigField("float","GAME_RENDER_SCALE","1.0f")
  buildConfigField("int","GAME_RENDER_WIDTH","0"); buildConfigField("int","GAME_RENDER_HEIGHT","0")
 }
 externalNativeBuild.cmake { path=file("src/main/cpp/CMakeLists.txt");version="3.22.1" }
 sourceSets["main"].assets.directories+="../../game"
 sourceSets["main"].res.directories+="../../generated/android/res"
 buildFeatures { buildConfig=true }
 buildTypes { getByName("release") { isMinifyEnabled=true;proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"),"proguard-rules.pro") } }
 androidResources.noCompress+=listOf("ktx2","zspine","skel","webp")
 androidResources.ignoreAssetsPattern="!.svn:!.git:!.ds_store:!*.scc:.*:!CVS:!thumbs.db:!picasa.ini:!*~"
 compileOptions { sourceCompatibility=JavaVersion.VERSION_17;targetCompatibility=JavaVersion.VERSION_17 }
}
kotlin { compilerOptions.jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) }
dependencies {
 implementation("androidx.core:core-ktx:1.17.0")
 implementation("androidx.core:core-splashscreen:1.0.1")
 implementation("com.squareup.okhttp3:okhttp:5.4.0")
 implementation("com.squareup.okhttp3:okhttp-java-net-cookiejar:5.4.0")
}
