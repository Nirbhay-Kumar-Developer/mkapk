package com.mkapk.tools;

import java.io.File;
import java.io.PrintStream;
import java.net.URL;
import java.util.Arrays;
import java.util.LinkedHashSet;
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

        Set<File> resolvedFiles = new LinkedHashSet<>();

        // Batch all coordinates into a single request for automatic version mediation
        CollectRequest collectRequest = new CollectRequest();
        for (String rawCoordinate : args) {
            if (rawCoordinate == null || rawCoordinate.trim().isEmpty()) continue;
            
            String[] coordParts = rawCoordinate.split(":");
            String primaryCoordinate = rawCoordinate;
            if (coordParts.length == 3) {
                primaryCoordinate = coordParts[0] + ":" + coordParts[1] + ":aar:" + coordParts[2];
            }
            
            collectRequest.addDependency(new Dependency(new DefaultArtifact(primaryCoordinate), JavaScopes.COMPILE));
        }

        collectRequest.addRepository(googleRepo);
        collectRequest.addRepository(centralRepo);

        CollectResult collectResult = null;
        try {
            collectResult = system.collectDependencies(session, collectRequest);
        } catch (DependencyCollectionException e) {
            collectResult = e.getResult();
            outStream.println("[WARN]|Partial graph collection: " + e.getMessage());
        }

        // FULL GRAPH TRAVERSAL: Use Aether's built-in generator to preserve correctly mediated versions
        PreorderNodeListGenerator nlg = new PreorderNodeListGenerator();
        if (collectResult != null && collectResult.getRoot() != null) {
            collectResult.getRoot().accept(nlg);
        }

        // Resolve local files for ALL mediated direct & transitive artifacts
        for (DependencyNode node : nlg.getNodes()) {
            if (node.getDependency() != null && node.getDependency().getArtifact() != null) {
                Artifact art = node.getDependency().getArtifact();
                
                // Attempt AAR first
                Artifact aarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "aar", art.getVersion());
                ArtifactRequest aarReq = new ArtifactRequest(aarArtifact, Arrays.asList(googleRepo, centralRepo), null);
                try {
                    ArtifactResult aarRes = system.resolveArtifact(session, aarReq);
                    if (aarRes.isResolved() && aarRes.getArtifact().getFile() != null) {
                        resolvedFiles.add(aarRes.getArtifact().getFile());
                        continue; 
                    }
                } catch (Exception ignored) {}

                // Fallback to JAR
                Artifact jarArtifact = new DefaultArtifact(art.getGroupId(), art.getArtifactId(), art.getClassifier(), "jar", art.getVersion());
                ArtifactRequest jarReq = new ArtifactRequest(jarArtifact, Arrays.asList(googleRepo, centralRepo), null);
                try {
                    ArtifactResult jarRes = system.resolveArtifact(session, jarReq);
                    if (jarRes.isResolved() && jarRes.getArtifact().getFile() != null) {
                        resolvedFiles.add(jarRes.getArtifact().getFile());
                    }
                } catch (Exception ignored) {}
            }
        }

        // Pipe all resolved absolute file paths back to C++
        StringBuilder resolvedPaths = new StringBuilder("MKAPK_RESOLVED");
        for (File f : resolvedFiles) {
            if (f != null && f.exists()) {
                String name = f.getName();
                if (name.contains("kotlin-stdlib-jdk7") || name.contains("kotlin-stdlib-jdk8")) {
                    continue; 
                }
                
                resolvedPaths.append("|").append(f.getAbsolutePath());
                try {
                    dynamicClassPathUrls.add(f.toURI().toURL());
                } catch (Exception ignored) {}
            }
        }

        outStream.println(resolvedPaths.toString());
        return true;
    }
}