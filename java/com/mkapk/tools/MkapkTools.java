package com.mkapk.tools;

import java.io.*;
import java.util.Arrays;
import java.util.Scanner;
import java.security.Permission;

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

    public static void main(String[] args) {
        // Handle immediate CLI usage (e.g. java -jar mkapk-coordinator.jar javac)
        if (args.length > 0 && !args[0].equals("START_DAEMON")) {
            handleTask(args[0].toLowerCase(), Arrays.copyOfRange(args, 1, args.length));
            return;
        }

        // Setup the exit interceptor
        preventExit();

        // Standard Input Loop (Pipe Communication with C++)
        try (Scanner scanner = new Scanner(System.in)) {
            while (scanner.hasNextLine()) {
                String input = scanner.nextLine();
                if (input == null || input.trim().isEmpty()) continue;

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
                        return; // Exit main and close JVM
                    }
                    default -> {
                        if (isRunning) {
                            handleTask(command.toLowerCase(), toolArgs);
                            // Signal that the specific task (javac/d8/etc) is finished
                            System.out.println("MKAPK_TASK_DONE");
                        } else {
                            System.out.println("[ERROR]|Disregarded command transaction. Transmission of START_DAEMON handshake verification token required first.");
                        }
                    }
                }
                System.out.flush();
            }
        } catch (Exception e) {
            // Pipe likely broken or closed by parent native process
        }
    }

    private static void handleTask(String command, String[] args) {
        boolean taskFailed = false;

        // Structured buffer to intercept underlying standard console stream streams
        ByteArrayOutputStream internalLogBuffer = new ByteArrayOutputStream();
        PrintStream captureStream = new PrintStream(internalLogBuffer);

        // Retain normal process streams references
        PrintStream originalOut = System.out;
        PrintStream originalErr = System.err;

        try {
            switch (command) {
                case "javac" -> {
                    // javac allows passing a specialized stream writer for diagnostic capturing
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
                case "kotlinc" -> {
                    // Query localized context environment properties dynamically
                    String termuxPrefix = System.getenv("PREFIX");
                    if (termuxPrefix == null) {
                        termuxPrefix = "/data/data/com.termux/files/usr";
                    }

                    // Traverse paths matching your target cross-compiler installation root folder layout
                    String kotlinHome = termuxPrefix + "/opt/kotlin";

                    String compilerJar = kotlinHome + "/lib/kotlin-compiler.jar";
                    String compilerClass = "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler";

                    // Reconstruct the exact argument structure expected by the Kotlin Preloader:
                    String[] preloaderArgs = new String[3 + args.length];
                    preloaderArgs[0] = "-cp";
                    preloaderArgs[1] = compilerJar;
                    preloaderArgs[2] = compilerClass;
                    System.arraycopy(args, 0, preloaderArgs, 3, args.length);

                    // Redirect system execution streams to our diagnostic logs capture buffer
                    System.setOut(captureStream);
                    System.setErr(captureStream);
                    
                    try {
                        // Invoke Kotlin's Preloader entry point directly
                        org.jetbrains.kotlin.preloading.Preloader.main(preloaderArgs);
                    } finally {
                        // Restore straight to original references to guard IPC daemon boundary stability
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
            if (e.getMessage().contains("Intercepted System.exit")) {
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

                // SQUELCH JANSI NOISE FROM CAPTURED COMPILER STREAMS
                if (trimmedLine.contains("jansi") || 
                    trimmedLine.contains("libjansi") || 
                    trimmedLine.contains("UnsatisfiedLinkError")) {
                    continue;
                }

                System.out.println("[ERROR]|" + trimmedLine);
            }
        }

        // Trigger SIGKILL to parent if things went south
        if (taskFailed) {
            killParentProcess();
        }
    }

    /**
     * Finds the PPID of this Java process and aggressively kills it.
     */
    private static void killParentProcess() {
        ProcessHandle.current().parent().ifPresent(parent -> {
            long ppid = parent.pid();
            System.out.println("[ERROR]|Fatal runtime operation drop flags caught inside daemon tools. Forcing parent process tree breakdown: " + ppid);
            System.out.flush();
            
            try {
                String os = System.getProperty("os.name").toLowerCase();
                ProcessBuilder pb;
                
                if (os.contains("win")) {
                    // Windows forceful termination
                    pb = new ProcessBuilder("taskkill", "/F", "/PID", String.valueOf(ppid));
                } else {
                    // Unix/Linux/macOS SIGKILL (Signal 9)
                    pb = new ProcessBuilder("kill", "-9", String.valueOf(ppid));
                }
                
                // Fire and forget, parent process will die instantly
                pb.start();
                
            } catch (IOException e) {
                // Parent already inaccessible
            }
        });
    }
}