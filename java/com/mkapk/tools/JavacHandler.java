package com.mkapk.tools;

import java.io.PrintStream;
import java.io.PrintWriter;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.Set;

public class JavacHandler implements ToolHandler {

    private final Set<URL> dynamicClassPathUrls;

    /**
     * @param dynamicClassPathUrls Reference to the shared pool tracking URLs resolved dynamically via ResolverHandler
     */
    public JavacHandler(Set<URL> dynamicClassPathUrls) {
        this.dynamicClassPathUrls = dynamicClassPathUrls;
    }

    @Override
    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
        // javac's API expects a PrintWriter to capture diagnostics and compilation output
        PrintWriter logWriter = new PrintWriter(outStream);

        // Save original thread context classloader for restoration in the finally block
        ClassLoader originalContextLoader = Thread.currentThread().getContextClassLoader();

        // Create child context isolating runtime dependencies properly
        try (URLClassLoader contextLoader = new URLClassLoader(
                dynamicClassPathUrls.toArray(new URL[0]), 
                JavacHandler.class.getClassLoader())) {

            // Bridge context classloader safely onto the active thread map
            Thread.currentThread().setContextClassLoader(contextLoader);

            // Delegate execution to OpenJDK internal compiler entry point
            int result = com.sun.tools.javac.Main.compile(args, logWriter);
            logWriter.flush();

            // javac returns 0 on successful compilation
            return result == 0;
        } finally {
            // Guarantee restoration of the daemon's standard classloader
            Thread.currentThread().setContextClassLoader(originalContextLoader);
        }
    }
}
