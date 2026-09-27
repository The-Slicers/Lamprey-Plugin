import java.io.File;
import java.lang.reflect.Method;
import java.net.URLClassLoader;
import Janus.Janus;

// For testing jar execution

public class Main {
    static {
        System.loadLibrary("Janus");
        System.load(new java.io.File("").getAbsolutePath() + "\\Janus.dll");
    }

    public static void main(String[] args) throws Exception {
        Janus.PrintData();
    }
}