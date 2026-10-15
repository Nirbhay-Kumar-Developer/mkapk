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

    public static RepositorySystem newRepositorySystem() {
        return new RepositorySystemSupplier().get();
    }

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

    public static RepositorySystemSession newRepositorySystemSession(RepositorySystem system, File localRepoDir) {
        if (system == null) {
            throw new IllegalStateException("RepositorySystem initialization failed.");
        }

        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();
        LocalRepository localRepo = new LocalRepository(localRepoDir);
        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));

        session.setDependencySelector(new PermissiveDependencySelector());
        session.setDependencyTraverser(new AllDependencyTraverser());
        session.setConfigProperty("aether.dependencyCollector.useSkip", false);

        ArtifactTypeRegistry existingRegistry = session.getArtifactTypeRegistry();
        DefaultArtifactTypeRegistry stereotypes = (existingRegistry instanceof DefaultArtifactTypeRegistry)
                ? (DefaultArtifactTypeRegistry) existingRegistry
                : new DefaultArtifactTypeRegistry();

        stereotypes.add(new DefaultArtifactType("aar", "aar", "", "java", false, true));
        stereotypes.add(new DefaultArtifactType("jar", "jar", "", "java", false, false));
        session.setArtifactTypeRegistry(stereotypes);

        session.setChecksumPolicy(RepositoryPolicy.CHECKSUM_POLICY_WARN);
        session.setTransferListener(null);
        session.setRepositoryListener(null);

        return session;
    }
}
