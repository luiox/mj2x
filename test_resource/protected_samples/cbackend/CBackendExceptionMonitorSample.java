package protected_samples.cbackend;

/**
 * CBackend fixture for exception and monitor trace development.
 *
 * <p>The methods exercise explicit throws, catch handlers, and synchronized
 * blocks so mjdbg can distinguish Java exception edges from native JNI calls.
 */
public class CBackendExceptionMonitorSample {
    public static int throwCatch(int x) {
        try {
            if (x > 0) {
                throw new IllegalArgumentException("positive");
            }
            return 7;
        } catch (IllegalArgumentException ex) {
            return 19;
        }
    }

    public static int synchronizedAdd(int x) {
        Object lock = new Object();
        synchronized (lock) {
            return x + 1;
        }
    }

    public static int throwThroughNative(int x) {
        if (x == 0) {
            throw new IllegalStateException("zero");
        }
        return x;
    }
}
