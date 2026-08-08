package com.mkapk.tools.ipc;

import java.util.ArrayList;
import java.util.List;

public class IpcMessage {
    public String command = "";
    public String[] args = new String[0];

    /**
     * Dependency-free lightweight JSON tokenizer.
     * Expects standard C++ nlohmann::json flat object layout.
     * @param json The raw JSON string sent over the pipe
     * @return Parsed IpcMessage object
     */
    public static IpcMessage fromJson(String json) {
        IpcMessage msg = new IpcMessage();
        msg.command = extractString(json, "\"command\"");
        
        List<String> parsedArgs = new ArrayList<>();
        int arrayStart = json.indexOf("\"args\":[");
        if (arrayStart != -1) {
            int current = arrayStart + 8; // skip past "[
            while (current < json.length()) {
                // Find next string
                int startQuote = json.indexOf('"', current);
                if (startQuote == -1) break; // Array end
                
                int endQuote = startQuote + 1;
                while (endQuote < json.length()) {
                    // Check for end quote, ignoring escaped quotes
                    if (json.charAt(endQuote) == '"' && json.charAt(endQuote - 1) != '\\') {
                        break;
                    }
                    endQuote++;
                }

                parsedArgs.add(unescapeJson(json.substring(startQuote + 1, endQuote)));
                
                // Move cursor to next element or end of array
                current = json.indexOf(',', endQuote);
                int arrayEnd = json.indexOf(']', endQuote);
                
                // If there are no more commas, or the array ends before the next comma, break
                if (current == -1 || (arrayEnd != -1 && arrayEnd < current)) {
                    break;
                }
            }
        }
        msg.args = parsedArgs.toArray(new String[0]);
        return msg;
    }

    /**
     * Extracts a top-level string value for a given key.
     */
    private static String extractString(String json, String key) {
        int keyIdx = json.indexOf(key);
        if (keyIdx == -1) return "";
        
        int startQuote = json.indexOf('"', keyIdx + key.length() + 1);
        if (startQuote == -1) return "";
        
        int endQuote = startQuote + 1;
        while (endQuote < json.length()) {
            if (json.charAt(endQuote) == '"' && json.charAt(endQuote - 1) != '\\') {
                break;
            }
            endQuote++;
        }
        return unescapeJson(json.substring(startQuote + 1, endQuote));
    }

    /**
     * Cleans up standard JSON escape sequences.
     */
    private static String unescapeJson(String text) {
        if (text == null) return "";
        return text.replace("\\\"", "\"")
                   .replace("\\n", "\n")
                   .replace("\\\\", "\\");
    }
}