package com.mkapk.tools;

import com.android.manifmerger.ManifestMerger2;
import com.android.manifmerger.ManifestSystemProperty;
import com.android.manifmerger.MergingReport;
import com.android.utils.ILogger;
import org.w3c.dom.Document;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;
import org.xml.sax.InputSource;

import javax.xml.parsers.DocumentBuilder;
import javax.xml.parsers.DocumentBuilderFactory;
import javax.xml.transform.OutputKeys;
import javax.xml.transform.Transformer;
import javax.xml.transform.TransformerFactory;
import javax.xml.transform.dom.DOMSource;
import javax.xml.transform.stream.StreamResult;
import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.PrintStream;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class ManifestMergerHandler implements ToolHandler {

    private static final String ANDROID_NS = "http://schemas.android.com/apk/res/android";

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
            public void verbose(String msgFormat, Object... logArgs) {}
        };

        ManifestMerger2.Invoker invoker = ManifestMerger2.newMerger(
                mainManifest,
                logger,
                ManifestMerger2.MergeType.APPLICATION
        );

        invoker.withFeatures(ManifestMerger2.Invoker.Feature.USES_SDK_IN_MANIFEST_LENIENT_HANDLING);
        invoker.setOverride(ManifestSystemProperty.UsesSdk.MIN_SDK_VERSION, minSdk);
        invoker.setOverride(ManifestSystemProperty.UsesSdk.TARGET_SDK_VERSION, targetSdk);

        List<File> activeJarFiles = new ArrayList<>();
        for (int i = 2; i < manifestFiles.size(); i++) {
            File libFile = new File(manifestFiles.get(i));
            if (libFile.exists()) {
                invoker.addLibraryManifest(libFile);

                // Collect active library bytecode jars (classes.jar alongside AndroidManifest.xml)
                File parentDir = libFile.getParentFile();
                if (parentDir != null) {
                    File classesJar = new File(parentDir, "classes.jar");
                    if (classesJar.exists()) {
                        activeJarFiles.add(classesJar);
                    }
                }
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

        // Fast O(1) pruning of unresolvable library components
        String filteredXml = pruneUnresolvedComponents(mergedXml, activeJarFiles, outStream);

        if (outputManifest.getParentFile() != null) {
            outputManifest.getParentFile().mkdirs();
        }

        Files.write(outputManifest.toPath(), filteredXml.getBytes(StandardCharsets.UTF_8));
        return true;
    }

    private String pruneUnresolvedComponents(String xmlContent, List<File> activeJarFiles, PrintStream outStream) {
        try {
            DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
            factory.setNamespaceAware(true);
            DocumentBuilder builder = factory.newDocumentBuilder();
            Document doc = builder.parse(new InputSource(new ByteArrayInputStream(xmlContent.getBytes(StandardCharsets.UTF_8))));

            Element root = doc.getDocumentElement();
            String appPackage = root.getAttribute("package");

            NodeList appList = root.getElementsByTagName("application");
            if (appList.getLength() == 0) return xmlContent;

            Element appElement = (Element) appList.item(0);
            List<String> componentTags = Arrays.asList("provider", "activity", "service", "receiver");

            Map<String, Boolean> existenceCache = new HashMap<>();

            for (String tag : componentTags) {
                NodeList nodes = appElement.getElementsByTagName(tag);
                for (int i = nodes.getLength() - 1; i >= 0; i--) {
                    Element component = (Element) nodes.item(i);
                    String className = getAttribute(component, "name");

                    if (className == null || className.isEmpty()) continue;

                    // Standard project classes belong to the app source; do not prune
                    if (className.startsWith(".") || (appPackage != null && className.startsWith(appPackage))) {
                        continue;
                    }

                    // For external library components, verify existence via O(1) zip lookup
                    if (!isClassPresent(className, activeJarFiles, existenceCache)) {
                        outStream.println("[INFO] ManifestMerger: Pruning unreferenced <" + tag + " android:name=\"" + className + "\"> (optional dependency not on classpath)");
                        component.getParentNode().removeChild(component);
                    }
                }
            }

            // Clean unreferenced startup Initializer meta-data
            NodeList metaList = appElement.getElementsByTagName("meta-data");
            for (int i = metaList.getLength() - 1; i >= 0; i--) {
                Element meta = (Element) metaList.item(i);
                String metaValue = getAttribute(meta, "value");
                if ("androidx.startup".equals(metaValue)) {
                    String metaName = getAttribute(meta, "name");
                    if (metaName != null && !isClassPresent(metaName, activeJarFiles, existenceCache)) {
                        meta.getParentNode().removeChild(meta);
                    }
                }
            }

            Transformer transformer = TransformerFactory.newInstance().newTransformer();
            transformer.setOutputProperty(OutputKeys.INDENT, "yes");
            transformer.setOutputProperty(OutputKeys.ENCODING, "UTF-8");
            transformer.setOutputProperty("{http://xml.apache.org/xslt}indent-amount", "4");

            StringWriter writer = new StringWriter();
            transformer.transform(new DOMSource(doc), new StreamResult(writer));
            return writer.toString();

        } catch (Exception e) {
            outStream.println("[WARN] ManifestMerger: Component pruning skipped: " + e.getMessage());
            return xmlContent;
        }
    }

    /**
     * O(1) check in active JAR central directories.
     */
    private boolean isClassPresent(String className, List<File> activeJarFiles, Map<String, Boolean> existenceCache) {
        if (existenceCache.containsKey(className)) {
            return existenceCache.get(className);
        }

        String classEntry = className.replace('.', '/') + ".class";
        boolean found = false;

        for (File jar : activeJarFiles) {
            try (ZipFile zip = new ZipFile(jar)) {
                ZipEntry entry = zip.getEntry(classEntry);
                if (entry != null) {
                    found = true;
                    break;
                }
            } catch (Exception ignored) {}
        }

        existenceCache.put(className, found);
        return found;
    }

    private String getAttribute(Element element, String localName) {
        if (element.hasAttributeNS(ANDROID_NS, localName)) {
            return element.getAttributeNS(ANDROID_NS, localName);
        }
        return element.getAttribute("android:" + localName);
    }
}
