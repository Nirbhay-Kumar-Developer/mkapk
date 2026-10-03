diff --git a/.github/workflows/codeql.yml b/.github/workflows/codeql.yml
index 62dae07..a75e171 100644
--- a/.github/workflows/codeql.yml
+++ b/.github/workflows/codeql.yml
@@ -3,9 +3,9 @@ name: "CodeQL Advanced & Linting Pipeline"
 
 "on":
   push:
-    branches: ["main"]
+    branches: ["Dependency-Resolver"]
   pull_request:
-    branches: ["main"]
+    branches: ["Dependency-Resolver"]
   schedule:
     - cron: '18 21 * * 2'
 
@@ -129,4 +129,3 @@ jobs:
         uses: github/codeql-action/upload-sarif@v4
         with:
           sarif_file: 'megalinter-reports/megalinter-report.sarif'
-          
\ No newline at end of file
diff --git a/.gitignore b/.gitignore
index 06cfc51..8123b07 100644
--- a/.gitignore
+++ b/.gitignore
@@ -1,5 +1,8 @@
 build/
+test/
 bundle.sh
 bundle.cpp
-test/,
+test
 compile_commands.json
+to-do.txt
+.cache
diff --git a/include/mkapk_config.hpp b/include/mkapk_config.hpp
index 5a7fe90..d901878 100644
--- a/include/mkapk_config.hpp
+++ b/include/mkapk_config.hpp
@@ -50,4 +50,4 @@ namespace MkapkEnv {
     MkapkConfig load_config(const std::filesystem::path& config_path = std::filesystem::current_path() / "config.json");
 }
 
-#endif
+#endif
\ No newline at end of file
diff --git a/include/mkapk_extractor.hpp b/include/mkapk_extractor.hpp
new file mode 100644
index 0000000..e77722a
--- /dev/null
+++ b/include/mkapk_extractor.hpp
@@ -0,0 +1,26 @@
+#ifndef MKAPK_EXTRACTOR_HPP
+#define MKAPK_EXTRACTOR_HPP
+
+#include <string>
+#include <vector>
+
+namespace MkapkExtractor {
+    /**
+     * Extracts an AAR file to the localized $PREFIX cache folder structure.
+     * Maps AndroidManifest.xml, classes.jar, and the res/ directory tree cleanly.
+     * 
+     * @param aar_path Absolute path to the cached .aar file.
+     * @return true if extraction succeeded, false otherwise.
+     */
+    bool extract_aar(const std::string& aar_path);
+
+    /**
+     * Iterates over a list of resolved artifact paths, identifies .aar files,
+     * and triggers safe extraction.
+     * 
+     * @param resolved_paths Vector containing absolute paths of resolved .aar/.jar files.
+     */
+    void extract_all(const std::vector<std::string>& resolved_paths);
+}
+
+#endif // MKAPK_EXTRACTOR_HPP
\ No newline at end of file
diff --git a/include/mkapk_helpers.hpp b/include/mkapk_helpers.hpp
index d3910ff..47b157c 100644
--- a/include/mkapk_helpers.hpp
+++ b/include/mkapk_helpers.hpp
@@ -27,24 +27,6 @@ namespace MkapkEnv {
     std::string get_jni_classpath(const MkapkConfig& config);
     bool init_project();
     bool run_system_cmd(const std::vector<std::string>& args);
-    
-    // --- EXTENSIBLE PACKAGE MANAGEMENT PLUGIN FRAMEWORK SYSTEM ---
-    
-    /**
-     * Unpacks, verifies cryptographic signature records, installs dependencies 
-     * via Termux apt, and writes plugin definitions safely to persistent cache storage.
-     */
-    bool install_plugin(const std::string& pl_package_path);
-
-    /**
-     * Removes structural configuration footprints and clears the plugin from the storage cache registry.
-     */
-    bool uninstall_plugin(const std::string& plugin_name);
-
-    /**
-     * Scans and initializes the active collection of LanguagePlugin objects from cache directory files
-     */
-     std::map<std::string, LanguagePlugin> load_installed_plugins();
 }
 
 void cleanup_stale_assets(
@@ -62,7 +44,8 @@ Result<std::pair<fs::path, fs::path>> compile_source_logic(
     std::map<std::string, std::vector<fs::path>>& changed_files,
     std::map<std::string, std::vector<fs::path>>& deleted_files,
     bool do_res,
-    RunFunc run
+    RunFunc run,
+    const std::vector<fs::path>& extra_jvm_classpaths
 );
 
 Result<void> start_daemon(const std::string& classpath);
diff --git a/include/mkapk_manifest_merger.hpp b/include/mkapk_manifest_merger.hpp
new file mode 100644
index 0000000..a9ff081
--- /dev/null
+++ b/include/mkapk_manifest_merger.hpp
@@ -0,0 +1,25 @@
+#ifndef MKAPK_MANIFEST_MERGER_HPP
+#define MKAPK_MANIFEST_MERGER_HPP
+
+#include <string>
+#include <vector>
+#include "mkapk_config.hpp"
+
+namespace MkapkManifestMerger {
+    /**
+     * Gathers all dependency manifests, runs the programmatic Java Merger,
+     * and writes the validated result directly to the build/ directory.
+     * 
+     * @param main_manifest Path to the primary AndroidManifest.xml (e.g., /app/src/main/AndroidManifest.xml)
+     * @param output_manifest Path to place the merged build output (e.g., /app/build/AndroidManifest.xml)
+     * @param resolved_paths List of resolved absolute paths for jar/aar dependencies returned by the resolver.
+     * @return true if merging succeeded and output manifest exists on disk, false otherwise.
+     */
+    bool merge_manifests(
+        const std::string& main_manifest,
+        const std::string& output_manifest,
+        const std::vector<std::string>& resolved_paths
+    );
+}
+
+#endif // MKAPK_MANIFEST_MERGER_HPP
\ No newline at end of file
diff --git a/include/mkapk_plugin_manager.hpp b/include/mkapk_plugin_manager.hpp
new file mode 100644
index 0000000..90de367
--- /dev/null
+++ b/include/mkapk_plugin_manager.hpp
@@ -0,0 +1,36 @@
+#ifndef MKAPK_PLUGIN_MANAGER_HPP
+#define MKAPK_PLUGIN_MANAGER_HPP
+
+#include <string>
+#include <map>
+#include "mkapk_tools.hpp" // Required for the LanguagePlugin struct definition
+
+namespace MkapkPluginManager {
+
+    /**
+     * Unpacks, cryptographically validates, resolves dependencies via apt,
+     * and writes verified plugin definitions to the storage cache registry.
+     * 
+     * @param pl_package_path The path to the downloaded .pl bundle.
+     * @return true if successful, false otherwise.
+     */
+    bool install_plugin(const std::string& pl_package_path);
+
+    /**
+     * Clears systemic structural cache footprints of an isolated plugin safely.
+     * 
+     * @param plugin_name The identifier of the plugin to remove.
+     * @return true if successful, false otherwise.
+     */
+    bool uninstall_plugin(const std::string& plugin_name);
+
+    /**
+     * Scans and initializes the active collection of LanguagePlugin objects from cache directory files.
+     * 
+     * @return A map of source extensions (e.g., ".java") to their LanguagePlugin configurations.
+     */
+    std::map<std::string, LanguagePlugin> load_installed_plugins();
+
+}
+
+#endif // MKAPK_PLUGIN_MANAGER_HPP
\ No newline at end of file
diff --git a/include/mkapk_resolver.hpp b/include/mkapk_resolver.hpp
new file mode 100644
index 0000000..71d9908
--- /dev/null
+++ b/include/mkapk_resolver.hpp
@@ -0,0 +1,20 @@
+#ifndef MKAPK_RESOLVER_HPP
+#define MKAPK_RESOLVER_HPP
+
+#include <string>
+#include <vector>
+#include "mkapk_config.hpp"
+
+namespace MkapkResolver {
+    /**
+     * Spawns a dedicated connection loop to the Java Daemon, requests 
+     * recursive dependency graph resolution, and translates absolute file paths.
+     * 
+     * @param coordinates The target Maven strings matrix
+     * @param config The structural configuration reference container.
+     * @return A vector list containing paths to cached on-device .aar and .jar artifacts.
+     */
+    std::vector<std::string> resolve_dependencies(const std::vector<std::string>& coordinates, const MkapkConfig& config);
+}
+
+#endif // MKAPK_RESOLVER_HPP
\ No newline at end of file
diff --git a/include/mkapk_tools.hpp b/include/mkapk_tools.hpp
index e5b3040..d1e8ee4 100644
--- a/include/mkapk_tools.hpp
+++ b/include/mkapk_tools.hpp
@@ -61,7 +61,8 @@ Result<void> compile_incremental_java(
     const fs::path& android_jar,
     const fs::path& out_dir,
     const std::vector<fs::path>& changed_files,
-    RunFunc run_func
+    RunFunc run_func,
+    const std::vector<fs::path>& extra_dependency_jars = {}
 );
 
 Result<void> compile_incremental_kotlin(
@@ -70,6 +71,16 @@ Result<void> compile_incremental_kotlin(
     const fs::path& classes_dir,
     const std::vector<fs::path>& changed_files,
     RunFunc run_func,
+    const std::string& compose_plugin = "",
+    const std::vector<std::string>& classpath_extra = {}
+);
+
+Result<void> compile_kotlin(
+    const std::string& KOTLINC,
+    const fs::path& android_jar,
+    const fs::path& classes_dir,
+    const fs::path& src_dir,
+    RunFunc run_func,
     const std::string& compose_plugin = ""
 );
 
@@ -79,7 +90,8 @@ Result<void> compile_resources(
     const fs::path& res_dir,
     const fs::path& bin_dir,
     RunFunc run_func,
-    const std::vector<fs::path>* changed_res_files = nullptr
+    const std::vector<fs::path>* changed_res_files = nullptr,
+    const std::vector<fs::path>& exta_dependency_res_dirs = {}
 );
 
 Result<void> link_manifest(
@@ -137,6 +149,7 @@ Result<void> run_incremental_dex(
     const fs::path& java_out,
     const fs::path& dex_cache,
     const std::vector<fs::path>& files_to_dex,
+    const std::vector<fs::path>& extra_jvm_classpaths,
     RunFunc run
 );
 
diff --git a/java/com/mkapk/tools/Booter.java b/java/com/mkapk/tools/Booter.java
new file mode 100644
index 0000000..58cfe81
--- /dev/null
+++ b/java/com/mkapk/tools/Booter.java
@@ -0,0 +1,98 @@
+package com.mkapk.tools;
+
+import org.apache.maven.repository.internal.MavenRepositorySystemUtils;
+import org.eclipse.aether.DefaultRepositorySystemSession;
+import org.eclipse.aether.RepositorySystem;
+import org.eclipse.aether.RepositorySystemSession;
+import org.eclipse.aether.artifact.DefaultArtifactType;
+import org.eclipse.aether.connector.basic.BasicRepositoryConnectorFactory;
+import org.eclipse.aether.impl.DefaultServiceLocator;
+import org.eclipse.aether.repository.LocalRepository;
+import org.eclipse.aether.repository.RepositoryPolicy;
+import org.eclipse.aether.spi.connector.RepositoryConnectorFactory;
+import org.eclipse.aether.spi.connector.transport.TransporterFactory;
+import org.eclipse.aether.transport.http.HttpTransporterFactory;
+import org.eclipse.aether.util.artifact.DefaultArtifactTypeRegistry;
+
+// Zero-DI Locking Bypasses
+import org.eclipse.aether.impl.SyncContextFactory;
+import org.eclipse.aether.SyncContext;
+import org.eclipse.aether.artifact.Artifact;
+import org.eclipse.aether.metadata.Metadata;
+
+import java.io.File;
+import java.util.Collection;
+
+public class Booter {
+
+    public static RepositorySystem newRepositorySystem() {
+        DefaultServiceLocator locator = MavenRepositorySystemUtils.newServiceLocator();
+        
+        // 1. Explicit Programmatic Bindings (Zero-Reflection Path)
+        locator.addService(RepositoryConnectorFactory.class, BasicRepositoryConnectorFactory.class);
+        locator.addService(TransporterFactory.class, HttpTransporterFactory.class);
+
+        // 2. Pass the Class blueprint instead of an initialized instance
+        locator.setService(SyncContextFactory.class, NoopSyncContextFactory.class);
+
+        locator.setErrorHandler(new DefaultServiceLocator.ErrorHandler() {
+            @Override
+            public void serviceCreationFailed(Class<?> type, Class<?> impl, Throwable exception) {
+                System.err.println("[WARN]|Service creation failed for " + type.getName() + ": " + exception.getMessage());
+            }
+        });
+
+        return locator.getService(RepositorySystem.class);
+    }
+
+    /**
+     * Initializes a lightweight repo session mapped directly to local storage targets.
+     */
+    public static RepositorySystemSession newRepositorySystemSession(RepositorySystem system, File localRepoDir) {
+        if (system == null) {
+            throw new IllegalStateException("RepositorySystem initialization failed. Verify that core dependencies exist inside the daemon's active classpath.");
+        }
+
+        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();
+
+        LocalRepository localRepo = new LocalRepository(localRepoDir);
+        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));
+
+        // CRITICAL FIX: Retrieve the default Maven registry (so 'pom' is preserved) and APPEND 'aar'
+        org.eclipse.aether.artifact.ArtifactTypeRegistry existingRegistry = session.getArtifactTypeRegistry();
+        DefaultArtifactTypeRegistry stereotypes;
+        
+        if (existingRegistry instanceof DefaultArtifactTypeRegistry) {
+            stereotypes = (DefaultArtifactTypeRegistry) existingRegistry;
+        } else {
+            stereotypes = new DefaultArtifactTypeRegistry();
+        }
+        
+        // includesDependencies = false (Enables transitive traversal for AARs)
+        // addedToClasspath     = true  (Includes resolved artifacts in build path)
+        stereotypes.add(new DefaultArtifactType("aar", "aar", "", "java", true, true));
+        session.setArtifactTypeRegistry(stereotypes);
+
+        session.setChecksumPolicy(RepositoryPolicy.CHECKSUM_POLICY_WARN);
+
+        // Disable remote tracking listeners to keep stdout clean for the C++ IPC layer
+        session.setTransferListener(null);
+        session.setRepositoryListener(null);
+
+        return session;
+    }
+
+    private static class NoopSyncContextFactory implements SyncContextFactory {
+        @Override
+        public SyncContext newInstance(RepositorySystemSession session, boolean shared) {
+            return new SyncContext() {
+                @Override
+                public void acquire(Collection<? extends Artifact> artifacts,
+                                    Collection<? extends Metadata> metadatas) {}
+
+                @Override
+                public void close() {}
+            };
+        }
+    }
+}
\ No newline at end of file
diff --git a/java/com/mkapk/tools/DaemonServer.java b/java/com/mkapk/tools/DaemonServer.java
index 6cab2a0..ab6d362 100644
--- a/java/com/mkapk/tools/DaemonServer.java
+++ b/java/com/mkapk/tools/DaemonServer.java
@@ -61,4 +61,4 @@ public class DaemonServer {
             System.err.println("[JVM STDERR] IPC Pipe disconnected: " + e.getMessage());
         }
     }
-}
+}
\ No newline at end of file
diff --git a/java/com/mkapk/tools/DependencyResolver.java b/java/com/mkapk/tools/DependencyResolver.java
new file mode 100644
index 0000000..b30f544
--- /dev/null
+++ b/java/com/mkapk/tools/DependencyResolver.java
@@ -0,0 +1,139 @@
+package com.mkapk.tools;
+
+import java.io.File;
+import java.io.PrintStream;
+import java.net.URL;
+import java.util.Arrays;
+import java.util.LinkedHashSet;
+import java.util.Set;
+
+import org.eclipse.aether.RepositorySystem;
+import org.eclipse.aether.RepositorySystemSession;
+import org.eclipse.aether.artifact.Artifact;
+import org.eclipse.aether.artifact.DefaultArtifact;
+import org.eclipse.aether.collection.CollectRequest;
+import org.eclipse.aether.collection.CollectResult;
+import org.eclipse.aether.collection.DependencyCollectionException;
+import org.eclipse.aether.graph.Dependency;
+import org.eclipse.aether.graph.DependencyNode;
+import org.eclipse.aether.repository.RemoteRepository;
+import org.eclipse.aether.resolution.ArtifactRequest;
+import org.eclipse.aether.resolution.ArtifactResult;
+import org.eclipse.aether.util.artifact.JavaScopes;
+import org.eclipse.aether.util.graph.visitor.PreorderNodeListGenerator;
+
+public class DependencyResolver implements ToolHandler {
+
+    private final Set<URL> dynamicClassPathUrls;
+
+    public DependencyResolver(Set<URL> dynamicClassPathUrls) {
+        this.dynamicClassPathUrls = dynamicClassPathUrls;
+    }
+
+    private static File getLocalCacheDir() {
+        String termuxPrefix = System.getenv("PREFIX");
+        if (termuxPrefix == null || termuxPrefix.isEmpty()) {
+            termuxPrefix = System.getProperty("user.home") + "/.mkapk";
+        }
+        
+        File cacheDir = new File(termuxPrefix + "/var/lib/mkapk/lib");
+        if (!cacheDir.exists()) {
+            cacheDir.mkdirs();
+        }
+        return cacheDir;
+    }
+
+    @Override
+    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
+        if (args.length < 1) {
+            outStream.println("[ERROR]|Provide at least one maven coordinate");
+            return false;
+        }
+
+        RepositorySystem system = Booter.newRepositorySystem();
+        RepositorySystemSession session = Booter.newRepositorySystemSession(system, getLocalCacheDir());
+
+        RemoteRepository googleRepo = new RemoteRepository.Builder("google", "default", "https://dl.google.com/dl/android/maven2/").build();
+        RemoteRepository centralRepo = new RemoteRepository.Builder("central", "default", "https://repo1.maven.org/maven2/").build();
+
+        Set<File> resolvedFiles = new LinkedHashSet<>();
+
+        // Batch all coordinates into a single request for automatic version mediation
+        CollectRequest collectRequest = new CollectRequest();
+        for (String rawCoordinate : args) {
+            if (rawCoordinate == null || rawCoordinate.trim().isEmpty()) continue;
+            
+            String[] coordParts = rawCoordinate.split(":");
+            String primaryCoordinate = rawCoordinate;
+            if (coordParts.length == 3) {
+                primaryCoordinate = coordParts[0] + ":" + coordParts[1] + ":aar:" + coordParts[2];
+            }
+            
+            collectRequest.addDependency(new Dependency(new DefaultArtifact(primaryCoordinate), JavaScopes.COMPILE));
+        }
+
+        collectRequest.addRepository(googleRepo);
+        collectRequest.addRepository(centralRepo);
+
+        CollectResult collectResult = null;
+        try {
+            collectResult = system.collectDependencies(session, collectRequest);
+        } catch (DependencyCollectionException e) {
+            collectResult = e.getResult();
+            outStream.println("[WARN]|Partial graph collection: " + e.getMessage());
+        }
+
+        // FULL GRAPH TRAVERSAL: Use Aether's built-in generator to preserve correctly mediated versions
+        PreorderNodeListGenerator nlg = new PreorderNodeListGenerator();
+        if (collectResult != null && collectResult.getRoot() != null) {
+            collectResult.getRoot().accept(nlg);
+        }
+
+        // Resolve local files for ALL mediated direct & transitive artifacts
+        for (DependencyNode node : nlg.getNodes()) {
+            if (node.getDependency() != null && node.getDependency().getArtifact() != null) {
+                Artifact art = node.getDependency().getArtifact();
+                
+                // Attempt AAR first
+                Artifact aarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "aar", art.getVersion());
+                ArtifactRequest aarReq = new ArtifactRequest(aarArtifact, Arrays.asList(googleRepo, centralRepo), null);
+                try {
+                    ArtifactResult aarRes = system.resolveArtifact(session, aarReq);
+                    if (aarRes.isResolved() && aarRes.getArtifact().getFile() != null) {
+                        resolvedFiles.add(aarRes.getArtifact().getFile());
+                        continue; 
+                    }
+                } catch (Exception ignored) {}
+
+                // Fallback to JAR
+                Artifact jarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "jar", art.getVersion());
+                ArtifactRequest jarReq = new ArtifactRequest(jarArtifact, Arrays.asList(googleRepo, centralRepo), null);
+                try {
+                    ArtifactResult jarRes = system.resolveArtifact(session, jarReq);
+                    if (jarRes.isResolved() && jarRes.getArtifact().getFile() != null) {
+                        resolvedFiles.add(jarRes.getArtifact().getFile());
+                    }
+                } catch (Exception ignored) {}
+            }
+        }
+
+        // Pipe all resolved absolute file paths back to C++
+        StringBuilder resolvedPaths = new StringBuilder("MKAPK_RESOLVED");
+        for (File f : resolvedFiles) {
+            if (f != null && f.exists()) {
+                String name = f.getName();
+                if (name.contains("kotlin-stdlib-jdk7") || name.contains("kotlin-stdlib-jdk8")) {
+                    continue; 
+                }
+                
+                resolvedPaths.append("|").append(f.getAbsolutePath());
+                try {
+                    dynamicClassPathUrls.add(f.toURI().toURL());
+                } catch (Exception ignored) {}
+            }
+        }
+
+        outStream.println(resolvedPaths.toString());
+        return true;
+    }
+}
\ No newline at end of file
diff --git a/java/com/mkapk/tools/JavacHandler.java b/java/com/mkapk/tools/JavacHandler.java
index 36a498e..29b8824 100644
--- a/java/com/mkapk/tools/JavacHandler.java
+++ b/java/com/mkapk/tools/JavacHandler.java
@@ -44,4 +44,4 @@ public class JavacHandler implements ToolHandler {
             Thread.currentThread().setContextClassLoader(originalContextLoader);
         }
     }
-}
+}
\ No newline at end of file
diff --git a/java/com/mkapk/tools/KotlinHandler.java b/java/com/mkapk/tools/KotlinHandler.java
index 6b1780a..8b32eee 100644
--- a/java/com/mkapk/tools/KotlinHandler.java
+++ b/java/com/mkapk/tools/KotlinHandler.java
@@ -59,4 +59,4 @@ public class KotlinHandler implements ToolHandler {
             Thread.currentThread().setContextClassLoader(originalContextLoader);
         }
     }
-}
+}
\ No newline at end of file
diff --git a/java/com/mkapk/tools/ManifestMergerHandler.java b/java/com/mkapk/tools/ManifestMergerHandler.java
new file mode 100644
index 0000000..5d592bf
--- /dev/null
+++ b/java/com/mkapk/tools/ManifestMergerHandler.java
@@ -0,0 +1,70 @@
+package com.mkapk.tools;
+
+import java.io.File;
+import java.io.PrintStream;
+import java.nio.file.Files;
+import java.nio.file.StandardCopyOption;
+import java.util.ArrayList;
+import java.util.List;
+
+public class ManifestMergerHandler implements ToolHandler {
+
+    @Override
+    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
+        System.setOut(outStream);
+        System.setErr(errStream);
+
+        if (args.length < 2) {
+            outStream.println("[ERROR]|ManifestMerger requires at least 2 arguments: mainManifest and outputManifest.");
+            return false;
+        }
+
+        File mainManifest = new File(args[0]);
+        File outFile = new File(args[1]);
+
+        if (outFile.getParentFile() != null) {
+            outFile.getParentFile().mkdirs();
+        }
+
+        // FIX: NO LIBRARIES TO MERGE: Fast copy main manifest directly to destination
+        if (args.length == 2) {
+            Files.copy(mainManifest.toPath(), outFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
+            return outFile.exists() && outFile.length() > 0;
+        }
+
+        List<String> mergerArgs = new ArrayList<>();
+        mergerArgs.add("--main");
+        mergerArgs.add(args[0]);
+        mergerArgs.add("--out");
+        mergerArgs.add(args[1]);
+
+        StringBuilder libsBuilder = new StringBuilder();
+        for (int i = 2; i < args.length; i++) {
+            if (args[i] != null && !args[i].trim().isEmpty()) {
+                libsBuilder.append(args[i]);
+                if (i < args.length - 1) {
+                    libsBuilder.append(File.pathSeparator);
+                }
+            }
+        }
+
+        if (libsBuilder.length() > 0) {
+            mergerArgs.add("--libs");
+            mergerArgs.add(libsBuilder.toString());
+        }
+
+        try {
+            com.android.manifmerger.Merger.main(mergerArgs.toArray(new String[0]));
+        } catch (MkapkTools.ExitInterceptedException e) {
+            if (e.status != 0) {
+                outStream.println("[ERROR]|ManifestMerger exited with status code: " + e.status);
+                return false;
+            }
+        } catch (Throwable t) {
+            outStream.println("[ERROR]|ManifestMerger exception: " + t.getMessage());
+            return false;
+        }
+
+        return outFile.exists() && outFile.length() > 0;
+    }
+}
\ No newline at end of file
diff --git a/java/com/mkapk/tools/SimpleToolHandler.java b/java/com/mkapk/tools/SimpleToolHandler.java
index bd9302b..c64e3f3 100644
--- a/java/com/mkapk/tools/SimpleToolHandler.java
+++ b/java/com/mkapk/tools/SimpleToolHandler.java
@@ -34,4 +34,4 @@ public class SimpleToolHandler implements ToolHandler {
 
         return true;
     }
-}
+}
\ No newline at end of file
diff --git a/java/com/mkapk/tools/ToolHandler.java b/java/com/mkapk/tools/ToolHandler.java
index 1051bb2..4ce941b 100644
--- a/java/com/mkapk/tools/ToolHandler.java
+++ b/java/com/mkapk/tools/ToolHandler.java
@@ -11,4 +11,4 @@ public interface ToolHandler {
      * @return true if successful, false otherwise.
      */
     boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception;
-}
+}
\ No newline at end of file
diff --git a/java/com/mkapk/tools/ToolRouter.java b/java/com/mkapk/tools/ToolRouter.java
index f89fa1c..382fa38 100644
--- a/java/com/mkapk/tools/ToolRouter.java
+++ b/java/com/mkapk/tools/ToolRouter.java
@@ -46,6 +46,8 @@ public class ToolRouter {
             case "r8"             -> new SimpleToolHandler("com.android.tools.r8.R8");
             case "resguard"       -> new SimpleToolHandler("com.tencent.mm.resourceproguard.cli.CliMain");
             case "apksigner"      -> new SimpleToolHandler("com.android.apksigner.ApkSignerTool");
+            case "manifestmerger" -> new ManifestMergerHandler();
+            case "resolve"        -> new DependencyResolver(dynamicClassPathUrls);
             case "kotlinc"        -> new KotlinHandler();
             default               -> null;
         };
diff --git a/mkapk-aarch64.deb b/mkapk-aarch64.deb
index 8053a85..9587193 100644
Binary files a/mkapk-aarch64.deb and b/mkapk-aarch64.deb differ
diff --git a/mkapk-aarch64/DEBIAN/control b/mkapk-aarch64/DEBIAN/control
index 01e242e..f586ba3 100644
--- a/mkapk-aarch64/DEBIAN/control
+++ b/mkapk-aarch64/DEBIAN/control
@@ -1,5 +1,5 @@
 Package: mkapk
-Version: 0.4.0
+Version: 0.3.0
 Section: utils
 Priority: optional
 Architecture: all
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/AndroidManifest.xml b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/AndroidManifest.xml
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/AndroidManifest.xml
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/AndroidManifest.xml
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/andresguard-config.xml b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/andresguard-config.xml
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/andresguard-config.xml
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/andresguard-config.xml
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/config.json b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/config.json
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/config.json
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/config.json
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/proguard-rules.pro b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/proguard-rules.pro
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/proguard-rules.pro
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/proguard-rules.pro
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/res/layout/activity_main.xml b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/res/layout/activity_main.xml
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/res/layout/activity_main.xml
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/res/layout/activity_main.xml
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/src/C++/native_bridge.cpp b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/src/C++/native_bridge.cpp
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/src/C++/native_bridge.cpp
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/src/C++/native_bridge.cpp
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/src/C/core_logic.c b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/src/C/core_logic.c
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/src/C/core_logic.c
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/src/C/core_logic.c
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/src/com/example/polyglot/JavaBridge.java b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/src/com/example/polyglot/JavaBridge.java
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/src/com/example/polyglot/JavaBridge.java
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/src/com/example/polyglot/JavaBridge.java
diff --git a/mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/src/com/example/polyglot/MainActivity.kt b/mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/src/com/example/polyglot/MainActivity.kt
similarity index 100%
rename from mkapk-aarch64/data/data/com.termux/files/usr/etc/.setup/proj-templates/android/src/com/example/polyglot/MainActivity.kt
rename to mkapk-aarch64/data/data/com.termux/files/usr/etc/setup/proj-templates/android/src/com/example/polyglot/MainActivity.kt
diff --git a/scripts/classpath.txt b/scripts/classpath.txt
index 4303dfa..3204fd3 100644
--- a/scripts/classpath.txt
+++ b/scripts/classpath.txt
@@ -1 +1 @@
-.:/data/data/com.termux/files/home/android-sdk/Sdk/cmdline-tools/latest/lib/r8.jar:/data/data/com.termux/files/home/AndResGuard/AndResGuard-cli-1.2.15.jar:/data/data/com.termux/files/usr/share/java/apksigner.jar:/data/data/com.termux/files/usr/opt/kotlin/lib/kotlin-preloader.jar
+.:/data/data/com.termux/files/usr/share/mkapk/mkapk-coordinator.jar:/data/data/com.termux/files/usr/share/java/apksigner.jar:/data/data/com.termux/files/home/android-sdk/Sdk/cmdline-tools/latest/lib/r8.jar:/data/data/com.termux/files/home/android-sdk/Sdk/cmdline-tools/latest/lib/d8-classpath.jar:/data/data/com.termux/files/home/android-sdk/Sdk/cmdline-tools/latest/lib/build-system/tools.manifest-merger.jar:/data/data/com.termux/files/home/android-sdk/Sdk/cmdline-tools/latest/lib/zipflinger/zipflinger.jar:/data/data/com.termux/files/home/android-sdk/Sdk/cmdline-tools/latest/lib/external/google/jimfs/jimfs/1.1/jimfs-1.1.jar:/data/data/com.termux/files/home/android-sdk/Sdk/cmdline-tools/latest/lib/org/slf4j/slf4j-api/2.0.16/slf4j-api-2.0.16.jar:/data/data/com.termux/files/usr/share/java/maven-artifact-3.9.6.jar:/data/data/com.termux/files/usr/share/java/maven-builder-support-3.9.6.jar:/data/data/com.termux/files/usr/share/java/maven-model-3.9.6.jar:/data/data/com.termux/files/usr/share/java/maven-model-builder-3.9.6.jar:/data/data/com.termux/files/usr/share/java/maven-repository-metadata-3.9.6.jar:/data/data/com.termux/files/usr/share/java/maven-resolver-api-1.9.18.jar:/data/data/com.termux/files/usr/share/java/maven-resolver-impl-1.9.18.jar:/data/data/com.termux/files/usr/share/java/maven-resolver-named-locks-1.9.18.jar:/data/data/com.termux/files/usr/share/java/maven-resolver-provider-3.9.6.jar:/data/data/com.termux/files/usr/share/java/maven-resolver-spi-1.9.18.jar:/data/data/com.termux/files/usr/share/java/maven-resolver-util-1.9.18.jar:/data/data/com.termux/files/usr/share/java/maven-resolver-connector-basic-1.9.18.jar:/data/data/com.termux/files/usr/share/java/maven-resolver-transport-http-1.9.18.jar:/data/data/com.termux/files/usr/share/java/plexus-interpolation-1.26.jar:/data/data/com.termux/files/usr/share/java/plexus-utils-3.5.1.jar:/data/data/com.termux/files/home/AndResGuard/AndResGuard-cli-1.2.15.jar:/data/data/com.termux/files/usr/opt/kotlin/lib/kotlin-preloader.jar:/data/data/com.termux/files/usr/share/java/httpclient-4.5.13.jar:/data/data/com.termux/files/usr/share/java/httpcore-4.4.13.jar
diff --git a/src/main.cpp b/src/main.cpp
index eb462a5..fbc736c 100644
--- a/src/main.cpp
+++ b/src/main.cpp
@@ -7,6 +7,7 @@
 #include "mkapk_helpers.hpp"
 #include "mkapk_tools.hpp"
 #include "mkapk_ui.hpp"
+#include "mkapk_plugin_manager.hpp"
 
 namespace fs = std::filesystem;
 
@@ -175,7 +176,7 @@ int main(int argc, char* argv[]) {
                 std::cerr.rdbuf(old_cerr_buf);
                 return 1;
             }
-            bool success = MkapkEnv::install_plugin(args[1]);
+            bool success = MkapkPluginManager::install_plugin(args[1]);
             std::cerr.rdbuf(old_cerr_buf);
             return success ? 0 : 1;
         }
@@ -186,7 +187,7 @@ int main(int argc, char* argv[]) {
                 std::cerr.rdbuf(old_cerr_buf);
                 return 1;
             }
-            bool success = MkapkEnv::uninstall_plugin(args[1]);
+            bool success = MkapkPluginManager::uninstall_plugin(args[1]);
             std::cerr.rdbuf(old_cerr_buf);
             return success ? 0 : 1;
         }
@@ -266,4 +267,4 @@ int main(int argc, char* argv[]) {
 
     std::cerr.rdbuf(old_cerr_buf);
     return 0;
-}
+}
\ No newline at end of file
diff --git a/src/mkapk_compiler.cpp b/src/mkapk_compiler.cpp
index b211958..92f849f 100644
--- a/src/mkapk_compiler.cpp
+++ b/src/mkapk_compiler.cpp
@@ -96,7 +96,8 @@ Result<std::pair<fs::path, fs::path>> compile_source_logic(
     std::map<std::string, std::vector<fs::path>>& changed_files,
     std::map<std::string, std::vector<fs::path>>& deleted_files,
     bool do_res,
-    RunFunc run) 
+    RunFunc run,
+    const std::vector<fs::path>& extra_jvm_classpaths) 
 {
 
     fs::path java_out = fs::absolute(bin_dir / "classes" / "java_classes");
@@ -109,6 +110,12 @@ Result<std::pair<fs::path, fs::path>> compile_source_logic(
     // 1. Clean up stale class/dex files
     cleanup_stale_assets(deleted_files, java_out, dex_cache);
 
+    // Convert extra_jvm_classpaths to string vector for Kotlinc
+    std::vector<std::string> classpath_extra_strs;
+    for (const auto& p : extra_jvm_classpaths) {
+        classpath_extra_strs.push_back(fs::absolute(p).string());
+    }
+
     // --- PHASE 0: EXTRACT KOTLIN STANDARD LIBRARY ---
     if (changed_files.find("kotlin") != changed_files.end() && !changed_files["kotlin"].empty()) {
         const char* prefix_env = std::getenv("PREFIX");
@@ -172,7 +179,8 @@ Result<std::pair<fs::path, fs::path>> compile_source_logic(
             java_out,
             joint_sources,
             run,
-            compose_plug
+            compose_plug,
+            classpath_extra_strs 
         );
         if (kot_res.is_err()) {
             return Result<std::pair<fs::path, fs::path>>::error(kot_res.get_error());
@@ -191,7 +199,8 @@ Result<std::pair<fs::path, fs::path>> compile_source_logic(
             fs::absolute(android_jar), 
             fs::absolute(java_out), 
             changed_files["java"], 
-            run
+            run, 
+            extra_jvm_classpaths
         );
         if (java_res.is_err()) {
             return Result<std::pair<fs::path, fs::path>>::error(java_res.get_error());
@@ -214,4 +223,4 @@ Result<std::pair<fs::path, fs::path>> compile_source_logic(
     }
 
     return Result<std::pair<fs::path, fs::path>>::success({java_out, dex_cache});
-}
+}
\ No newline at end of file
diff --git a/src/mkapk_env.cpp b/src/mkapk_env.cpp
index 6569655..05f04f3 100644
--- a/src/mkapk_env.cpp
+++ b/src/mkapk_env.cpp
@@ -82,18 +82,19 @@ namespace MkapkEnv {
         try {
             json j = json::parse(config_content);
 
-            config.project_name   = j.value("NAME", "MyApp");
-            config.bin_dir        = j.value("BIN_DIR", "bin");
-            config.src_dir        = j.value("SRC_DIR", "src");
-            config.res_dir        = j.value("RES_DIR", "res");
-            config.assets_dir     = j.value("ASSETS_DIR", "assets");
-            config.manifest       = j.value("MANIFEST", "AndroidManifest.xml");
+            // Empty strings match the exact behavior of original get_json_val()
+            config.project_name   = j.value("NAME", "");
+            config.bin_dir        = j.value("BIN_DIR", "");
+            config.src_dir        = j.value("SRC_DIR", "");
+            config.res_dir        = j.value("RES_DIR", "");
+            config.assets_dir     = j.value("ASSETS_DIR", "");
+            config.manifest       = j.value("MANIFEST", "");
             
-            config.sdk_root       = j.value("SDK_ROOT", "/data/data/com.termux/files/home/android-sdk/Sdk");
-            config.target_sdk     = j.value("TARGET_SDK", "33");
-            config.java_version   = j.value("JAVA_VERSION", "17");
+            config.sdk_root       = j.value("SDK_ROOT", "");
+            config.target_sdk     = j.value("TARGET_SDK", "");
+            config.java_version   = j.value("JAVA_VERSION", "");
             config.compose_plugin = j.value("COMPOSE_PLUGIN", "");
-            config.proguard_rules = j.value("PROGUARD_RULES", "proguard-rules.pro");
+            config.proguard_rules = j.value("PROGUARD_RULES", "");
             
             config.keystore       = j.value("KEYSTORE", "");
             config.keystore_alias = j.value("KEYSTORE_ALIAS", "");
@@ -104,6 +105,12 @@ namespace MkapkEnv {
                     if (lib.is_string()) config.system_shared_libs.push_back(lib.get<std::string>());
                 }
             }
+            
+           if (j.contains("DEPENDENCIES") && j["DEPENDENCIES"].is_array()) {
+               for (const auto& dep : j["DEPENDENCIES"]) {
+                   if (dep.is_string()) config.dependencies.push_back(dep.get<std::string>());
+               }
+           }
 
             if (j.contains("NATIVE_TARGETS") && j["NATIVE_TARGETS"].is_array()) {
                 for (const auto& item : j["NATIVE_TARGETS"]) {
@@ -183,48 +190,74 @@ namespace MkapkEnv {
     }
 
     std::string get_jni_classpath(const MkapkConfig& config) {
-        if (config.sdk_root.empty()) {
-             UI::warn("SDK_ROOT variable context not explicit in project layout configuration file.");
-        }
-        
-        fs::path sdk_root = resolve_path(config.sdk_root);
-        
-        fs::path coord_jar = fs::path(TERMUX_SHARE) / "mkapk/mkapk-coordinator.jar";
-        fs::path apksigner_jar = fs::path(TERMUX_SHARE) / "java/apksigner.jar";
-        
-        fs::path r8_jar = sdk_root / "cmdline-tools/latest/lib/r8.jar";
-        fs::path d8_jar = sdk_root / "cmdline-tools/latest/lib/d8-classpath.jar";
-        
-        fs::path resguard_jar = resolve_path("~/AndResGuard/AndResGuard-cli-1.2.15.jar");
-        
-        fs::path kotlin_preloader = "/data/data/com.termux/files/usr/opt/kotlin/lib/kotlin-preloader.jar";
+    if (config.sdk_root.empty()) {
+        UI::warn("SDK_ROOT variable context not explicit in project layout configuration file.");
+    }
+    
+    fs::path sdk_root = resolve_path(config.sdk_root);
+    fs::path cmdline_lib = sdk_root / "cmdline-tools/latest/lib";
+    fs::path prefix_java = fs::path(TERMUX_SHARE) / "java";
 
-        std::vector<std::string> cp_entries;
-        
-        if (fs::exists(coord_jar)) cp_entries.push_back(coord_jar.string());
-        else UI::error("Missing tool dependency footprint registry path", coord_jar.string());
+    std::vector<std::string> cp_entries;
 
-        if (fs::exists(r8_jar)) cp_entries.push_back(r8_jar.string());
-        else UI::error("Missing tool dependency footprint registry path", r8_jar.string());
-        
-        if (fs::exists(apksigner_jar)) cp_entries.push_back(apksigner_jar.string());
-        else UI::error("Missing tool dependency footprint registry path", apksigner_jar.string());
+    // ============================================================================
+    // 1. CORE MKAPK & COMPILER UTILITIES
+    // ============================================================================
+    fs::path coord_jar = fs::path(TERMUX_SHARE) / "mkapk/mkapk-coordinator.jar";
+    fs::path apksigner_jar = prefix_java / "apksigner.jar";
+    fs::path r8_jar = cmdline_lib / "r8.jar";
+    fs::path d8_jar = cmdline_lib / "d8-classpath.jar";
+    fs::path resguard_jar = resolve_path("~/AndResGuard/AndResGuard-cli-1.2.15.jar");
+    fs::path kotlin_preloader = "/data/data/com.termux/files/usr/opt/kotlin/lib/kotlin-preloader.jar";
 
-        if (fs::exists(d8_jar)) cp_entries.push_back(d8_jar.string());
+    if (fs::exists(coord_jar)) cp_entries.push_back(coord_jar.string());
+    else UI::error("Missing tool dependency footprint registry path", coord_jar.string());
 
-        if (fs::exists(resguard_jar)) cp_entries.push_back(resguard_jar.string());
-        
-        if (fs::exists(kotlin_preloader)) cp_entries.push_back(kotlin_preloader.string());
-        else UI::error("Kotlin Compiler installation not found at standard path");
+    if (fs::exists(r8_jar)) cp_entries.push_back(r8_jar.string());
+    else UI::error("Missing tool dependency footprint registry path", r8_jar.string());
+    
+    if (fs::exists(apksigner_jar)) cp_entries.push_back(apksigner_jar.string());
+    else UI::error("Missing tool dependency footprint registry path", apksigner_jar.string());
 
-        std::string full_cp = "";
-        for (size_t i = 0; i < cp_entries.size(); ++i) {
-            full_cp += cp_entries[i] + (i == cp_entries.size() - 1 ? "" : ":");
+    if (fs::exists(d8_jar)) cp_entries.push_back(d8_jar.string());
+    if (fs::exists(resguard_jar)) cp_entries.push_back(resguard_jar.string());
+    
+        // ============================================================================
+    // 2. DYNAMIC ANDROID SDK COMPONENT INJECTIONS
+    // ============================================================================
+    if (fs::exists(cmdline_lib)) {
+        for (const auto& entry : fs::recursive_directory_iterator(cmdline_lib)) {
+            if (entry.is_regular_file() && entry.path().extension() == ".jar") {
+                std::string filename = entry.path().filename().string();
+                
+                // FILTER: Exclude internal SDK Kotlin jars to prevent ClassCastExceptions in Kotlinc
+                if (filename.find("kotlin") == std::string::npos) {
+                    cp_entries.push_back(entry.path().string());
+                }
+            }
         }
-        
-        return full_cp;
+    } else {
+        UI::warn("Android SDK cmdline-tools missing at: " + cmdline_lib.string());
+    }
+
+    if (fs::exists(prefix_java)) {
+        for (const auto& entry : fs::directory_iterator(prefix_java)) {
+            if (entry.is_regular_file() && entry.path().extension() == ".jar") {
+                cp_entries.push_back(entry.path().string());
+            }
+        }
+    } else {
+        UI::warn("System Java share directory missing at: " + prefix_java.string());
+    }
+        // 4. CLASSPATH FORMAT ASSEMBLER LOOP
+   std::string full_cp = "";
+    for (size_t i = 0; i < cp_entries.size(); ++i) {
+        full_cp += cp_entries[i] + (i == cp_entries.size() - 1 ? "" : ":");
     }
     
+    return full_cp;
+}
+    
     std::vector<NativeTargetConfig> parse_json_native_targets(const std::string& config_content) {
         std::vector<NativeTargetConfig> targets;
         try {
@@ -261,7 +294,7 @@ namespace MkapkEnv {
 
     bool init_project() {
         
-        const fs::path TEMPLATE_PATH = fs::path(TERMUX_ETC) / ".setup/proj-templates/android";
+        const fs::path TEMPLATE_PATH = fs::path(TERMUX_ETC) / "setup/proj-templates/android";
         UI::stage("Initialization", "Seeding default template paths structure");
 
         if (!fs::exists(TEMPLATE_PATH)) {
@@ -288,4 +321,4 @@ namespace MkapkEnv {
             return false;
         }
     }
-}
+}
\ No newline at end of file
diff --git a/src/mkapk_helper.cpp b/src/mkapk_helper.cpp
index 5dd4a74..a7b5fe7 100644
--- a/src/mkapk_helper.cpp
+++ b/src/mkapk_helper.cpp
@@ -52,4 +52,4 @@ Result<void> smart_run(const std::vector<std::string>& args, const std::string&
         
         return Result<void>::success();
     }
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_modules/dex.cpp b/src/modules/build_modules/dex.cpp
index c0d5e81..85019eb 100644
--- a/src/modules/build_modules/dex.cpp
+++ b/src/modules/build_modules/dex.cpp
@@ -46,6 +46,7 @@ Result<void> run_incremental_dex(const std::string& D8,
                          const fs::path& java_out,
                          const fs::path& dex_cache,
                          const std::vector<fs::path>& files_to_dex,
+                         const std::vector<fs::path>& extra_jvm_classpaths,
                          RunFunc run) {
     if (files_to_dex.empty()) return Result<void>::success();
 
@@ -74,7 +75,13 @@ Result<void> run_incremental_dex(const std::string& D8,
                 "--classpath", fs::absolute(java_out).string(),
                 "--output", fs::absolute(target_dex_dir).string()
             };
-            
+
+            // Inject the extra dependencies into D8's classpath for interface desugaring resolution
+            for (const auto& jar : extra_jvm_classpaths) {
+                d8_args.push_back("--classpath");
+                d8_args.push_back(fs::absolute(jar).string());
+            }
+
             for (const auto& cls : family_classes) d8_args.push_back(cls);
 
             auto res = run(d8_args, "Incremental D8 failed for: " + base_name);
@@ -204,4 +211,4 @@ Result<void> run_dex_d8(
     if (res.is_err()) return res;
 
     return Result<void>::success();
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_modules/finish.cpp b/src/modules/build_modules/finish.cpp
index 3f66b77..38ed554 100644
--- a/src/modules/build_modules/finish.cpp
+++ b/src/modules/build_modules/finish.cpp
@@ -432,4 +432,4 @@ Result<std::pair<std::string, std::string>> handle_debug_keystore() {
     }
     
     return Result<std::pair<std::string, std::string>>::success({debug_ks.string(), "androiddebugkey"});
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_modules/java.cpp b/src/modules/build_modules/java.cpp
index 3887b80..a91cec5 100644
--- a/src/modules/build_modules/java.cpp
+++ b/src/modules/build_modules/java.cpp
@@ -19,7 +19,8 @@ Result<void> compile_incremental_java(
     const fs::path& android_jar,
     const fs::path& out_dir,
     const std::vector<fs::path>& changed_files,
-    RunFunc run_func)
+    RunFunc run_func,
+    const std::vector<fs::path>& extra_dependency_jars)
 {
     if (changed_files.empty()) return Result<void>::success();
 
@@ -29,6 +30,12 @@ Result<void> compile_incremental_java(
     cp_components.push_back(fs::absolute(android_jar).string());
     cp_components.push_back(fs::absolute(out_dir).string());
 
+    for (const auto& jar : extra_dependency_jars) {
+        if (fs::exists(jar)) {
+            cp_components.push_back(fs::absolute(jar).string());
+        }
+    }
+
     fs::path libs_dir = "libs";
     if (fs::exists(libs_dir)) {
         for (const auto& entry : fs::recursive_directory_iterator(libs_dir)) {
@@ -68,3 +75,39 @@ Result<void> compile_incremental_java(
     
     return Result<void>::success();
 }
+
+Result<void> compile_java(
+    const std::string& java_version,
+    const std::vector<std::string>& javac_flags,
+    const fs::path& android_jar,
+    const fs::path& classes_dir,
+    const fs::path& src_dir,
+    RunFunc run_func) 
+{
+    UI::stage(UI::Msg::JAVA_STAGE, "Compiling all sources via JNI backend...");
+
+    if (!fs::exists(src_dir)) {
+        return Result<void>::error("Source directory missing: " + src_dir.string());
+    }
+
+    std::vector<fs::path> java_files;
+    for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
+        if (entry.is_regular_file() && entry.path().extension() == ".java") {
+            java_files.push_back(entry.path());
+        }
+    }
+
+    if (java_files.empty()) {
+        UI::info("No Java source files found.");
+        return Result<void>::success();
+    }
+
+    return compile_incremental_java(
+        java_version,
+        javac_flags,
+        android_jar,
+        classes_dir,
+        java_files,
+        run_func,
+        {});
+}
\ No newline at end of file
diff --git a/src/modules/build_modules/kotlin.cpp b/src/modules/build_modules/kotlin.cpp
index c27311d..f7e82a9 100644
--- a/src/modules/build_modules/kotlin.cpp
+++ b/src/modules/build_modules/kotlin.cpp
@@ -20,7 +20,8 @@ Result<void> compile_incremental_kotlin(
     const fs::path& classes_dir,
     const std::vector<fs::path>& changed_files,
     RunFunc run_func,
-    const std::string& compose_plugin) 
+    const std::string& compose_plugin,
+    const std::vector<std::string>& classpath_extra) 
 {
     if (changed_files.empty()) return Result<void>::success();
 
@@ -58,6 +59,13 @@ Result<void> compile_incremental_kotlin(
         }
     }
 
+    // 4. Resolved Maven AAR/JAR Extracted Classpaths (Crucial for androidx/lifecycleScope/etc.)
+    for (const auto& extra : classpath_extra) {
+        if (!extra.empty()) {
+            add_to_cp(fs::path(extra));
+        }
+    }
+
     // Assemble the delimited classpath string
     std::string classpath = "";
     for (size_t i = 0; i < cp_components.size(); ++i) {
@@ -100,3 +108,27 @@ Result<void> compile_incremental_kotlin(
     
     return Result<void>::success();
 }
+
+Result<void> compile_kotlin(
+    const std::string& KOTLINC,
+    const fs::path& android_jar,
+    const fs::path& classes_dir,
+    const fs::path& src_dir,
+    RunFunc run_func,
+    const std::string& compose_plugin) 
+{
+    std::vector<fs::path> kt_files;
+    if (!fs::exists(src_dir)) {
+        return Result<void>::error("Kotlin workspace tracking directory missing from context layout: " + src_dir.string());
+    }
+
+    for (const auto& entry : fs::recursive_directory_iterator(src_dir)) {
+        if (entry.is_regular_file() && entry.path().extension() == ".kt") {
+            kt_files.push_back(entry.path());
+        }
+    }
+
+    if (kt_files.empty()) return Result<void>::success();
+
+    return compile_incremental_kotlin(KOTLINC, android_jar, classes_dir, kt_files, run_func, compose_plugin, {});
+}
\ No newline at end of file
diff --git a/src/modules/build_modules/native.cpp b/src/modules/build_modules/native.cpp
index 1cf3302..a2549c0 100644
--- a/src/modules/build_modules/native.cpp
+++ b/src/modules/build_modules/native.cpp
@@ -185,4 +185,4 @@ bool compile_native(
     }
 
     return true;
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_modules/res.cpp b/src/modules/build_modules/res.cpp
index f69cc72..ea34c93 100644
--- a/src/modules/build_modules/res.cpp
+++ b/src/modules/build_modules/res.cpp
@@ -16,7 +16,8 @@ Result<void> compile_resources(
     const fs::path& res_dir,
     const fs::path& bin_dir,
     RunFunc run_func,
-    const std::vector<fs::path>* changed_res_files)
+    const std::vector<fs::path>* changed_res_files,
+    const std::vector<fs::path>& extra_dependency_res_dirs)
 {
     fs::path flat_dir = bin_dir / "flat_res";
     fs::create_directories(flat_dir);
@@ -53,7 +54,30 @@ Result<void> compile_resources(
     } else {
         UI::warn("Primary resource directory not located at: " + res_dir.string());
     }
-    
+
+    // --- PHASE 2: PROCESS EXTRA AAR LIBRARY DEPENDENCY RESOURCE TREES ---
+    if (!extra_dependency_res_dirs.empty()) {
+        for (const auto& extra_res : extra_dependency_res_dirs) {
+            if (fs::exists(extra_res) && !fs::is_empty(extra_res)) {
+                
+                std::string lib_name = extra_res.parent_path().parent_path().filename().string();
+                std::string lib_version = extra_res.parent_path().filename().string();
+                
+                fs::path lib_out_arc = flat_dir / (lib_name + "_" + lib_version + ".flata");
+
+                if (!fs::exists(lib_out_arc)) {
+                    std::vector<std::string> extra_args = {
+                        AAPT2, "compile",
+                        "--dir", fs::absolute(extra_res).string(),
+                        "-o", fs::absolute(lib_out_arc).string()
+                    };
+                    
+                    UI::info("[+] Compiling library resources: " + lib_name);
+                    run_func(extra_args, "Failed compilation of external dependency resource directory tree: " + extra_res.string());
+                }
+            }
+        }
+    }
     return Result<void>::success();
 }
 
@@ -93,6 +117,31 @@ Result<void> link_manifest(
         "--auto-add-overlay"
     };
 
+    std::vector<std::string> library_archives;
+
+    // FIX: Safely route AAR libraries to the -R flag, while keeping app resources positional
+    for (const auto& entry : fs::directory_iterator(flat_dir)) {
+        if (entry.is_regular_file()) {
+            std::string ext = entry.path().extension().string();
+            if (ext == ".flat") {
+                // Positional inputs (Strict AAPT2 deduping applied)
+                args.push_back(fs::absolute(entry.path()).string());
+            } else if (ext == ".flata") {
+                // Collect library archives for ordered -R injection
+                library_archives.push_back(fs::absolute(entry.path()).string());
+            }
+        }
+    }
+
+    // Sort alphabetically so appcompat always merges before material
+    std::sort(library_archives.begin(), library_archives.end());
+
+    // Inject the -R flags to enable AAPT2 library resource merging
+    for (const auto& arc : library_archives) {
+        args.push_back("-R");
+        args.push_back(arc);
+    }
+
     if (debug) {
         args.push_back("--debug-mode");
     }
@@ -139,4 +188,4 @@ fs::path obfuscate_resources(
 
     UI::warn("AndResGuard execution completed but no output APK was found. Reverting to base package.");
     return in_apk;
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_pipeline.cpp b/src/modules/build_pipeline.cpp
index 0b5b851..9ab5f57 100644
--- a/src/modules/build_pipeline.cpp
+++ b/src/modules/build_pipeline.cpp
@@ -10,8 +10,11 @@
 #include "mkapk_tools.hpp"
 #include "mkapk_ui.hpp"
 #include "mkapk_config.hpp"
+#include "mkapk_plugin_manager.hpp"
 #include "pipeline_stage.hpp"
 
+// FIX: Removed invalid forward declarations here. They are now exported by pipeline_stage.hpp
+
 namespace fs = std::filesystem;
 
 std::string perform_build(const std::vector<std::string>& raw_args, const MkapkConfig& config) {
@@ -32,13 +35,13 @@ std::string perform_build(const std::vector<std::string>& raw_args, const MkapkC
     ctx.src_dir = fs::absolute(MkapkEnv::resolve_path(config.src_dir));
     ctx.res_dir = fs::absolute(MkapkEnv::resolve_path(config.res_dir));
     ctx.manifest_path = fs::absolute(MkapkEnv::resolve_path(config.manifest));
-    ctx.active_manifest_path = ctx.manifest_path;
     ctx.android_jar = fs::absolute(MkapkEnv::get_android_jar(config));
+
     fs::create_directories(ctx.bin_dir);
     fs::create_directories(ctx.build_dir);
 
     ctx.tools = MkapkEnv::get_tools_map(config);
-    ctx.active_plugins = MkapkEnv::load_installed_plugins();
+    ctx.active_plugins = MkapkPluginManager::load_installed_plugins();
 
     ctx.run_func = [](const std::vector<std::string>& args, const std::string& err_msg) -> Result<void> {
         return smart_run(args, err_msg);
@@ -74,12 +77,16 @@ std::string perform_build(const std::vector<std::string>& raw_args, const MkapkC
     }
 
     ctx.resources_triggered = (ctx.diff.res_changed || ctx.diff.manifest_changed || ctx.force_all);
-    
+
+    DependencyStage dep_stage;
     ResourceStage res_stage;
     NativeStage native_stage;
     JvmStage jvm_stage;
     PackageStage pkg_stage;
-    
+
+    Result<void> res_dep = dep_stage.execute(config, ctx);
+    if (res_dep.is_err()) throw std::runtime_error("Dependency resolution failure: " + res_dep.get_error());
+
     auto resource_worker = std::async(std::launch::async, [&]() -> Result<void> {
         return res_stage.execute(config, ctx);
     });
@@ -107,4 +114,4 @@ std::string perform_build(const std::vector<std::string>& raw_args, const MkapkC
     if (res_pack.is_err()) throw std::runtime_error("Packaging failure: " + res_pack.get_error());
 
     return ctx.final_output_msg;
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_stages/dependency_stage.cpp b/src/modules/build_stages/dependency_stage.cpp
new file mode 100644
index 0000000..f68bf6c
--- /dev/null
+++ b/src/modules/build_stages/dependency_stage.cpp
@@ -0,0 +1,89 @@
+#include "pipeline_stage.hpp"
+#include "mkapk_resolver.hpp"
+#include "mkapk_extractor.hpp"
+#include "mkapk_manifest_merger.hpp"
+#include "mkapk_ui.hpp"
+#include "mkapk_tools.hpp"
+#include <algorithm>
+#include <fstream>
+#include <sstream>
+#include <functional>
+
+namespace fs = std::filesystem;
+
+Result<void> DependencyStage::execute(const MkapkConfig& config, PipelineContext& ctx) {
+    ctx.active_manifest_path = ctx.manifest_path;
+
+    if (config.dependencies.empty()) {
+        return Result<void>::success();
+    }
+
+    fs::path dep_hash_file = ctx.build_dir / ".hashes" / "deps.hash";
+    fs::create_directories(dep_hash_file.parent_path());
+
+    std::stringstream deps_ss;
+    for (const auto& dep : config.dependencies) {
+        deps_ss << dep << "\n";
+    }
+    
+    // FIX: Hash the string configuration data natively
+    std::hash<std::string> hasher;
+    std::string current_deps_hash = std::to_string(hasher(deps_ss.str()));
+
+    std::string cached_deps_hash = "";
+    if (fs::exists(dep_hash_file)) {
+        std::ifstream hf(dep_hash_file);
+        hf >> cached_deps_hash;
+    }
+
+    bool deps_config_changed = (current_deps_hash != cached_deps_hash) || ctx.force_all;
+
+    std::vector<std::string> cached_artifacts = MkapkResolver::resolve_dependencies(config.dependencies, config);
+    bool missing_cached_files = false;
+    for (const auto& art_path : cached_artifacts) {
+        if (!fs::exists(art_path)) {
+            missing_cached_files = true;
+            break;
+        }
+    }
+
+    if (deps_config_changed || cached_artifacts.empty() || missing_cached_files) {
+        UI::stage("Resolver", "Resolving dependencies via Maven matrix...");
+        ctx.all_resolved_artifacts = MkapkResolver::resolve_dependencies(config.dependencies, config);
+
+        std::ofstream hf(dep_hash_file);
+        hf << current_deps_hash;
+    } else {
+        UI::info("Dependencies configuration up-to-date. Using cached resolution graph.");
+        ctx.all_resolved_artifacts = cached_artifacts;
+    }
+
+    std::sort(ctx.all_resolved_artifacts.begin(), ctx.all_resolved_artifacts.end());
+    auto last = std::unique(ctx.all_resolved_artifacts.begin(), ctx.all_resolved_artifacts.end());
+    ctx.all_resolved_artifacts.erase(last, ctx.all_resolved_artifacts.end());
+
+    MkapkExtractor::extract_all(ctx.all_resolved_artifacts);
+
+    fs::path merged_manifest_output = ctx.build_dir / "AndroidManifest.xml";
+
+    bool manifest_src_changed = ctx.diff.manifest_changed;
+    bool merged_manifest_missing = !fs::exists(merged_manifest_output);
+
+    if (manifest_src_changed || deps_config_changed || merged_manifest_missing || ctx.force_all) {
+        bool merge_success = MkapkManifestMerger::merge_manifests(
+            ctx.manifest_path.string(), 
+            merged_manifest_output.string(), 
+            ctx.all_resolved_artifacts
+        );
+
+        if (merge_success) {
+            ctx.active_manifest_path = merged_manifest_output;
+        } else {
+            UI::warn("Manifest integration anomaly caught. Falling back to primary configuration file layout.");
+        }
+    } else {
+        ctx.active_manifest_path = merged_manifest_output;
+    }
+
+    return Result<void>::success();
+}
\ No newline at end of file
diff --git a/src/modules/build_stages/jvm_stage.cpp b/src/modules/build_stages/jvm_stage.cpp
index 4e90a40..e26506a 100644
--- a/src/modules/build_stages/jvm_stage.cpp
+++ b/src/modules/build_stages/jvm_stage.cpp
@@ -8,6 +8,24 @@ Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx)
         UI::stage("Source Pipeline", "Analyzing active code changes");
     }
 
+    std::vector<std::filesystem::path> extra_jvm_classpaths;
+
+    // Filter and construct the strict classpath for active dependencies ONLY
+    for (const auto& artifact : ctx.all_resolved_artifacts) {
+        std::filesystem::path file_path(artifact);
+        
+        if (file_path.extension() == ".aar") {
+            std::filesystem::path classes_jar = file_path.parent_path() / "classes.jar";
+            if (std::filesystem::exists(classes_jar)) {
+                extra_jvm_classpaths.push_back(classes_jar);
+            }
+        } else if (file_path.extension() == ".jar") {
+            if (std::filesystem::exists(file_path)) {
+                extra_jvm_classpaths.push_back(file_path);
+            }
+        }
+    }
+
     // Execute compilation logic, directly passing classpaths to Kotlinc and Javac
     auto logic_res = compile_source_logic(
         config, 
@@ -18,7 +36,8 @@ Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx)
         ctx.diff.changed_files, 
         ctx.diff.deleted_files, 
         (ctx.diff.res_changed || ctx.force_all), 
-        ctx.run_func
+        ctx.run_func, 
+        extra_jvm_classpaths
     );
     
     if (logic_res.is_err()) {
@@ -47,12 +66,19 @@ Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx)
             ctx.src_dir, 
             java_out, 
             dex_cache, 
-            unified_dex_targets,
+            unified_dex_targets, 
+            extra_jvm_classpaths, 
             ctx.run_func
         );
         if (d8_inc_res.is_err()) return d8_inc_res;
 
         std::vector<std::filesystem::path> jars_to_dex;
+        for (const auto& jar : extra_jvm_classpaths) {
+            std::filesystem::path target_cached_dex = dex_cache / jar.filename().replace_extension(".dex");
+            if (!std::filesystem::exists(target_cached_dex) || ctx.force_all) {
+                jars_to_dex.push_back(jar);
+            }
+        }
 
         if (!jars_to_dex.empty()) {
             std::vector<std::string> d8_library_args = {
@@ -71,4 +97,4 @@ Result<void> JvmStage::execute(const MkapkConfig& config, PipelineContext& ctx)
     }
 
     return Result<void>::success();
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_stages/native_stage.cpp b/src/modules/build_stages/native_stage.cpp
index 26326ce..c5ad120 100644
--- a/src/modules/build_stages/native_stage.cpp
+++ b/src/modules/build_stages/native_stage.cpp
@@ -21,4 +21,4 @@ Result<void> NativeStage::execute(const MkapkConfig& config, PipelineContext& ct
     }
 
     return Result<void>::success();
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_stages/package_stage.cpp b/src/modules/build_stages/package_stage.cpp
index a27ef13..19c2797 100644
--- a/src/modules/build_stages/package_stage.cpp
+++ b/src/modules/build_stages/package_stage.cpp
@@ -84,4 +84,4 @@ Result<void> PackageStage::execute(const MkapkConfig& config, PipelineContext& c
         dynamic_ret_path;
 
     return Result<void>::success();
-}
+}
\ No newline at end of file
diff --git a/src/modules/build_stages/resource_stage.cpp b/src/modules/build_stages/resource_stage.cpp
index aac3f86..cf77482 100644
--- a/src/modules/build_stages/resource_stage.cpp
+++ b/src/modules/build_stages/resource_stage.cpp
@@ -9,13 +9,27 @@ Result<void> ResourceStage::execute(const MkapkConfig& config, PipelineContext&
     }
 
     UI::stage(UI::Msg::RES_STAGE, "Processing resource channels");
+    
+    std::vector<std::filesystem::path> lib_res_dirs;
+    
+    for (const auto& path : ctx.all_resolved_artifacts) {
+        std::filesystem::path file_path(path);
+        if (file_path.extension() == ".aar") {
+            // Path-agnostic lookup: extracted res/ directory lives alongside the .aar artifact
+            std::filesystem::path ext_res = file_path.parent_path() / "res";
+            if (std::filesystem::exists(ext_res) && !std::filesystem::is_empty(ext_res)) {
+                lib_res_dirs.push_back(ext_res);
+            }
+        }
+    }
 
     auto comp_res = compile_resources(
         ctx.tools["aapt2"], 
         ctx.res_dir, 
         ctx.build_dir, 
         ctx.run_func, 
-        (ctx.diff.res_changed && !ctx.force_all) ? &ctx.diff.changed_resources : nullptr
+        (ctx.diff.res_changed && !ctx.force_all) ? &ctx.diff.changed_resources : nullptr,
+        lib_res_dirs
     );
     if (comp_res.is_err()) return comp_res;
 
@@ -32,4 +46,4 @@ Result<void> ResourceStage::execute(const MkapkConfig& config, PipelineContext&
     if (link_res.is_err()) return link_res;
 
     return Result<void>::success();
-}
+}
\ No newline at end of file
diff --git a/src/modules/change_checker.cpp b/src/modules/change_checker.cpp
index 793e5cd..cd7cc9b 100644
--- a/src/modules/change_checker.cpp
+++ b/src/modules/change_checker.cpp
@@ -13,6 +13,7 @@
 #include "mkapk_helpers.hpp"
 #include "mkapk_tools.hpp"
 #include "mkapk_config.hpp"
+#include "mkapk_plugin_manager.hpp"
 
 namespace fs = std::filesystem;
 
@@ -119,7 +120,7 @@ std::pair<BuildResults, std::map<std::string, std::string>> check_changes(
     results.mode_switched = (old_state["meta"]["mode"] != current_mode);
 
     // B: Dynamic Tool/Extension Registration Parsing
-    std::map<std::string, LanguagePlugin> installed_plugins = MkapkEnv::load_installed_plugins();
+    std::map<std::string, LanguagePlugin> installed_plugins = MkapkPluginManager::load_installed_plugins();
     
     if (installed_plugins.find(".java") == installed_plugins.end()) {
         installed_plugins[".java"] = {"java", "javac", ".java", "jvm", "", true};
@@ -217,4 +218,4 @@ void save_state(const fs::path& build_dir, const std::map<std::string, std::stri
     for (auto const& [key, hash] : next_state) {
         f << key << "|" << hash << "|\n";
     }
-}
+}
\ No newline at end of file
diff --git a/src/modules/dependency/extractor.cpp b/src/modules/dependency/extractor.cpp
new file mode 100644
index 0000000..06006c4
--- /dev/null
+++ b/src/modules/dependency/extractor.cpp
@@ -0,0 +1,112 @@
+#include <iostream>
+#include <string>
+#include <vector>
+#include <filesystem>
+#include <fstream>
+#include <zip.h>
+
+#include "mkapk_extractor.hpp"
+#include "mkapk_ui.hpp"
+
+namespace fs = std::filesystem;
+
+namespace MkapkExtractor {
+
+static bool extract_zip_entry(zip_t* archive, zip_uint64_t index, const fs::path& dest_path) {
+    zip_file_t* file = zip_fopen_index(archive, index, 0);
+    if (!file) return false;
+
+    fs::create_directories(dest_path.parent_path());
+    std::ofstream out(dest_path, std::ios::binary);
+    if (!out.is_open()) {
+        zip_fclose(file);
+        return false;
+    }
+
+    char buffer[8192];
+    zip_int64_t bytes_read;
+    while ((bytes_read = zip_fread(file, buffer, sizeof(buffer))) > 0) {
+        out.write(buffer, bytes_read);
+    }
+
+    zip_fclose(file);
+    return true;
+}
+
+bool extract_aar(const std::string& aar_path) {
+    fs::path aar(aar_path);
+    if (!fs::exists(aar)) {
+        UI::error("AAR file does not exist", aar_path);
+        return false;
+    }
+
+    fs::path dest_dir = aar.parent_path();
+    std::string lib_identifier = aar.stem().string();
+
+    // Fast Skip: Check if extracted components already exist on disk[span_8](start_span)[span_8](end_span)
+    if (fs::exists(dest_dir / "AndroidManifest.xml") && fs::exists(dest_dir / "classes.jar")) {
+        return true; 
+    }
+
+    UI::stage("Extracting AAR", lib_identifier);
+
+    int err = 0;
+    zip_t* archive = zip_open(aar.string().c_str(), 0, &err);
+    if (!archive) {
+        UI::error("Failed to open AAR archive: " + aar_path);
+        return false;
+    }
+
+    zip_int64_t num_entries = zip_get_num_entries(archive, 0);
+    bool extraction_failed = false;
+
+    for (zip_int64_t i = 0; i < num_entries; ++i) {
+        const char* name = zip_get_name(archive, i, 0);
+        if (!name) continue;
+
+        std::string entry_name(name);
+
+        if (entry_name == "AndroidManifest.xml") {
+            if (!extract_zip_entry(archive, i, dest_dir / "AndroidManifest.xml")) {
+                extraction_failed = true;
+            }
+        } 
+        else if (entry_name == "classes.jar") {
+            if (!extract_zip_entry(archive, i, dest_dir / "classes.jar")) {
+                extraction_failed = true;
+            }
+        } 
+        else if (entry_name.rfind("res/", 0) == 0) {
+            fs::path target_res_path = dest_dir / entry_name;
+            if (entry_name.back() == '/') {
+                fs::create_directories(target_res_path);
+            } else {
+                if (!extract_zip_entry(archive, i, target_res_path)) {
+                    extraction_failed = true;
+                }
+            }
+        }
+    }
+
+    zip_close(archive);
+
+    if (extraction_failed) {
+        UI::error("Partial failure occurred during AAR extraction mapping for " + lib_identifier);
+        fs::remove(dest_dir / "AndroidManifest.xml");
+        fs::remove(dest_dir / "classes.jar");
+        fs::remove_all(dest_dir / "res");
+        return false;
+    }
+
+    return true;
+}
+
+void extract_all(const std::vector<std::string>& resolved_paths) {
+    for (const auto& path : resolved_paths) {
+        if (path.rfind(".aar") != std::string::npos || (path.size() >= 4 && path.substr(path.size() - 4) == ".aar")) {
+            extract_aar(path);
+        }
+    }
+}
+
+} // namespace MkapkExtractor
\ No newline at end of file
diff --git a/src/modules/dependency/manifest_merger.cpp b/src/modules/dependency/manifest_merger.cpp
new file mode 100644
index 0000000..e156345
--- /dev/null
+++ b/src/modules/dependency/manifest_merger.cpp
@@ -0,0 +1,101 @@
+#include <iostream>
+#include <vector>
+#include <string>
+#include <filesystem>
+#include <sstream>
+
+#include "mkapk_manifest_merger.hpp"
+#include "mkapk_helpers.hpp"
+#include "mkapk_ui.hpp"
+
+namespace fs = std::filesystem;
+
+namespace MkapkManifestMerger {
+
+/**
+ * Resolves the cached manifest file path inside $PREFIX for a given resolved AAR path.
+ */
+static std::string resolve_cached_manifest_path(const fs::path& aar_path) {
+    // AAR Path structure: .../mkapk/lib/<groupId>.<artifactId>/<version>/<artifactId>-<version>.aar
+    try {
+        auto parent_dir = aar_path.parent_path();
+        fs::path manifest_path = parent_dir / "AndroidManifest.xml";
+        if (fs::exists(manifest_path)) {
+            return fs::absolute(manifest_path).string();
+        }
+    } catch (...) {
+        // Fall through to empty string
+    }
+    return "";
+}
+
+bool merge_manifests(
+    const std::string& main_manifest,
+    const std::string& output_manifest,
+    const std::vector<std::string>& resolved_paths) 
+{
+    UI::stage("Manifest Merger", "Merging library manifests with the primary AndroidManifest.xml");
+
+    std::vector<std::string> target_manifests;
+    
+    fs::path primary_path(main_manifest);
+    if (!fs::exists(primary_path)) {
+        UI::error("Primary AndroidManifest.xml not found", main_manifest);
+        return false;
+    }
+    target_manifests.push_back(fs::absolute(primary_path).string());
+
+    // Use a intermediate temp path in internal Termux memory to avoid /storage/emulated/0 write locks
+    fs::path final_output_path(output_manifest);
+    fs::create_directories(final_output_path.parent_path());
+    
+    fs::path temp_output_path = fs::temp_directory_path() / "merged_AndroidManifest.xml";
+    if (fs::exists(temp_output_path)) fs::remove(temp_output_path);
+
+    target_manifests.push_back(fs::absolute(temp_output_path).string());
+
+    for (const auto& path : resolved_paths) {
+        fs::path file_path(path);
+        if (file_path.extension() == ".aar") {
+            std::string cached_manifest = resolve_cached_manifest_path(file_path);
+            if (!cached_manifest.empty()) {
+                target_manifests.push_back(cached_manifest);
+                UI::info("[+] Enqueued for merging: " + fs::path(cached_manifest).parent_path().parent_path().filename().string());
+            }
+        }
+    }
+
+    std::stringstream ss;
+    ss << "manifestmerger";
+    for (const auto& item : target_manifests) {
+        ss << "|" << item;
+    }
+
+    std::vector<std::string> daemon_args;
+    std::string arg;
+    while (std::getline(ss, arg, '|')) {
+        daemon_args.push_back(arg);
+    }
+
+    try {
+        call_java_tool(daemon_args);
+    } catch (const std::exception& e) {
+        UI::error("Manifest merging execution failed during daemon processing", e.what());
+        return false;
+    }
+
+    // Copy from internal temp storage to target output path
+    if (fs::exists(temp_output_path) && fs::file_size(temp_output_path) > 0) {
+        std::error_code ec;
+        fs::copy_file(temp_output_path, final_output_path, fs::copy_options::overwrite_existing, ec);
+        fs::remove(temp_output_path, ec);
+        
+        UI::success("Manifest integration complete: " + final_output_path.filename().string());
+        return true;
+    } else {
+        UI::error("Merged manifest output verification failed. File not found or empty at: " + output_manifest);
+        return false;
+    }
+}
+
+} // namespace MkapkManifestMerger
\ No newline at end of file
diff --git a/src/modules/dependency/resolver.cpp b/src/modules/dependency/resolver.cpp
new file mode 100644
index 0000000..e54a466
--- /dev/null
+++ b/src/modules/dependency/resolver.cpp
@@ -0,0 +1,110 @@
+#include <iostream>
+#include <vector>
+#include <string>
+#include <sstream>
+#include <algorithm>
+#include <filesystem>
+#include <cctype>
+
+#include "mkapk_resolver.hpp"
+#include "mkapk_helpers.hpp"
+#include "mkapk_ui.hpp"
+
+namespace MkapkResolver {
+
+/**
+ * Clean token splitter to tokenize JVM arrays bounded by pipe delimiters.
+ */
+static std::vector<std::string> split_tokens(const std::string& str, char delimiter) {
+    std::vector<std::string> tokens;
+    std::string token;
+    std::istringstream tokenStream(str);
+    while (std::getline(tokenStream, token, delimiter)) {
+        if (!token.empty()) {
+            tokens.push_back(token);
+        }
+    }
+    return tokens;
+}
+
+/**
+ * Helper to trim whitespace from IPC string paths.
+ */
+static std::string trim(const std::string& str) {
+    size_t first = str.find_first_not_of(" \t\r\n");
+    if (first == std::string::npos) return "";
+    size_t last = str.find_last_not_of(" \t\r\n");
+    return str.substr(first, (last - first + 1));
+}
+
+std::vector<std::string> resolve_dependencies(const std::vector<std::string>& coordinates, const MkapkConfig& config) {
+    std::vector<std::string> resolved_paths;
+    if (coordinates.empty()) return resolved_paths;
+
+    // 1. Package unified protocol arguments to send down the global daemon pipe
+    std::vector<std::string> daemon_args = {"resolve"};
+    daemon_args.insert(daemon_args.end(), coordinates.begin(), coordinates.end());
+
+    // 2. Delegate execution securely to the background thread pool proxy handler
+    auto res = call_java_tool(daemon_args);
+    if (res.is_err()) {
+        UI::warn(std::string("Resolver notice: ") + res.get_error());
+    }
+
+    // 3. Parse explicit IPC output payload returned by Java Daemon (MKAPK_RESOLVED|/path/a.aar|/path/b.jar...)
+    const auto& outputs = get_last_daemon_output();
+    for (const auto& line : outputs) {
+        if (line.rfind("MKAPK_RESOLVED|", 0) == 0) {
+            std::stringstream ss(line.substr(15));
+            std::string path_token;
+            while (std::getline(ss, path_token, '|')) {
+                std::string clean_path = trim(path_token);
+                if (!clean_path.empty() && std::filesystem::exists(clean_path)) {
+                    resolved_paths.push_back(clean_path);
+                }
+            }
+        }
+    }
+
+    // 4. Post-Resolution Fallback: On-disk cache discovery if daemon output was missing
+    if (resolved_paths.empty()) {
+        const char* prefix_env = std::getenv("PREFIX");
+        std::filesystem::path local_cache = prefix_env 
+            ? std::filesystem::path(prefix_env) / "var/lib/mkapk/lib" 
+            : "/data/data/com.termux/files/usr/var/lib/mkapk/lib";
+
+        for (const auto& coordinate : coordinates) {
+            std::vector<std::string> coords = split_tokens(coordinate, ':');
+            if (coords.size() >= 3) {
+                std::string group_id = coords[0];
+                std::string artifact_id = coords[1];
+                std::string version = (coords.size() == 3) ? coords[2] : coords.back();
+
+                // FIX: Convert group_id dots to folder slashes (e.g., androidx.core -> androidx/core)
+                std::string group_path = group_id;
+                std::replace(group_path.begin(), group_path.end(), '.', '/');
+
+                std::filesystem::path target_version_dir = local_cache / group_path / artifact_id / version;
+                
+                if (std::filesystem::exists(target_version_dir) && std::filesystem::is_directory(target_version_dir)) {
+                    for (const auto& entry : std::filesystem::recursive_directory_iterator(target_version_dir)) {
+                        if (entry.is_regular_file()) {
+                            std::string ext = entry.path().extension().string();
+                            if (ext == ".jar" || ext == ".aar") {
+                                resolved_paths.push_back(entry.path().string());
+                            }
+                        }
+                    }
+                }
+            }
+        }
+    }
+
+    // 5. De-duplicate layout items safely to resolve graph collisions
+    std::sort(resolved_paths.begin(), resolved_paths.end());
+    resolved_paths.erase(std::unique(resolved_paths.begin(), resolved_paths.end()), resolved_paths.end());
+
+    return resolved_paths;
+}
+
+} // namespace MkapkResolver
\ No newline at end of file
diff --git a/src/modules/plugin_manager.cpp b/src/modules/plugins/plugin_manager.cpp
similarity index 99%
rename from src/modules/plugin_manager.cpp
rename to src/modules/plugins/plugin_manager.cpp
index 61ee5fc..8b31bc7 100644
--- a/src/modules/plugin_manager.cpp
+++ b/src/modules/plugins/plugin_manager.cpp
@@ -23,7 +23,7 @@ using json = nlohmann::json;
 
 extern char** environ;
 
-namespace MkapkEnv {
+namespace MkapkPluginManager {
     
     const std::string PLUGINS_CACHE_DIR = "/data/data/com.termux/files/usr/var/lib/mkapk/plugins/";
 
@@ -275,4 +275,4 @@ namespace MkapkEnv {
         }
         return active_registry;
     }
-}
+}
\ No newline at end of file
