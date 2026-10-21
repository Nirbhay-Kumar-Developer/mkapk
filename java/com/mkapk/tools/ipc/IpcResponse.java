package com.mkapk.tools.ipc;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonObject;

public class IpcResponse {
    private static final Gson GSON = new GsonBuilder()
            .disableHtmlEscaping()
            .create();

    public static String handshakeOk() {
        JsonObject obj = new JsonObject();
        obj.addProperty("type", "HANDSHAKE_OK");
        return GSON.toJson(obj);
    }

    public static String done() {
        JsonObject obj = new JsonObject();
        obj.addProperty("type", "DONE");
        return GSON.toJson(obj);
    }

    public static String failed() {
        JsonObject obj = new JsonObject();
        obj.addProperty("type", "FAILED");
        return GSON.toJson(obj);
    }

    public static String log(String level, String message) {
        JsonObject obj = new JsonObject();
        obj.addProperty("type", "LOG");
        obj.addProperty("level", level != null ? level : "INFO");
        obj.addProperty("message", message != null ? message : "");
        return GSON.toJson(obj);
    }

    public static String data(String payload) {
        JsonObject obj = new JsonObject();
        obj.addProperty("type", "DATA");
        obj.addProperty("payload", payload != null ? payload : "");
        return GSON.toJson(obj);
    }
}
