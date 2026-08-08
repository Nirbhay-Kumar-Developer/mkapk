package com.mkapk.tools;

import java.security.Permission;
import java.util.Arrays;

public class MkapkTools {

    /**
     * Custom exception to carry status code when intercepting System.exit calls.
     */
    public static class ExitInterceptedException extends SecurityException {
        public final int status;
        public ExitInterceptedException(int status) {
            super("Intercepted System.exit(" + status + ")");
            this.status = status;
        }
    }

    /**
     * Prevents tools like D8/R8/ManifestMerger from killing the JVM process.
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
                    // Intercept exit calls from Android tools and preserve exit status
                    throw new ExitInterceptedException(status);
                }
            });
        } catch (UnsupportedOperationException e) {
            // Forward a structured JSON warning over standard out for the C++ IPC client
            System.out.println("{\"type\":\"LOG\",\"level\":\"WARN\",\"message\":\"Security Manager restricted. Ensure JVM execution args include -Djava.security.manager=allow\"}");
        }
    }

    public static void main(String[] args) {
        // Handle immediate CLI usage bypassing the Daemon loop (e.g. java -jar mkapk-coordinator.jar javac)
        if (args.length > 0 && !args[0].equals("START_DAEMON")) {
            ToolRouter fallbackRouter = new ToolRouter();
            boolean success = fallbackRouter.routeAndExecute(args[0], Arrays.copyOfRange(args, 1, args.length));
            System.exit(success ? 0 : 1);
            return;
        }

        // Setup the exit interceptor
        preventExit();

        // Boot up the dedicated Server Loop
        DaemonServer server = new DaemonServer();
        server.start();
    }
}