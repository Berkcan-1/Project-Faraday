package com.aerolon.ncv;

/** SADECE TEST: Kotlin NativeBridge.kt'nin Java aynası (host JVM'de JNI sembollerini doğrulamak için). Projeye kopyalama. */
public class NativeBridge {
    static { System.loadLibrary("ncv"); }
    public native String nativeGetEngineVersion();
    public native String nativeSelfTest();
    public native long nativeCreateVm(int ramMiB);
    public native long nativeCreateVmWithInitialEL(int ramMiB, int initialEL);
    public native int nativeDestroyVm(long handle);
    public native int nativeLoadPayload(long handle, byte[] data, long addr);
    public native int nativeSetEntry(long handle, long pc);
    public native int nativeStart(long handle);
    public native int nativePause(long handle);
    public native int nativeResume(long handle);
    public native int nativeStop(long handle);
    public native int nativeReset(long handle);
    public native int nativeGetState(long handle);
    public native byte[] nativeGetConsole(long handle, boolean take);
    public native String nativeGetStopInfo(long handle);
}
