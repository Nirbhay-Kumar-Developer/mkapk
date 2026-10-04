package com.mkapk.tools;

import com.android.manifmerger.ManifestMerger2;
import com.android.manifmerger.ManifestSystemProperty;
import com.android.manifmerger.MergingReport;
import com.android.utils.ILogger;
import java.io.File;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

public class ManifestMergerHandler implements ToolHandler {

    @Override
    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
        String minSdk = null;
        String targetSdk = null;
        List<String> manifestFiles = new ArrayList<>();

        for (int i = 0; i < args.length; i++) {
            String arg = args[i];
            if ("--min-sdk".equals(arg) && i + 1 < args.length) {
                minSdk = args[++i];
            } else if ("--target-sdk".equals(arg) && i + 1 < args.length) {
                targetSdk = args[++i];
            } else {
                manifestFiles.add(arg);
            }
        }

        if (manifestFiles.size() < 2) {
            errStream.println("Usage: manifestmerger [--min-sdk <val>] [--target-sdk <val>] <mainManifest> <outputManifest> [libraryManifests...]");
            return false;
        }

        File mainManifest = new File(manifestFiles.get(0));
        File outputManifest = new File(manifestFiles.get(1));

        if (minSdk == null || minSdk.trim().isEmpty()) {
            minSdk = "21";
        }
        if (targetSdk == null || targetSdk.trim().isEmpty()) {
            targetSdk = "33";
        }

        ILogger logger = new ILogger() {
            @Override
            public void error(Throwable t, String msgFormat, Object... logArgs) {
                if (msgFormat != null) errStream.printf("[ERROR] " + msgFormat + "%n", logArgs);
                if (t != null) t.printStackTrace(errStream);
            }

            @Override
            public void warning(String msgFormat, Object... logArgs) {
                if (msgFormat != null) outStream.printf("[WARN] " + msgFormat + "%n", logArgs);
            }

            @Override
            public void info(String msgFormat, Object... logArgs) {
                if (msgFormat != null) outStream.printf("[INFO] " + msgFormat + "%n", logArgs);
            }

            @Override
            public void verbose(String msgFormat, Object... logArgs) {
            }
        };

        ManifestMerger2.Invoker invoker = ManifestMerger2.newMerger(
                mainManifest,
                logger,
                ManifestMerger2.MergeType.APPLICATION
        );

        // 1. Enable lenient handling so AGP 9.0 doesn't crash on uses-sdk
        invoker.withFeatures(ManifestMerger2.Invoker.Feature.USES_SDK_IN_MANIFEST_LENIENT_HANDLING);

        // 2. Set exact overrides using the verified ManifestSystemProperty.UsesSdk enum constants
        invoker.setOverride(ManifestSystemProperty.UsesSdk.MIN_SDK_VERSION, minSdk);
        invoker.setOverride(ManifestSystemProperty.UsesSdk.TARGET_SDK_VERSION, targetSdk);

        // 3. Enqueue dependency library manifests
        for (int i = 2; i < manifestFiles.size(); i++) {
            File libFile = new File(manifestFiles.get(i));
            if (libFile.exists()) {
                invoker.addLibraryManifest(libFile);
            }
        }

        MergingReport report = invoker.merge();
        if (report.getResult().isError()) {
            report.log(logger);
            return false;
        }

        String mergedXml = report.getMergedDocument(MergingReport.MergedManifestKind.MERGED);
        if (mergedXml == null || mergedXml.isEmpty()) {
            errStream.println("ManifestMerger Error: Merged document content was null or empty.");
            return false;
        }

        if (outputManifest.getParentFile() != null) {
            outputManifest.getParentFile().mkdirs();
        }

        Files.write(outputManifest.toPath(), mergedXml.getBytes(StandardCharsets.UTF_8));
        return true;
    }
}
