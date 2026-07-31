package com.mkapk.tools;

import java.io.File;
import java.io.PrintStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;

public class ManifestMergerHandler implements ToolHandler {

    @Override
    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
        System.setOut(outStream);
        System.setErr(errStream);

        if (args.length < 2) {
            outStream.println("[ERROR]|ManifestMerger requires at least 2 arguments: mainManifest and outputManifest.");
            return false;
        }

        File mainManifest = new File(args[0]);
        File outFile = new File(args[1]);

        if (outFile.getParentFile() != null) {
            outFile.getParentFile().mkdirs();
        }

        // FIX: NO LIBRARIES TO MERGE: Fast copy main manifest directly to destination
        if (args.length == 2) {
            Files.copy(mainManifest.toPath(), outFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
            return outFile.exists() && outFile.length() > 0;
        }

        List<String> mergerArgs = new ArrayList<>();
        mergerArgs.add("--main");
        mergerArgs.add(args[0]);
        mergerArgs.add("--out");
        mergerArgs.add(args[1]);

        StringBuilder libsBuilder = new StringBuilder();
        for (int i = 2; i < args.length; i++) {
            if (args[i] != null && !args[i].trim().isEmpty()) {
                libsBuilder.append(args[i]);
                if (i < args.length - 1) {
                    libsBuilder.append(File.pathSeparator);
                }
            }
        }

        if (libsBuilder.length() > 0) {
            mergerArgs.add("--libs");
            mergerArgs.add(libsBuilder.toString());
        }

        try {
            com.android.manifmerger.Merger.main(mergerArgs.toArray(new String[0]));
        } catch (MkapkTools.ExitInterceptedException e) {
            if (e.status != 0) {
                outStream.println("[ERROR]|ManifestMerger exited with status code: " + e.status);
                return false;
            }
        } catch (Throwable t) {
            outStream.println("[ERROR]|ManifestMerger exception: " + t.getMessage());
            return false;
        }

        return outFile.exists() && outFile.length() > 0;
    }
}