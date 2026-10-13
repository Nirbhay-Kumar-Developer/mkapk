package com.mkapk.tools;

import org.apache.maven.repository.internal.MavenRepositorySystemUtils;
import org.eclipse.aether.DefaultRepositorySystemSession;
import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.artifact.ArtifactTypeRegistry;
import org.eclipse.aether.artifact.DefaultArtifactType;
import org.eclipse.aether.collection.DependencyCollectionContext;
import org.eclipse.aether.collection.DependencySelector;
import org.eclipse.aether.collection.DependencyTraverser;
import org.eclipse.aether.graph.Dependency;
import org.eclipse.aether.repository.LocalRepository;
import org.eclipse.aether.repository.RepositoryPolicy;
import org.eclipse.aether.supplier.RepositorySystemSupplier;
import org.eclipse.aether.util.artifact.DefaultArtifactTypeRegistry;
import org.eclipse.aether.util.artifact.JavaScopes;

import java.io.File;

public class Booter {

    /**
     * Initializes the RepositorySystem using Resolver 2.x Supplier mechanism.
     */
    public static RepositorySystem newRepositorySystem() {
        return new RepositorySystemSupplier().get();
    }

    /**
     * Retains compile, runtime, and optional dependencies,
     * discarding only test, provided, and system scopes.
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
     * preventing optional and transitive runtime components from being pruned.
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

    /**
     * Configures a repository session mapped to local storage, preserving built-in
     * POM handlers, registering AAR/JAR packaging, enforcing full graph traversal,
     * and silencing listeners to protect stdout IPC streams.
     */
    public static RepositorySystemSession newRepositorySystemSession(RepositorySystem system, File localRepoDir) {
        if (system == null) {
            throw new IllegalStateException("RepositorySystem initialization failed. Verify dependencies in daemon classpath.");
        }

        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();

        LocalRepository localRepo = new LocalRepository(localRepoDir);
        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));

        // 1. Full transitive graph traversal rules
        session.setDependencySelector(new PermissiveDependencySelector());
        session.setDependencyTraverser(new AllDependencyTraverser());
        session.setConfigProperty("aether.dependencyCollector.useSkip", false);

        // 2. Preserve default Maven stereotypes (e.g., 'pom') and append 'aar' and 'jar'
        ArtifactTypeRegistry existingRegistry = session.getArtifactTypeRegistry();
        DefaultArtifactTypeRegistry stereotypes = (existingRegistry instanceof DefaultArtifactTypeRegistry)
                ? (DefaultArtifactTypeRegistry) existingRegistry
                : new DefaultArtifactTypeRegistry();

        stereotypes.add(new DefaultArtifactType("aar", "aar", "", "java", false, true));
        stereotypes.add(new DefaultArtifactType("jar", "jar", "", "java", false, false));
        session.setArtifactTypeRegistry(stereotypes);

        // 3. Tolerant checksum validation
        session.setChecksumPolicy(RepositoryPolicy.CHECKSUM_POLICY_WARN);

        // 4. Mute console listeners to prevent stdout stream pollution over C++ IPC
        session.setTransferListener(null);
        session.setRepositoryListener(null);

        return session;
    }
}
