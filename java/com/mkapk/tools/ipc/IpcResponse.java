package com.mkapk.tools.ipc;

public class IpcResponse {
    
    public static String handshakeOk() {
        return "{\"type\":\"HANDSHAKE_OK\"}";
    }

    public static String done() {
        return "{\"type\":\"DONE\"}";
    }

    public static String failed() {
        return "{\"type\":\"FAILED\"}";
    }

    public static String log(String level, String message) {
        return "{\"type\":\"LOG\",\"level\":\"" + level + "\",\"message\":\"" + escapeJson(message) + "\"}";
    }

    public static String data(String payload) {
        return "{\"type\":\"DATA\",\"payload\":\"" + escapeJson(payload) + "\"}";
    }

    private static String escapeJson(String text) {
        if (text == null) return "";
        return text.replace("\\", "\\\\")
                   .replace("\"", "\\\"")
                   .replace("\n", "\\n")
                   .replace("\r", "");
    }
}