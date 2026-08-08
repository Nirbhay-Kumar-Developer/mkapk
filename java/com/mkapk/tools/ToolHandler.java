package com.mkapk.tools;

import java.io.PrintStream;

public interface ToolHandler {
    /**
     * Executes the tool.
     * @param args The arguments passed from the C++ coordinator.
     * @param outStream The intercepted output stream for standard logs.
     * @param errStream The intercepted error stream.
     * @return true if successful, false otherwise.
     */
    boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception;
}
