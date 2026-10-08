package com.mkapk.tools;

import org.apache.maven.repository.internal.MavenRepositorySystemUtils;
import org.eclipse.aether.DefaultRepositorySystemSession;
import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.artifact.Artifact;
import org.eclipse.aether.artifact.DefaultArtifact;
import org.eclipse.aether.artifact.DefaultArtifactType;
import org.eclipse.aether.collection.CollectRequest;
import org.eclipse.aether.graph.Dependency;
import org.eclipse.aether.graph.DependencyFilter;
import org.eclipse.aether.repository.LocalRepository;
import org.eclipse.aether.repository.RemoteRepository;
import org.eclipse.aether.resolution.ArtifactResult;
import org.eclipse.aether.resolution.DependencyRequest;
import org.eclipse.aether.resolution.DependencyResult;
import org.eclipse.aether.util.artifact.DefaultArtifactTypeRegistry;
import org.eclipse.aether.util.artifact.JavaScopes;
import org.eclipse.aether.util.filter.DependencyFilterUtils;

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

    @Override
    public boolean execute(String[] args, PrintStream out, PrintStream err) throws Exception {
        if (args.length < 1) {
            out.println("[ERROR]|No coordinates supplied");
            return false;
        }

        RepositorySystem system = Booter.newRepositorySystem();
        
        // 1. Construct Maven session and register proper artifact handlers for aar and jar
        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();
        LocalRepository localRepo = new LocalRepository(getLocalCacheDir());
        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));

        DefaultArtifactTypeRegistry typeRegistry = new DefaultArtifactTypeRegistry();
        // Crucial: define "aar" packaging type so transitive AARs resolve to their .aar binaries
        typeRegistry.add(new DefaultArtifactType("aar", "aar", "", "java", false, true));
        typeRegistry.add(new DefaultArtifactType("jar", "jar", "", "java", false, false));
        session.setArtifactTypeRegistry(typeRegistry);

        // 2. Configure Repositories
        RemoteRepository google = new RemoteRepository.Builder("google", "default", "https://dl.google.com/dl/android/maven2/").build();
        RemoteRepository central = new RemoteRepository.Builder("central", "default", "https://repo1.maven.org/maven2/").build();
        List<RemoteRepository> repos = Arrays.asList(google, central);

        // 3. Assemble CollectRequest
        CollectRequest collectRequest = new CollectRequest();
        collectRequest.setRepositories(repos);

        for (String raw : args) {
            if (raw == null || raw.trim().isEmpty()) continue;
            String coord = raw.trim();
            collectRequest.addDependency(new Dependency(new DefaultArtifact(coord), JavaScopes.COMPILE));
        }

        // 4. Resolve dependencies transitively using DependencyRequest
        // Exclude test and provided scopes cleanly
        DependencyFilter classpathFilter = DependencyFilterUtils.classpathFilter(
            JavaScopes.COMPILE,
            JavaScopes.RUNTIME
        );

        DependencyRequest dependencyRequest = new DependencyRequest(collectRequest, classpathFilter);
        DependencyResult dependencyResult;

        try {
            dependencyResult = system.resolveDependencies(session, dependencyRequest);
        } catch (Exception e) {
            out.println("[WARN]|Transitive resolution warning: " + e.getMessage());
            // Attempt to retrieve partial results if graph resolution encountered an issue
            if (e instanceof org.eclipse.aether.resolution.DependencyResolutionException) {
                dependencyResult = ((org.eclipse.aether.resolution.DependencyResolutionException) e).getResult();
            } else {
                dependencyResult = null;
            }
        }

        if (dependencyResult == null || dependencyResult.getArtifactResults() == null) {
            out.println("MKAPK_RESOLVED");
            return true;
        }

        // 5. Collect and deduplicate resolved files (map GA to mediated version)
        Map<String, File> mediatedArtifacts = new LinkedHashMap<>();

        for (ArtifactResult res : dependencyResult.getArtifactResults()) {
            if (!res.isResolved() || res.getArtifact() == null) continue;

            Artifact art = res.getArtifact();
            File file = art.getFile();
            if (file == null || !file.exists()) continue;

            // Group:Artifact key to prevent version collision duplicates
            String gaKey = art.getGroupId() + ":" + art.getArtifactId();
            if (!mediatedArtifacts.containsKey(gaKey)) {
                mediatedArtifacts.put(gaKey, file);
            }
        }

        // 6. Return pipe-separated absolute paths to C++ coordinator
        StringBuilder sb = new StringBuilder("MKAPK_RESOLVED");
        for (File f : mediatedArtifacts.values()) {
            String name = f.getName();
            // Ignore legacy kotlin stdlib modules
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
