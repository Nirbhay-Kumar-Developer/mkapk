package com.mkapk.tools.ipc;

import com.google.gson.Gson;

public class IpcMessage {
    private static final Gson GSON = new Gson();

    public String command = "";
    public String[] args = new String[0];

    /**
     * Deserializes a JSON payload sent over the pipe using Gson.
     * Handles string escaping, nested arrays, and quote boundaries automatically.
     *
     * @param json The raw JSON string sent over the pipe
     * @return Parsed IpcMessage instance with guaranteed non-null fields
     */
    public static IpcMessage fromJson(String json) {
        if (json == null || json.trim().isEmpty()) {
            return new IpcMessage();
        }

        IpcMessage msg = GSON.fromJson(json, IpcMessage.class);
        if (msg == null) {
            return new IpcMessage();
        }

        // Guarantee non-null invariants if fields were omitted in JSON payload
        if (msg.command == null) {
            msg.command = "";
        }
        if (msg.args == null) {
            msg.args = new String[0];
        }

        return msg;
    }

    /**
     * Serializes this message to a JSON string.
     */
    public String toJson() {
        return GSON.toJson(this);
    }
}
