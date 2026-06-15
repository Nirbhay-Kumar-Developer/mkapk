package com.mkapk.tools;

import java.io.*;
import java.util.Arrays;
import java.util.Scanner;
import java.security.Permission;

/**
 * mkapk Java Daemon - High Performance Persistent Worker
 * Permanent Fix: Bypasses JNI memory tagging issues by running as a separate process.
 * Intercepts System.exit() to keep the JVM alive between build tasks.
 */
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
            // This happens on Java 18+ if the 'allow' flag is missing.
            // We print a warning but allow the daemon to try and run anyway.
            System.err.println("!! [DAEMON-WARN] Security Manager restricted.");
            System.err.println("!! Ensure JVM is started with -Djava.security.manager=allow");
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
                            System.err.println("!! [DAEMON] Ignore: Send START_DAEMON first.");
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
        try {
            switch (command) {
                case "javac" -> {
                    int result = com.sun.tools.javac.Main.compile(args);
                    if (result != 0) {
                        System.err.println("!! [JAVAC] Compilation failed.");
                        taskFailed = true;
                    }
                }
                case "d8" -> com.android.tools.r8.D8.main(args);
                case "r8" -> com.android.tools.r8.R8.main(args);
                case "resguard" -> com.tencent.mm.resourceproguard.cli.CliMain.main(args);
                case "apksigner" -> com.android.apksigner.ApkSignerTool.main(args);
                default -> System.err.println("!! [DAEMON] Unknown tool: " + command);
            }
        } catch (SecurityException e) {
            // Intercepted exit from D8/R8 indicating an execution failure
            if (e.getMessage().contains("Intercepted System.exit")) {
                taskFailed = true;
            } else {
                e.printStackTrace();
                taskFailed = true;
            }
        } catch (Throwable t) {
            System.err.println("!! [DAEMON] Task failure in " + command + ": " + t.getMessage());
            t.printStackTrace();
            taskFailed = true;
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
            System.err.println("!! [DAEMON-FATAL] Tool error/crash detected. Force-killing PPID: " + ppid);
            
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
                System.err.println("!! [DAEMON] Failed to issue kill command to PPID: " + e.getMessage());
            }
        });
    }
}