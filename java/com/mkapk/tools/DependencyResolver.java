package com.mkapk.tools;

import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.artifact.Artifact;
import org.eclipse.aether.artifact.DefaultArtifact;
import org.eclipse.aether.collection.CollectRequest;
import org.eclipse.aether.collection.CollectResult;
import org.eclipse.aether.collection.DependencyCollectionException;
import org.eclipse.aether.graph.Dependency;
import org.eclipse.aether.graph.DependencyNode;
import org.eclipse.aether.repository.RemoteRepository;
import org.eclipse.aether.resolution.ArtifactRequest;
import org.eclipse.aether.resolution.ArtifactResult;
import org.eclipse.aether.util.artifact.JavaScopes;
import org.eclipse.aether.util.version.GenericVersionScheme;
import org.eclipse.aether.version.Version;
import org.eclipse.aether.version.VersionScheme;

import java.io.*;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.*;

public class DependencyResolver implements ToolHandler {

    private static final int GRAPH_MAGIC = 0x4D4B4447; // "MKDG" in Little-Endian
    private static final short GRAPH_VERSION = 1;

    private final Set<URL> dynamicClassPathUrls;

    public DependencyResolver(Set<URL> dynamicClassPathUrls) {
        this.dynamicClassPathUrls = dynamicClassPathUrls;
    }

    private static File getLocalCacheDir() {
        String home = System.getenv("HOME");
        if (home == null || home.isEmpty()) {
            home = System.getProperty("user.home");
        }
        File cacheDir = new File(home, ".mkapk/lib");

        if (!cacheDir.exists()) {
            cacheDir.mkdirs();
        }
        return cacheDir;
    }

    // 1. GRAPH NODE DATA STRUCTURE

    public static class GraphNode {
        public final String groupId;
        public final String artifactId;
        public final String version;
        public final String extension;
        public final String relativePath;
        public File file;

        public GraphNode(String groupId, String artifactId, String version, String extension, String relativePath, File file) {
            this.groupId = groupId;
            this.artifactId = artifactId;
            this.version = version;
            this.extension = extension;
            this.relativePath = relativePath;
            this.file = file;
        }

        public String getGaKey() {
            return groupId + ":" + artifactId;
        }

        public String getGavKey() {
            return groupId + ":" + artifactId + ":" + version;
        }

        public String serialize() {
            return groupId + ":" + artifactId + ":" + version + ":" + extension + ":" + relativePath;
        }

        public static GraphNode deserialize(String raw, File cacheDir) {
            String[] parts = raw.split(":", 5);
            if (parts.length < 5) return null;
            File f = new File(cacheDir, parts[4]);
            return new GraphNode(parts[0], parts[1], parts[2], parts[3], parts[4], f);
        }

        public Artifact toArtifact() {
            return new DefaultArtifact(groupId, artifactId, "", extension, version);
        }
    }

    // 2. LITTLE-ENDIAN BINARY I/O HELPERS

    private static void writeIntLE(OutputStream os, int value) throws IOException {
        os.write(value & 0xFF);
        os.write((value >>> 8) & 0xFF);
        os.write((value >>> 16) & 0xFF);
        os.write((value >>> 24) & 0xFF);
    }

    private static void writeShortLE(OutputStream os, short value) throws IOException {
        os.write(value & 0xFF);
        os.write((value >>> 8) & 0xFF);
    }

    private static int readIntLE(InputStream is) throws IOException {
        int b0 = is.read();
        int b1 = is.read();
        int b2 = is.read();
        int b3 = is.read();
        if ((b0 | b1 | b2 | b3) < 0) throw new EOFException();
        return (b0) | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    private static short readShortLE(InputStream is) throws IOException {
        int b0 = is.read();
        int b1 = is.read();
        if ((b0 | b1) < 0) throw new EOFException();
        return (short) ((b0) | (b1 << 8));
    }

    // 3. BINARY GRAPH READ / WRITE

    /**
     * Reads graph.bin and verifies that all transitive dependency artifacts
     * and their respective graph.bin files exist on disk in the global cache.
     */
    private static List<GraphNode> readDependencyGraph(File graphFile, File cacheDir) {
        if (!graphFile.exists() || graphFile.length() < 10) {
            return null;
        }

        try (InputStream is = new BufferedInputStream(new FileInputStream(graphFile))) {
            int magic = readIntLE(is);
            if (magic != GRAPH_MAGIC) return null;

            short version = readShortLE(is);
            if (version != GRAPH_VERSION) return null;

            int nodeCount = readIntLE(is);
            if (nodeCount <= 0 || nodeCount > 50000) return null;

            List<GraphNode> nodes = new ArrayList<>(nodeCount);
            for (int i = 0; i < nodeCount; i++) {
                int len = readIntLE(is);
                if (len <= 0 || len > 65536) return null;

                byte[] buf = new byte[len];
                int totalRead = 0;
                while (totalRead < len) {
                    int r = is.read(buf, totalRead, len - totalRead);
                    if (r < 0) throw new EOFException();
                    totalRead += r;
                }

                String line = new String(buf, StandardCharsets.UTF_8);
                GraphNode node = GraphNode.deserialize(line, cacheDir);
                if (node == null || node.file == null || !node.file.exists() || node.file.length() == 0) {
                    return null; // Missing artifact in cache
                }

                // Ensure every transitive dependency also has its own graph.bin in cache
                File transitiveGraphFile = new File(node.file.getParentFile(), "graph.bin");
                if (!transitiveGraphFile.exists() || transitiveGraphFile.length() < 10) {
                    return null;
                }

                nodes.add(node);
            }
            return nodes;
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * Atomically writes the dependency closure into <dependency_dir>/graph.bin
     */
    private static void writeDependencyGraph(File graphFile, List<GraphNode> nodes) {
        File dir = graphFile.getParentFile();
        if (dir != null && !dir.exists()) {
            dir.mkdirs();
        }

        File tempFile = new File(dir, "graph.bin.tmp." + System.currentTimeMillis() + "." + System.nanoTime());
        try {
            try (OutputStream os = new BufferedOutputStream(new FileOutputStream(tempFile))) {
                writeIntLE(os, GRAPH_MAGIC);
                writeShortLE(os, GRAPH_VERSION);
                writeIntLE(os, nodes.size());

                for (GraphNode node : nodes) {
                    byte[] utf8 = node.serialize().getBytes(StandardCharsets.UTF_8);
                    writeIntLE(os, utf8.length);
                    os.write(utf8);
                }
                os.flush();
            }

            try {
                Files.move(tempFile.toPath(), graphFile.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(tempFile.toPath(), graphFile.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (Exception e) {
            if (tempFile.exists()) {
                tempFile.delete();
            }
        }
    }

    // 4. COORDINATE PARSER & ARTIFACT RESOLVER

    private static Artifact parseCoordinate(String raw) {
        String coord = raw.trim();
        String[] parts = coord.split(":");
        if (parts.length == 3) {
            String defaultExt = (parts[0].startsWith("androidx.") || parts[0].startsWith("com.google.android.")) ? "aar" : "jar";
            return new DefaultArtifact(parts[0], parts[1], "", defaultExt, parts[2]);
        } else if (parts.length == 4) {
            return new DefaultArtifact(parts[0], parts[1], "", parts[2], parts[3]);
        }
        return new DefaultArtifact(coord);
    }

    private static File getDependencyDir(File cacheDir, Artifact artifact) {
        String groupPath = artifact.getGroupId().replace('.', '/');
        return new File(cacheDir, groupPath + "/" + artifact.getArtifactId() + "/" + artifact.getVersion());
    }

    private static String getGavKey(Artifact art) {
        return art.getGroupId() + ":" + art.getArtifactId() + ":" + art.getVersion();
    }

    private static boolean isExcludedScope(String scope) {
        return JavaScopes.TEST.equalsIgnoreCase(scope)
                || JavaScopes.PROVIDED.equalsIgnoreCase(scope)
                || "system".equalsIgnoreCase(scope);
    }

    private static File resolveArtifactFile(RepositorySystem system, RepositorySystemSession session,
                                            List<RemoteRepository> repos, Artifact art) {
        File resolvedFile = null;
        boolean preferAar = "aar".equalsIgnoreCase(art.getExtension())
                || art.getGroupId().startsWith("androidx.")
                || art.getGroupId().startsWith("com.google.android.");

        // Strategy 1: Attempt AAR download for Android components
        if (preferAar) {
            Artifact aarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "aar", art.getVersion());
            try {
                ArtifactResult aarRes = system.resolveArtifact(session, new ArtifactRequest(aarArtifact, repos, null));
                if (aarRes.isResolved() && aarRes.getArtifact().getFile() != null) {
                    resolvedFile = aarRes.getArtifact().getFile();
                }
            } catch (Exception ignored) {}
        }

        // Strategy 2: Attempt download using declared extension
        if (resolvedFile == null) {
            try {
                ArtifactResult directRes = system.resolveArtifact(session, new ArtifactRequest(art, repos, null));
                if (directRes.isResolved() && directRes.getArtifact().getFile() != null) {
                    resolvedFile = directRes.getArtifact().getFile();
                }
            } catch (Exception ignored) {}
        }

        // Strategy 3: Explicit JAR fallback (handles pure-JAR androidx libraries or 3-part JAR coordinates)
        if (resolvedFile == null && !"jar".equalsIgnoreCase(art.getExtension())) {
            Artifact jarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "jar", art.getVersion());
            try {
                ArtifactResult jarRes = system.resolveArtifact(session, new ArtifactRequest(jarArtifact, repos, null));
                if (jarRes.isResolved() && jarRes.getArtifact().getFile() != null) {
                    resolvedFile = jarRes.getArtifact().getFile();
                }
            } catch (Exception ignored) {}
        }

        // Strategy 4: Fallback attempt as AAR if direct/JAR download failed
        if (resolvedFile == null && !preferAar) {
            Artifact aarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "aar", art.getVersion());
            try {
                ArtifactResult aarRes = system.resolveArtifact(session, new ArtifactRequest(aarArtifact, repos, null));
                if (aarRes.isResolved() && aarRes.getArtifact().getFile() != null) {
                    resolvedFile = aarRes.getArtifact().getFile();
                }
            } catch (Exception ignored) {}
        }

        return resolvedFile;
    }

    /**
     * Traverses the Aether DependencyNode tree to collect all unique artifacts
     * and build a complete parent -> children adjacency map across the graph.
     */
    private static void buildGraphTopology(DependencyNode node,
                                           Map<String, Artifact> uniqueArtifacts,
                                           Map<String, Set<String>> adjacencyMap,
                                           Set<DependencyNode> visitedNodes) {
        if (node == null || !visitedNodes.add(node)) return;

        String parentKey = null;
        Dependency dep = node.getDependency();
        if (dep != null && dep.getArtifact() != null) {
            if (isExcludedScope(dep.getScope())) {
                return;
            }
            Artifact art = dep.getArtifact();
            parentKey = getGavKey(art);
            uniqueArtifacts.putIfAbsent(parentKey, art);
            adjacencyMap.putIfAbsent(parentKey, new LinkedHashSet<>());
        }

        for (DependencyNode child : node.getChildren()) {
            Dependency childDep = child.getDependency();
            if (childDep != null && childDep.getArtifact() != null && !isExcludedScope(childDep.getScope())) {
                String childKey = getGavKey(childDep.getArtifact());
                if (parentKey != null) {
                    adjacencyMap.get(parentKey).add(childKey);
                }
                buildGraphTopology(child, uniqueArtifacts, adjacencyMap, visitedNodes);
            }
        }
    }

    /**
     * Collects the transitive GraphNode closure starting from startGavKey using the adjacency map.
     */
    private static void collectClosureFromAdjacency(String currentKey,
                                                    Map<String, GraphNode> resolvedNodeMap,
                                                    Map<String, Set<String>> adjacencyMap,
                                                    Set<String> visited,
                                                    List<GraphNode> outClosure) {
        if (!visited.add(currentKey)) return;

        GraphNode node = resolvedNodeMap.get(currentKey);
        if (node != null) {
            outClosure.add(node);
        }

        Set<String> children = adjacencyMap.get(currentKey);
        if (children != null) {
            for (String childKey : children) {
                collectClosureFromAdjacency(childKey, resolvedNodeMap, adjacencyMap, visited, outClosure);
            }
        }
    }

    // 5. PIPELINE EXECUTION

    @Override
    public boolean execute(String[] args, PrintStream out, PrintStream err) throws Exception {
        if (args.length < 1) {
            out.println("[ERROR]|No coordinates supplied");
            return false;
        }

        File cacheDir = getLocalCacheDir();
        RepositorySystem system = null;
        RepositorySystemSession session = null;

        RemoteRepository google = new RemoteRepository.Builder("google", "default", "https://dl.google.com/dl/android/maven2/").build();
        RemoteRepository central = new RemoteRepository.Builder("central", "default", "https://repo1.maven.org/maven2/").build();
        List<RemoteRepository> repos = Arrays.asList(google, central);

        List<GraphNode> allProjectNodes = new ArrayList<>();

        for (String rawCoord : args) {
            if (rawCoord == null || rawCoord.trim().isEmpty()) continue;
            Artifact rootArtifact = parseCoordinate(rawCoord);
            File depDir = getDependencyDir(cacheDir, rootArtifact);
            File rootGraphFile = new File(depDir, "graph.bin");

            // 1. Check Global Cache: Read binary graph & verify all transitive files and sub-graphs exist
            List<GraphNode> cachedGraph = readDependencyGraph(rootGraphFile, cacheDir);

            if (cachedGraph != null && !cachedGraph.isEmpty()) {
                allProjectNodes.addAll(cachedGraph);
                continue;
            }

            // 2. Cache Miss: Lazy initialize Maven resolver and collect dependency tree
            if (system == null) {
                system = Booter.newRepositorySystem();
                session = Booter.newRepositorySystemSession(system, cacheDir);
            }

            CollectRequest collectRequest = new CollectRequest();
            collectRequest.setRepositories(repos);
            collectRequest.setRoot(new Dependency(rootArtifact, JavaScopes.COMPILE));

            CollectResult collectResult;
            try {
                collectResult = system.collectDependencies(session, collectRequest);
            } catch (DependencyCollectionException dce) {
                collectResult = dce.getResult();
            } catch (Exception e) {
                collectResult = null;
            }

            if (collectResult == null || collectResult.getRoot() == null) {
                continue;
            }

            // 3. Build complete topology across all nodes in the resolved tree
            Map<String, Artifact> uniqueArtifacts = new LinkedHashMap<>();
            Map<String, Set<String>> adjacencyMap = new LinkedHashMap<>();
            buildGraphTopology(
                    collectResult.getRoot(),
                    uniqueArtifacts,
                    adjacencyMap,
                    Collections.newSetFromMap(new IdentityHashMap<>())
            );

            // 4. Resolve each artifact file (.aar or .jar) in the closure
            Map<String, GraphNode> resolvedNodeMap = new LinkedHashMap<>();
            for (Map.Entry<String, Artifact> entry : uniqueArtifacts.entrySet()) {
                String gavKey = entry.getKey();
                Artifact art = entry.getValue();

                File resolvedFile = resolveArtifactFile(system, session, repos, art);
                if (resolvedFile != null && resolvedFile.exists()) {
                    String relPath = cacheDir.toPath().relativize(resolvedFile.toPath()).toString().replace('\\', '/');
                    String ext = resolvedFile.getName().endsWith(".aar") ? "aar" : "jar";
                    resolvedNodeMap.put(gavKey, new GraphNode(
                            art.getGroupId(),
                            art.getArtifactId(),
                            art.getVersion(),
                            ext,
                            relPath,
                            resolvedFile
                    ));
                }
            }

            // 5. Write graph.bin for EVERY dependency (root + every transitive dependency)
            for (Map.Entry<String, Artifact> entry : uniqueArtifacts.entrySet()) {
                String gavKey = entry.getKey();
                Artifact art = entry.getValue();

                List<GraphNode> subClosure = new ArrayList<>();
                collectClosureFromAdjacency(gavKey, resolvedNodeMap, adjacencyMap, new HashSet<>(), subClosure);

                if (!subClosure.isEmpty()) {
                    File nodeGraphFile = new File(getDependencyDir(cacheDir, art), "graph.bin");
                    writeDependencyGraph(nodeGraphFile, subClosure);
                }
            }

            // 6. Add the root coordinate's closure to the project's active nodes
            String rootGavKey = getGavKey(rootArtifact);
            List<GraphNode> rootClosure = new ArrayList<>();
            collectClosureFromAdjacency(rootGavKey, resolvedNodeMap, adjacencyMap, new HashSet<>(), rootClosure);

            if (!rootClosure.isEmpty()) {
                writeDependencyGraph(rootGraphFile, rootClosure);
                allProjectNodes.addAll(rootClosure);
            } else if (!resolvedNodeMap.isEmpty()) {
                List<GraphNode> fallbackClosure = new ArrayList<>(resolvedNodeMap.values());
                writeDependencyGraph(rootGraphFile, fallbackClosure);
                allProjectNodes.addAll(fallbackClosure);
            }
        }

        if (allProjectNodes.isEmpty()) {
            out.println("MKAPK_RESOLVED");
            return true;
        }

        // 6. VERSION MEDIATION (Highest Version Wins across project closures)

        VersionScheme versionScheme = new GenericVersionScheme();
        Map<String, GraphNode> mediatedNodes = new LinkedHashMap<>();

        for (GraphNode node : allProjectNodes) {
            String gaKey = node.getGaKey();
            if (!mediatedNodes.containsKey(gaKey)) {
                mediatedNodes.put(gaKey, node);
            } else {
                GraphNode currentBest = mediatedNodes.get(gaKey);
                try {
                    Version currentVersion = versionScheme.parseVersion(currentBest.version);
                    Version candidateVersion = versionScheme.parseVersion(node.version);
                    if (candidateVersion.compareTo(currentVersion) > 0) {
                        mediatedNodes.put(gaKey, node);
                    }
                } catch (Exception ignored) {}
            }
        }

        // Ensure winning files exist or are resolved
        Map<String, File> finalArtifacts = new LinkedHashMap<>();
        for (Map.Entry<String, GraphNode> entry : mediatedNodes.entrySet()) {
            GraphNode node = entry.getValue();
            File file = (node.file != null && node.file.exists()) ? node.file : new File(cacheDir, node.relativePath);

            if (!file.exists() && system != null) {
                file = resolveArtifactFile(system, session, repos, node.toArtifact());
            }

            if (file != null && file.exists()) {
                finalArtifacts.put(entry.getKey(), file);
            }
        }

        // 7. EMIT IPC OUTPUT

        StringBuilder sb = new StringBuilder("MKAPK_RESOLVED");
        for (File f : finalArtifacts.values()) {
            String name = f.getName();
            // Drop legacy split Kotlin stdlib artifacts
            if (name.contains("kotlin-stdlib-jdk7") || name.contains("kotlin-stdlib-jdk8")) {
                continue;
            }

            sb.append("|").append(f.getAbsolutePath());
            if (dynamicClassPathUrls != null) {
                try {
                    dynamicClassPathUrls.add(f.toURI().toURL());
                } catch (Exception ignored) {}
            }
        }

        out.println(sb.toString());
        return true;
    }
}
