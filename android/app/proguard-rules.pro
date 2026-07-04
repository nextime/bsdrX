# Keep the JNI seam: native method names and the callbacks invoked from C
# (bsdr_jni.c looks these up by name via GetMethodID).
-keepclassmembers class net.nexlab.bsdrandroid.NativeBridge {
    *** nativeStart(...);
    *** nativeStop(...);
    *** nativePushVideo(...);
    *** nativePushAudio(...);
    *** nativePollVideoWant(...);
    public void onPairingCode(java.lang.String);
    public void onMicPcm(short[], int, int);
    public void onInputEvent(int, int, int);
}
