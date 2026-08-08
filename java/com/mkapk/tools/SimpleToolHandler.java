package com.mkapk.tools;

import java.io.PrintStream;
import java.lang.reflect.Method;

public class SimpleToolHandler implements ToolHandler {

    private final String mainClassName;

    /**
     * Constructs a handler for a tool identified by its main entry-point class.
     * 
     * @param mainClassName Fully qualified class name (e.g. "com.android.tools.r8.D8")
     */
    public SimpleToolHandler(String mainClassName) {
        this.mainClassName = mainClassName;
    }

    @Override
    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
        // 1. Intercept stream outputs so stdout/stderr don't corrupt the IPC pipe
        System.setOut(outStream);
        System.setErr(errStream);

        // 2. Load the target class dynamically from the daemon's active classloader
        Class<?> clazz = Class.forName(mainClassName);

        // 3. Locate the public static void main(String[] args) entry method
        Method mainMethod = clazz.getMethod("main", String[].class);

        // 4. Invoke the method
        // Note: Array cast to Object is required to prevent varargs unraveling
        mainMethod.invoke(null, (Object) args);

        return true;
    }
}
