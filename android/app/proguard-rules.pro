# JNI から参照される Kotlin/Java シンボルは難読化しない。
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class dev.saku.prismearring.NativeEngine { *; }
