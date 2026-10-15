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
import org.eclipse.aether.util.graph.visitor.PreorderNodeListGenerator;
import org.eclipse.aether.util.version.GenericVersionScheme;
import org.eclipse.aether.version.Version;
import org.eclipse.aether.version.VersionScheme;

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

    @Override
    public boolean execute(String[] args, PrintStream out, PrintStream err) throws Exception {
        if (args.length < 1) {
            out.println("[ERROR]|No coordinates supplied");
            return false;
        }

        RepositorySystem system = Booter.newRepositorySystem();
        RepositorySystemSession session = Booter.newRepositorySystemSession(system, getLocalCacheDir());

        // Remote repositories
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

        // PASS 1: Mediate Highest Version Wins for all Group:Artifact pairs
        VersionScheme versionScheme = new GenericVersionScheme();
        Map<String, Artifact> bestArtifacts = new LinkedHashMap<>();
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

            if (!bestArtifacts.containsKey(gaKey)) {
                bestArtifacts.put(gaKey, art);
            } else {
                Artifact currentBest = bestArtifacts.get(gaKey);
                try {
                    Version currentVersion = versionScheme.parseVersion(currentBest.getVersion());
                    Version candidateVersion = versionScheme.parseVersion(art.getVersion());
                    if (candidateVersion.compareTo(currentVersion) > 0) {
                        bestArtifacts.put(gaKey, art);
                    }
                } catch (Exception ignored) {}
            }
        }

        // PASS 2: Resolve the winning mediated artifacts
        Map<String, File> mediatedArtifacts = new LinkedHashMap<>();

        for (Map.Entry<String, Artifact> entry : bestArtifacts.entrySet()) {
            String gaKey = entry.getKey();
            Artifact art = entry.getValue();

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
