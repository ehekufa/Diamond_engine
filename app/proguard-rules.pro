# =============================================================================
#  Diamond Engine — R8 rules
# =============================================================================
#  The only reflection-ish surface is JNI: every native method lives on
#  com.diamond.engine.jni.NativeBridge and is called by name from C.
# =============================================================================

-keepclasseswithmembernames class * {
    native <methods>;
}

-keep class com.diamond.engine.jni.NativeBridge { *; }
-keep class com.diamond.engine.jni.NativeBridge$* { *; }
-keep class com.diamond.engine.model.** { *; }

# The AndroidSurfaceView host is instantiated from XML.
-keep class com.diamond.engine.view.EngineSurfaceView { *; }

# Keep enum valueOf/values used by the block editor serialisation.
-keepclassmembers enum * {
    public static **[] values();
    public static ** valueOf(java.lang.String);
}

-dontwarn androidx.recyclerview.**
-renamesourcefileattribute SourceFile
