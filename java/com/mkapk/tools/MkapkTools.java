package com.mkapk.tools;

import java.io.*;
import java.util.Arrays;
import java.security.Permission;

// Requires: maven-resolver-api, maven-resolver-impl, maven-resolver-connector-basic, maven-resolver-transport-http, maven-resolver-util
import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.artifact.DefaultArtifact;
import org.eclipse.aether.collection.CollectRequest;
import org.eclipse.aether.graph.Dependency;
import org.eclipse.aether.graph.DependencyFilter;
import org.eclipse.aether.repository.RemoteRepository;
import org.eclipse.aether.resolution.DependencyRequest;
import org.eclipse.aether.resolution.DependencyResult;
import org.eclipse.aether.util.artifact.JavaScopes;
import org.eclipse.aether.util.filter.DependencyFilterUtils;

public class MkapkTools {

    private static boolean isRunning = false;

    /**
     * Prevents tools like D8/R8 from killing the JVM process.
     * In Java 18+, this requires: -Djava.security.manager=allow
     */
    private static void preventExit() {
        try {
            System.setSecurityManager(new SecurityManager() {
                @Override
                public void checkPermission(Permission permission) {
                    // Allow all other actions
                }
                @Override
                public void checkExit(int status) {
                    // Intercept exit calls from Android tools
                    throw new SecurityException("Intercepted System.exit(" + status + ")");
                }
            });
        } catch (UnsupportedOperationException e) {
            // Forward a structured warning over standard out for C++ UI parser ingestion
            System.out.println("[WARN]|Security Manager restricted. Ensure JVM execution args include -Djava.security.manager=allow");
        }
    }

    /**
     * Resolves the local cache directory for Maven dependencies.
     * Defaults to the Termux prefix if running on-device.
     */
    private static File getLocalCacheDir() {
        String termuxPrefix = System.getenv("PREFIX");
        if (termuxPrefix == null || termuxPrefix.isEmpty()) {
            // Fallback for standard desktop Linux testing
            termuxPrefix = System.getProperty("user.home") + "/.mkapk";
        }
        
        File cacheDir = new File(termuxPrefix + "/var/lib/mkapk/lib");
        if (!cacheDir.exists()) {
            cacheDir.mkdirs();
        }
        return cacheDir;
    }

    public static void main(String[] args) {
        // Handle immediate CLI usage (e.g. java -jar mkapk-coordinator.jar javac)
        if (args.length > 0 && !args[0].equals("START_DAEMON")) {
            boolean success = handleTask(args[0].toLowerCase(), Arrays.copyOfRange(args, 1, args.length));
            System.exit(success ? 0 : 1);
            return;
        }

        // Setup the exit interceptor
        preventExit();

        // Standard Input Loop (Pipe Communication with C++)
        // BufferedReader is significantly more reliable and faster for IPC than Scanner
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(System.in))) {
            String input;
            
            while ((input = reader.readLine()) != null) {
                if (input.trim().isEmpty()) continue;

                // Protocol: COMMAND|ARG1|ARG2
                String[] parts = input.split("\\|");
                String command = parts[0].toUpperCase();
                String[] toolArgs = (parts.length > 1) 
                    ? Arrays.copyOfRange(parts, 1, parts.length) 
                    : new String[0];

                switch (command) {
                    case "START_DAEMON" -> {
                        isRunning = true;
                        System.out.println("MKAPK_DAEMON_STARTED");
                    }
                    case "STOP_DAEMON" -> {
                        System.out.println("MKAPK_DAEMON_STOPPING");
                        isRunning = false;
                        return; // Exit main and cleanly shutdown the JVM
                    }
                    default -> {
                        if (isRunning) {
                            boolean success = handleTask(command.toLowerCase(), toolArgs);
                            
                            // Signal the specific outcome to the C++ orchestrator
                            if (success) {
                                System.out.println("MKAPK_TASK_DONE");
                            } else {
                                System.out.println("MKAPK_TASK_FAILED");
                            }
                        } else {
                            System.out.println("[ERROR]|Disregarded command transaction. Transmission of START_DAEMON handshake verification token required first.");
                        }
                    }
                }
                // Crucial for IPC: Force the bytes through the pipe immediately
                System.out.flush();
            }
        } catch (Exception e) {
            // Pipe likely broken or closed gracefully by the parent native process
            System.err.println("[JVM STDERR] IPC Pipe disconnected: " + e.getMessage());
        }
    }

    /**
     * Executes the requested compiler/tool.
     * @return true if successful, false if a fatal error occurred.
     */
    private static boolean handleTask(String command, String[] args) {
        boolean taskFailed = false;

        // Structured buffer to intercept underlying standard console streams
        ByteArrayOutputStream internalLogBuffer = new ByteArrayOutputStream();
        PrintStream captureStream = new PrintStream(internalLogBuffer);

        // Retain normal process stream references
        PrintStream originalOut = System.out;
        PrintStream originalErr = System.err;

        try {
            switch (command) {
                case "javac" -> {
                    PrintWriter logWriter = new PrintWriter(captureStream);
                    int result = com.sun.tools.javac.Main.compile(args, logWriter);
                    logWriter.flush();
                    if (result != 0) {
                        taskFailed = true;
                    }
                }
                case "d8" -> {
                    System.setOut(captureStream);
                    System.setErr(captureStream);
                    com.android.tools.r8.D8.main(args);
                }
                case "r8" -> {
                    System.setOut(captureStream);
                    System.setErr(captureStream);
                    com.android.tools.r8.R8.main(args);
                }
                case "resguard" -> {
                    System.setOut(captureStream);
                    System.setErr(captureStream);
                    com.tencent.mm.resourceproguard.cli.CliMain.main(args);
                }
                case "apksigner" -> {
                    System.setOut(captureStream);
                    System.setErr(captureStream);
                    com.android.apksigner.ApkSignerTool.main(args);
                }
                
                case "resolve" -> {
                    if (args.length < 1) {
                        System.out.println("[ERROR]|Provide a maven coordinate (e.g., androidx.core:core:1.9.0)");
                        taskFailed = true;
                        break;
                    }
                    
                    String coordinate = args[0]; // Format: groupId:artifactId:extension:version (e.g. "androidx.core:core:aar:1.9.0")
                        
                    // 1. Initialize the Aether Repository System (Requires boilerplate Guice/ServiceLocator setup)
                    RepositorySystem system = Booter.newRepositorySystem();
                    
                    RepositorySystemSession session = Booter.newRepositorySystemSession(system, getLocalCacheDir());
                    
                    // 2. Define Repositories (Google Maven and Maven Central)
                    RemoteRepository googleRepo = new RemoteRepository.Builder("google", "default", "https://maven.google.com/").build();
                    RemoteRepository centralRepo = new RemoteRepository.Builder("central", "default", "https://repo1.maven.org/maven2/").build();
                    
                    // 3. Create the Dependency Request
                    Dependency dependency = new Dependency(new DefaultArtifact(coordinate), JavaScopes.COMPILE);
                    CollectRequest collectRequest = new CollectRequest();
                    collectRequest.setRoot(dependency);
                    collectRequest.addRepository(googleRepo);
                    collectRequest.addRepository(centralRepo);
                    
                    // Filter out 'test' and 'provided' scopes
                    DependencyFilter classpathFlter = DependencyFilterUtils.classpathFilter(JavaScopes.COMPILE, JavaScopes.RUNTIME);
                    DependencyRequest dependencyRequest = new DependencyRequest(collectRequest, classpathFlter);
                    
                    try {
                        // 4. Execute Resolution (This downloads files to your local cache automatically)
                        DependencyResult dependencyResult = system.resolveDependencies(session, dependencyRequest);
                        
                        // 5. Pipe the results back to C++
                        StringBuilder resolvedPaths = new StringBuilder("MKAPK_RESOLVED");
                        for (var artifactResult : dependencyResult.getArtifactResults()) {
                            File downloadedFile = artifactResult.getArtifact().getFile();
                            resolvedPaths.append("|").append(downloadedFile.getAbsolutePath());
                        }
                        System.out.println(resolvedPaths.toString());
                            
                    } catch (Exception e) {
                        System.out.println("[ERROR]|Dependency resolution failed: " + e.getMessage());
                        taskFailed = true;
                    }
                }
                case "kotlinc" -> {
                    String termuxPrefix = System.getenv("PREFIX");
                    if (termuxPrefix == null) {
                        termuxPrefix = "/data/data/com.termux/files/usr";
                    }

                    String kotlinHome = termuxPrefix + "/opt/kotlin";
                    String compilerJar = kotlinHome + "/lib/kotlin-compiler.jar";
                    String compilerClass = "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler";

                    String[] preloaderArgs = new String[3 + args.length];
                    preloaderArgs[0] = "-cp";
                    preloaderArgs[1] = compilerJar;
                    preloaderArgs[2] = compilerClass;
                    System.arraycopy(args, 0, preloaderArgs, 3, args.length);

                    System.setOut(captureStream);
                    System.setErr(captureStream);
                    
                    try {
                        org.jetbrains.kotlin.preloading.Preloader.main(preloaderArgs);
                    } finally {
                        System.setOut(originalOut);
                        System.setErr(originalErr);
                    }
                }
                default -> {
                    System.out.println("[ERROR]|Unknown system tool execution target requested: " + command);
                    taskFailed = true;
                }
            }
        } catch (SecurityException e) {
            // Intercepted exit from D8/R8 indicating an execution failure
            if (e.getMessage() != null && e.getMessage().contains("Intercepted System.exit")) {
                taskFailed = true;
            } else {
                System.setOut(originalOut);
                System.setErr(originalErr);
                System.out.println("[ERROR]|Daemon tracking caught security isolation exception validation fault: " + e.getMessage());
                taskFailed = true;
            }
        } catch (Throwable t) {
            System.setOut(originalOut);
            System.setErr(originalErr);
            System.out.println("[ERROR]|Task compilation pipeline crash monitored within " + command + ": " + t.getMessage());
            t.printStackTrace(captureStream);
            taskFailed = true;
        } finally {
            // Restore standard pipeline outputs safely to preserve daemon protocol stability
            System.setOut(originalOut);
            System.setErr(originalErr);
        }

        // Process and transmit captured compiler error outputs line by line
        String rawDiagnostics = internalLogBuffer.toString();
        if (!rawDiagnostics.isEmpty()) {
            String[] diagnosticLines = rawDiagnostics.split("\\r?\\n");
            for (String diagnosticLine : diagnosticLines) {
                String trimmedLine = diagnosticLine.trim();
                if (trimmedLine.isEmpty()) continue;

                // Squelch Jansi noise from captured compiler streams
                if (trimmedLine.contains("jansi") || 
                    trimmedLine.contains("libjansi") || 
                    trimmedLine.contains("UnsatisfiedLinkError")) {
                    continue;
                }

                System.out.println("[ERROR]|" + trimmedLine);
            }
        }

        // Return the operational status back to the main loop
        return !taskFailed;
    }
}