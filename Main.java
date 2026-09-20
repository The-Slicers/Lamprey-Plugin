import java.io.File;
import java.lang.reflect.Method;
import java.net.URLClassLoader;

// For testing jar execution

public class Main {
    public static void main(String[] args) throws Exception {
        java.net.URL jarUrl = new File("janus.jar").toURI().toURL();
        try (URLClassLoader loader = new URLClassLoader(new java.net.URL[]{ jarUrl },
                                             Main.class.getClassLoader())) {
            Class<?> cls = loader.loadClass("com.example.Janus.Janus");   // dots, not slashes

            // static method: String greet(String)
            Method printData = cls.getMethod("printData");
            printData.invoke(null);
        }
    }
}