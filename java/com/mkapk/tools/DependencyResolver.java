package com.mkapk.tools;

import org.apache.maven.repository.internal.MavenRepositorySystemUtils;
import org.eclipse.aether.DefaultRepositorySystemSession;
import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.artifact.Artifact;
import org.eclipse.aether.artifact.DefaultArtifact;
import org.eclipse.aether.artifact.DefaultArtifactType;
import org.eclipse.aether.collection.CollectRequest;
import org.eclipse.aether.collection.CollectResult;
import org.eclipse.aether.collection.DependencyCollectionContext;
import org.eclipse.aether.collection.DependencyCollectionException;
import org.eclipse.aether.collection.DependencySelector;
import org.eclipse.aether.collection.DependencyTraverser;
import org.eclipse.aether.graph.Dependency;
import org.eclipse.aether.graph.DependencyNode;
import org.eclipse.aether.repository.LocalRepository;
import org.eclipse.aether.repository.RemoteRepository;
import org.eclipse.aether.resolution.ArtifactRequest;
import org.eclipse.aether.resolution.ArtifactResult;
import org.eclipse.aether.util.artifact.DefaultArtifactTypeRegistry;
import org.eclipse.aether.util.artifact.JavaScopes;
import org.eclipse.aether.util.graph.visitor.PreorderNodeListGenerator;

import java.io.File;
import java.io.PrintStream;
import java.net.URL;
import java.util.*;

public class DependencyResolver implements ToolHandler {

    private final Set<URL> dynamicClassPathUrls;

    public DependencyResolver(Set<URL> dynamicClassPathUrls) {
        this.dynamicClassPathUrls = dynamicClassPathUrls;
    }

    private static File getLocalCacheDir() {
        String prefix = System.getenv("PREFIX");
        if (prefix == null || prefix.isEmpty()) {
            prefix = System.getProperty("user.home") + "/.mkapk";
        }
        File cacheDir = new File(prefix, "var/lib/mkapk/lib");
        if (!cacheDir.exists()) {
            cacheDir.mkdirs();
        }
        return cacheDir;
    }

    /**
     * Retains compile, runtime, and optional dependencies,
     * discarding only test and provided scopes.
     */
    private static class PermissiveDependencySelector implements DependencySelector {
        @Override
        public boolean selectDependency(Dependency dependency) {
            if (dependency == null) return false;
            String scope = dependency.getScope();
            return !JavaScopes.TEST.equalsIgnoreCase(scope)
                    && !JavaScopes.PROVIDED.equalsIgnoreCase(scope)
                    && !"system".equalsIgnoreCase(scope);
        }

        @Override
        public DependencySelector deriveChildSelector(DependencyCollectionContext context) {
            return this;
        }
    }

    /**
     * Forces Aether to traverse into child POMs for every dependency,
     * preventing optional runtime components from being skipped.
     */
    private static class AllDependencyTraverser implements DependencyTraverser {
        @Override
        public boolean traverseDependency(Dependency dependency) {
            return true;
        }

        @Override
        public DependencyTraverser deriveChildTraverser(DependencyCollectionContext context) {
            return this;
        }
    }

    @Override
    public boolean execute(String[] args, PrintStream out, PrintStream err) throws Exception {
        if (args.length < 1) {
            out.println("[ERROR]|No coordinates supplied");
            return false;
        }

        RepositorySystem system = Booter.newRepositorySystem();
        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();
        LocalRepository localRepo = new LocalRepository(getLocalCacheDir());
        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));

        // Enforce full graph traversal without scope/optional pruning in Resolver 2.x
        session.setDependencySelector(new PermissiveDependencySelector());
        session.setDependencyTraverser(new AllDependencyTraverser());
        session.setConfigProperty("aether.dependencyCollector.useSkip", false);

        // Register AAR and JAR packaging handlers
        DefaultArtifactTypeRegistry typeRegistry = new DefaultArtifactTypeRegistry();
        typeRegistry.add(new DefaultArtifactType("aar", "aar", "", "java", false, true));
        typeRegistry.add(new DefaultArtifactType("jar", "jar", "", "java", false, false));
        session.setArtifactTypeRegistry(typeRegistry);

        // Repositories
        RemoteRepository google = new RemoteRepository.Builder("google", "default", "https://dl.google.com/dl/android/maven2/").build();
        RemoteRepository central = new RemoteRepository.Builder("central", "default", "https://repo1.maven.org/maven2/").build();
        List<RemoteRepository> repos = Arrays.asList(google, central);

        CollectRequest collectRequest = new CollectRequest();
        collectRequest.setRepositories(repos);

        // Coordinate normalization
        for (String raw : args) {
            if (raw == null || raw.trim().isEmpty()) continue;
            String coord = raw.trim();
            String[] parts = coord.split(":");
            Artifact artifact;
            if (parts.length == 3) {
                artifact = new DefaultArtifact(parts[0], parts[1], "", "aar", parts[2]);
            } else {
                artifact = new DefaultArtifact(coord);
            }
            collectRequest.addDependency(new Dependency(artifact, JavaScopes.COMPILE));
        }

        // Build transitive dependency graph
        CollectResult collectResult;
        try {
            collectResult = system.collectDependencies(session, collectRequest);
        } catch (DependencyCollectionException dce) {
            collectResult = dce.getResult();
        } catch (Exception e) {
            collectResult = null;
        }

        if (collectResult == null || collectResult.getRoot() == null) {
            out.println("MKAPK_RESOLVED");
            return true;
        }

        // Extract nodes in pre-order traversal
        PreorderNodeListGenerator nlg = new PreorderNodeListGenerator();
        collectResult.getRoot().accept(nlg);

        Map<String, File> mediatedArtifacts = new LinkedHashMap<>();
        List<DependencyNode> nodes = nlg.getNodes();

        for (DependencyNode node : nodes) {
            Dependency dep = node.getDependency();
            if (dep == null || dep.getArtifact() == null) continue;

            String scope = dep.getScope();
            if (JavaScopes.TEST.equalsIgnoreCase(scope) || JavaScopes.PROVIDED.equalsIgnoreCase(scope) || "system".equalsIgnoreCase(scope)) {
                continue;
            }

            Artifact art = dep.getArtifact();
            String gaKey = art.getGroupId() + ":" + art.getArtifactId();
            if (mediatedArtifacts.containsKey(gaKey)) {
                continue;
            }

            File resolvedFile = null;
            boolean preferAar = "aar".equalsIgnoreCase(art.getExtension()) || art.getGroupId().startsWith("androidx.");

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

            // Strategy 3: Fallback attempt as AAR if direct download failed
            if (resolvedFile == null && !preferAar) {
                Artifact aarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "aar", art.getVersion());
                try {
                    ArtifactResult aarRes = system.resolveArtifact(session, new ArtifactRequest(aarArtifact, repos, null));
                    if (aarRes.isResolved() && aarRes.getArtifact().getFile() != null) {
                        resolvedFile = aarRes.getArtifact().getFile();
                    }
                } catch (Exception ignored) {}
            }

            if (resolvedFile != null && resolvedFile.exists()) {
                mediatedArtifacts.put(gaKey, resolvedFile);
            }
        }

        // Return pipe-separated absolute paths back to C++ coordinator
        StringBuilder sb = new StringBuilder("MKAPK_RESOLVED");
        for (File f : mediatedArtifacts.values()) {
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
