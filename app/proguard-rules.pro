# Keep JNI entry points
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.vemod.container.bridge.** { *; }
