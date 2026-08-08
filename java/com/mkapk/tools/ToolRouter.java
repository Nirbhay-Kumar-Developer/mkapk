package com.mkapk.tools;

import com.mkapk.tools.ipc.IpcResponse;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.PrintStream;
import java.net.URL;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

public class ToolRouter {
    
    // Persistent pool tracking dynamically downloaded dependencies across IPC build loops
    private final Set<URL> dynamicClassPathUrls = Collections.synchronizedSet(new LinkedHashSet<>());

    /**
     * Routes the command to the appropriate handler and captures all diagnostics.
     * @param command The tool identifier (e.g., "javac", "d8", "resolve")
     * @param args The command-line arguments for the tool
     * @return true if successful, false if a fatal error occurred.
     */
    public boolean routeAndExecute(String command, String[] args) {
        boolean taskFailed = false;

        // Structured buffer to intercept underlying standard console streams
        ByteArrayOutputStream internalLogBuffer = new ByteArrayOutputStream();
        PrintStream captureStream = new PrintStream(internalLogBuffer);

        // Retain normal process stream references
        PrintStream originalOut = System.out;
        PrintStream originalErr = System.err;

        // Strip path directory prefixes if C++ passed a full file path (e.g., /usr/bin/d8 -> d8)
        if (command.contains("/") || command.contains("\\")) {
            command = new File(command).getName();
        }
        command = command.toLowerCase().trim();

        // Strategy selection mapping string commands to specific handlers
        ToolHandler handler = switch (command) {
            case "javac"          -> new JavacHandler(dynamicClassPathUrls);
            case "d8"             -> new SimpleToolHandler("com.android.tools.r8.D8");
            case "r8"             -> new SimpleToolHandler("com.android.tools.r8.R8");
            case "resguard"       -> new SimpleToolHandler("com.tencent.mm.resourceproguard.cli.CliMain");
            case "apksigner"      -> new SimpleToolHandler("com.android.apksigner.ApkSignerTool");
            case "kotlinc"        -> new KotlinHandler();
            default               -> null;
        };

        if (handler == null) {
            originalOut.println(IpcResponse.log("ERROR", "Unknown system tool execution target requested: " + command));
            return false;
        }

        try {
            boolean success = handler.execute(args, captureStream, captureStream);
            if (!success) {
                taskFailed = true;
            }
        } catch (MkapkTools.ExitInterceptedException e) {
            // Exit code 0 indicates clean/successful process execution
            if (e.status != 0) {
                taskFailed = true;
            }
        } catch (SecurityException e) {
            // Intercepted exit fallback from tools indicating an execution failure boundary
            if (e.getMessage() != null && e.getMessage().contains("Intercepted System.exit")) {
                if (!e.getMessage().contains("System.exit(0)")) {
                    taskFailed = true;
                }
            } else {
                System.setOut(originalOut);
                System.setErr(originalErr);
                originalOut.println(IpcResponse.log("ERROR", "Security isolation fault: " + e.getMessage()));
                taskFailed = true;
            }
        } catch (Throwable t) {
            System.setOut(originalOut);
            System.setErr(originalErr);
            originalOut.println(IpcResponse.log("ERROR", "Pipeline crash within " + command + ": " + t.getMessage()));
            t.printStackTrace(captureStream);
            taskFailed = true;
        } finally {
            // Restore standard pipeline outputs safely to preserve daemon protocol stability
            System.setOut(originalOut);
            System.setErr(originalErr);
        }

        // Process and transmit captured compiler error outputs as JSON via originalOut
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

                // Map legacy resolver string format to new JSON DATA payload, route others to logs
                if (trimmedLine.startsWith("MKAPK_RESOLVED|")) {
                    originalOut.println(IpcResponse.data(trimmedLine.substring(15)));
                } else if (trimmedLine.startsWith("[ERROR]|")) {
                    originalOut.println(IpcResponse.log("ERROR", trimmedLine.substring(8)));
                } else if (trimmedLine.startsWith("[WARN]|")) {
                    originalOut.println(IpcResponse.log("WARN", trimmedLine.substring(7)));
                } else {
                    originalOut.println(IpcResponse.log("INFO", trimmedLine));
                }
            }
        }

        return !taskFailed;
    }
}