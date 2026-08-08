package com.mkapk.tools;

import com.mkapk.tools.ipc.IpcMessage;
import com.mkapk.tools.ipc.IpcResponse;

import java.io.BufferedReader;
import java.io.InputStreamReader;

public class DaemonServer {
    private boolean isRunning = false;
    private final ToolRouter router;

    public DaemonServer() {
        this.router = new ToolRouter();
    }

    public void start() {
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(System.in))) {
            String input;
            
            while ((input = reader.readLine()) != null) {
                if (input.trim().isEmpty()) continue;

                IpcMessage msg;
                try {
                    msg = IpcMessage.fromJson(input);
                } catch (Exception e) {
                    System.out.println(IpcResponse.log("ERROR", "JSON IPC Parse Error: " + e.getMessage()));
                    System.out.flush();
                    continue;
                }

                if ("START_DAEMON".equalsIgnoreCase(msg.command)) {
                    isRunning = true;
                    System.out.println(IpcResponse.handshakeOk());
                } 
                else if ("STOP_DAEMON".equalsIgnoreCase(msg.command)) {
                    isRunning = false;
                    return; // Exit the server loop cleanly
                } 
                else {
                    if (isRunning) {
                        boolean success = router.routeAndExecute(msg.command, msg.args);
                        
                        // Signal the specific outcome to the C++ orchestrator
                        if (success) {
                            System.out.println(IpcResponse.done());
                        } else {
                            System.out.println(IpcResponse.failed());
                        }
                    } else {
                        System.out.println(IpcResponse.log("ERROR", "Disregarded transaction. Missing START_DAEMON handshake."));
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
}
