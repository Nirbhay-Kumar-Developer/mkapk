package com.mkapk.tools;

import java.io.File;
import java.io.PrintStream;
import java.net.URL;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

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

public class DependencyResolver implements ToolHandler {

    private final Set<URL> dynamicClassPathUrls;

    public DependencyResolver(Set<URL> dynamicClassPathUrls) {
        this.dynamicClassPathUrls = dynamicClassPathUrls;
    }

    private static File getLocalCacheDir() {
        String termuxPrefix = System.getenv("PREFIX");
        if (termuxPrefix == null || termuxPrefix.isEmpty()) {
            termuxPrefix = System.getProperty("user.home") + "/.mkapk";
        }
        
        File cacheDir = new File(termuxPrefix + "/var/lib/mkapk/lib");
        if (!cacheDir.exists()) {
            cacheDir.mkdirs();
        }
        return cacheDir;
    }

    @Override
    public boolean execute(String[] args, PrintStream outStream, PrintStream errStream) throws Exception {
        if (args.length < 1) {
            outStream.println("[ERROR]|Provide at least one maven coordinate");
            return false;
        }

        RepositorySystem system = Booter.newRepositorySystem();
        RepositorySystemSession session = Booter.newRepositorySystemSession(system, getLocalCacheDir());

        RemoteRepository googleRepo = new RemoteRepository.Builder("google", "default", "https://dl.google.com/dl/android/maven2/").build();
        RemoteRepository centralRepo = new RemoteRepository.Builder("central", "default", "https://repo1.maven.org/maven2/").build();
        List<RemoteRepository> repos = Arrays.asList(googleRepo, centralRepo);

        CollectRequest collectRequest = new CollectRequest();
        for (RemoteRepository repo : repos) {
            collectRequest.addRepository(repo);
        }

        // Add root dependencies using standard G:A:V coordinates
        for (String rawCoordinate : args) {
            if (rawCoordinate == null || rawCoordinate.trim().isEmpty()) continue;
            
            String coord = rawCoordinate.trim();
            // Normalize: If coordinate is group:artifact:version, keep as standard artifact
            String[] parts = coord.split(":");
            if (parts.length == 3) {
                collectRequest.addDependency(new Dependency(new DefaultArtifact(parts[0], parts[1], "", "aar", parts[2]), JavaScopes.COMPILE));
            } else {
                collectRequest.addDependency(new Dependency(new DefaultArtifact(coord), JavaScopes.COMPILE));
            }
        }

        CollectResult collectResult;
        try {
            collectResult = system.collectDependencies(session, collectRequest);
        } catch (DependencyCollectionException e) {
            collectResult = e.getResult();
            outStream.println("[WARN]|Partial graph collection: " + e.getMessage());
        }

        if (collectResult == null || collectResult.getRoot() == null) {
            outStream.println("MKAPK_RESOLVED");
            return true;
        }

        // Traverse the mediated graph to collect all transitive and direct nodes
        PreorderNodeListGenerator nlg = new PreorderNodeListGenerator();
        collectResult.getRoot().accept(nlg);

        Set<File> resolvedFiles = new LinkedHashSet<>();
        List<DependencyNode> nodes = nlg.getNodes();

        for (DependencyNode node : nodes) {
            Dependency dep = node.getDependency();
            if (dep == null || dep.getArtifact() == null) continue;

            // Skip test or provided scopes
            String scope = dep.getScope();
            if (JavaScopes.TEST.equalsIgnoreCase(scope) || JavaScopes.PROVIDED.equalsIgnoreCase(scope)) {
                continue;
            }

            Artifact art = dep.getArtifact();
            File resolvedFile = null;

            // 1. Try resolving as AAR
            Artifact aarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "aar", art.getVersion());
            try {
                ArtifactResult aarRes = system.resolveArtifact(session, new ArtifactRequest(aarArtifact, repos, null));
                if (aarRes.isResolved() && aarRes.getArtifact().getFile() != null) {
                    resolvedFile = aarRes.getArtifact().getFile();
                }
            } catch (Exception ignored) {}

            // 2. If no AAR is available, resolve as standard JAR
            if (resolvedFile == null) {
                Artifact jarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "jar", art.getVersion());
                try {
                    ArtifactResult jarRes = system.resolveArtifact(session, new ArtifactRequest(jarArtifact, repos, null));
                    if (jarRes.isResolved() && jarRes.getArtifact().getFile() != null) {
                        resolvedFile = jarRes.getArtifact().getFile();
                    }
                } catch (Exception ignored) {}
            }

            if (resolvedFile != null && resolvedFile.exists()) {
                resolvedFiles.add(resolvedFile);
            }
        }

        // Pipe mediated file paths back to the C++ orchestrator
        StringBuilder resolvedPaths = new StringBuilder("MKAPK_RESOLVED");
        for (File f : resolvedFiles) {
            String name = f.getName();
            // Ignore legacy jdk7/8 split artifacts bundled with Kotlin
            if (name.contains("kotlin-stdlib-jdk7") || name.contains("kotlin-stdlib-jdk8")) {
                continue;
            }
            
            resolvedPaths.append("|").append(f.getAbsolutePath());
            try {
                if (dynamicClassPathUrls != null) {
                    dynamicClassPathUrls.add(f.toURI().toURL());
                }
            } catch (Exception ignored) {}
        }

        outStream.println(resolvedPaths.toString());
        return true;
    }
}
