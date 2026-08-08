package com.mkapk.tools;

import java.io.File;
import java.io.PrintStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;

public class KotlinHandler implements ToolHandler {

    @Override
    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
        System.setOut(outStream);
        System.setErr(errStream);

        String termuxPrefix = System.getenv("PREFIX");
        if (termuxPrefix == null || termuxPrefix.isEmpty()) {
            termuxPrefix = "/data/data/com.termux/files/usr";
        }

        String kotlinHome = termuxPrefix + "/opt/kotlin";
        String compilerJar = kotlinHome + "/lib/kotlin-compiler.jar";
        String preloaderJar = kotlinHome + "/lib/kotlin-preloader.jar";
        String compilerClass = "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler";

        String[] preloaderArgs = new String[3 + args.length];
        preloaderArgs[0] = "-cp";
        preloaderArgs[1] = compilerJar;
        preloaderArgs[2] = compilerClass;
        System.arraycopy(args, 0, preloaderArgs, 3, args.length);

        ClassLoader originalContextLoader = Thread.currentThread().getContextClassLoader();
        URL[] preloaderUrls = new URL[]{ new File(preloaderJar).toURI().toURL() };

        try (URLClassLoader isolatedLoader = new URLClassLoader(preloaderUrls, ClassLoader.getPlatformClassLoader())) {
            Thread.currentThread().setContextClassLoader(isolatedLoader);

            Class<?> preloaderClazz = isolatedLoader.loadClass("org.jetbrains.kotlin.preloading.Preloader");
            Method mainMethod = preloaderClazz.getMethod("main", String[].class);

            try {
                mainMethod.invoke(null, (Object) preloaderArgs);
            } catch (InvocationTargetException e) {
                // Dig through nested reflection wrappers to catch ExitInterceptedException
                Throwable target = e;
                while (target instanceof InvocationTargetException) {
                    target = ((InvocationTargetException) target).getTargetException();
                }
                if (target instanceof MkapkTools.ExitInterceptedException) {
                    MkapkTools.ExitInterceptedException exitEx = (MkapkTools.ExitInterceptedException) target;
                    return exitEx.status == 0;
                }
                throw e;
            }

            return true;
        } finally {
            Thread.currentThread().setContextClassLoader(originalContextLoader);
        }
    }
}
