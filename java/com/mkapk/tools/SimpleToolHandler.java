package com.mkapk.tools;

import java.io.File;
import java.io.PrintStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.ArrayList;
import java.util.List;

public class SimpleToolHandler implements ToolHandler {

    private final String mainClassName;
    private final List<File> isolatedJars = new ArrayList<>();

    public SimpleToolHandler(String mainClassName) {
        this.mainClassName = mainClassName;
    }

    public SimpleToolHandler(String mainClassName, String... jarPaths) {
        this.mainClassName = mainClassName;
        for (String path : jarPaths) {
            File jarFile = new File(path);
            if (jarFile.exists()) {
                this.isolatedJars.add(jarFile);
            }
        }
    }

    @Override
    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
        PrintStream originalOut = System.out;
        PrintStream originalErr = System.err;
        System.setOut(outStream);
        System.setErr(errStream);

        try {
            if (!isolatedJars.isEmpty()) {
                URL[] urls = new URL[isolatedJars.size()];
                for (int i = 0; i < isolatedJars.size(); i++) {
                    urls[i] = isolatedJars.get(i).toURI().toURL();
                }

                try (URLClassLoader targetLoader = new URLClassLoader(urls, getClass().getClassLoader())) {
                    Class<?> clazz = Class.forName(mainClassName, true, targetLoader);
                    Method mainMethod = clazz.getMethod("main", String[].class);

                    ClassLoader previousContextLoader = Thread.currentThread().getContextClassLoader();
                    Thread.currentThread().setContextClassLoader(targetLoader);
                    try {
                        mainMethod.invoke(null, (Object) args);
                    } catch (InvocationTargetException e) {
                        return handleInvocationTargetException(e);
                    } finally {
                        Thread.currentThread().setContextClassLoader(previousContextLoader);
                    }
                }
            } else {
                Class<?> clazz = Class.forName(mainClassName);
                Method mainMethod = clazz.getMethod("main", String[].class);
                try {
                    mainMethod.invoke(null, (Object) args);
                } catch (InvocationTargetException e) {
                    return handleInvocationTargetException(e);
                }
            }

            return true;
        } finally {
            System.setOut(originalOut);
            System.setErr(originalErr);
        }
    }

    /**
     * Unwraps nested reflection exceptions to intercept System.exit status codes.
     */
    private boolean handleInvocationTargetException(InvocationTargetException e) throws Exception {
        Throwable target = e;
        while (target instanceof InvocationTargetException) {
            target = ((InvocationTargetException) target).getTargetException();
        }

        if (target instanceof MkapkTools.ExitInterceptedException exitEx) {
            return exitEx.status == 0;
        }

        if (target instanceof Exception ex) {
            throw ex;
        }

        if (target instanceof Error err) {
            throw err;
        }

        throw new RuntimeException(target);
    }
}
