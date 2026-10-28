import com.aerolon.ncv.NativeBridge;
import java.nio.charset.StandardCharsets;

public class HostJniTest {
    static int failures = 0;
    static void check(boolean c, String msg) { if (!c) { failures++; System.out.println("  FAIL: " + msg); } else System.out.println("  ok: " + msg); }
    static boolean waitState(NativeBridge b, long h, int want) throws Exception {
        for (int i = 0; i < 5000; i++) { if (b.nativeGetState(h) == want) return true; Thread.sleep(1); }
        return false;
    }
    public static void main(String[] a) throws Exception {
        NativeBridge b = new NativeBridge();
        check(b.nativeGetEngineVersion().equals("0.1.0-dev"), "version");
        String st = b.nativeSelfTest();
        check(st.startsWith("SELFTEST PASS"), "selftest: " + st.replace("\n", " | "));

        // hello payload (assembler çıktısı) JNI üzerinden
        byte[] hello = java.nio.file.Files.readAllBytes(java.nio.file.Paths.get(a[0]));
        long h = b.nativeCreateVm(1);
        check(h > 0, "createVm handle=" + h);
        check(b.nativeLoadPayload(h, hello, 0x40000000L) == 0, "loadPayload");
        check(b.nativeStart(h) == 0, "start");
        check(waitState(b, h, 3), "state -> Stopped");
        String out = new String(b.nativeGetConsole(h, false), StandardCharsets.UTF_8);
        check(out.equals("NCV GUEST BOOT OK\n"), "console=" + out.trim());
        check(new String(b.nativeGetConsole(h, true), StandardCharsets.UTF_8).equals(out), "console take");
        check(b.nativeGetConsole(h, false).length == 0, "console drained");
        check(b.nativeGetStopInfo(h).startsWith("reason=Halted"), "stopInfo: " + b.nativeGetStopInfo(h));
        check(b.nativeDestroyVm(h) == 0, "destroy");

        // geçersiz handle / argüman yolları (çökme olmamalı)
        check(b.nativeDestroyVm(h) == -100, "double destroy -> -100");
        check(b.nativeStart(h) == -100, "start on dead handle -> -100");
        check(b.nativeGetState(h) == -100, "state on dead handle -> -100");
        check(b.nativeGetConsole(h, false).length == 0, "console on dead handle -> empty");
        check(b.nativeStart(987654321L) == -100, "start on unknown handle -> -100");
        check(b.nativeCreateVm(0) < 0, "createVm(0) rejected");
        check(b.nativeCreateVm(5000) < 0, "createVm(5000) rejected");
        check(b.nativeCreateVmWithInitialEL(1, 2) < 0, "initialEL 2 rejected");
        long hEl1 = b.nativeCreateVmWithInitialEL(1, 1);
        check(hEl1 > 0, "createVmWithInitialEL(1, 1) handle=" + hEl1);
        check(b.nativeDestroyVm(hEl1) == 0, "destroy EL1-configured VM");
        long h2 = b.nativeCreateVm(1);
        check(b.nativeLoadPayload(h2, null, 0x40000000L) == 1, "null payload -> InvalidArgument(1)");
        check(b.nativeLoadPayload(h2, new byte[2 << 20], 0x40000000L) == 4, "payload > RAM -> OutOfBounds(4)");
        check(b.nativeLoadPayload(h2, hello, 0x1000L) == 3, "payload @unmapped -> Unmapped(3)");
        check(b.nativePause(h2) == 2, "pause in Created -> InvalidState(2)");

        // pause/resume/stop (b .)
        byte[] loop = {0, 0, 0, 0x14};
        check(b.nativeLoadPayload(h2, loop, 0x40000000L) == 0, "load loop");
        check(b.nativeStart(h2) == 0, "start loop");
        Thread.sleep(20);
        check(b.nativeGetState(h2) == 1, "Running");
        check(b.nativePause(h2) == 0 && b.nativeGetState(h2) == 2, "Paused");
        check(b.nativeResume(h2) == 0 && b.nativeGetState(h2) == 1, "Resumed");
        check(b.nativeStop(h2) == 0 && b.nativeGetState(h2) == 3, "Stopped");
        check(b.nativeGetStopInfo(h2).startsWith("reason=StoppedByHost"), "stopInfo: " + b.nativeGetStopInfo(h2));
        check(b.nativeReset(h2) == 0 && b.nativeGetState(h2) == 0, "Reset -> Created");
        check(b.nativeDestroyVm(h2) == 0, "destroy");

        // çalışırken destroy
        long h3 = b.nativeCreateVm(1);
        b.nativeLoadPayload(h3, loop, 0x40000000L);
        b.nativeStart(h3);
        check(b.nativeDestroyVm(h3) == 0, "destroy while running");

        System.out.println(failures == 0 ? "JNI HOST TEST: PASS" : "JNI HOST TEST: FAIL (" + failures + ")");
        System.exit(failures == 0 ? 0 : 1);
    }
}
