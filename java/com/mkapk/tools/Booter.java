package com.mkapk.tools;

import org.apache.maven.repository.internal.MavenRepositorySystemUtils;
import org.eclipse.aether.DefaultRepositorySystemSession;
import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.artifact.DefaultArtifactType;
import org.eclipse.aether.connector.basic.BasicRepositoryConnectorFactory;
import org.eclipse.aether.impl.DefaultServiceLocator;
import org.eclipse.aether.repository.LocalRepository;
import org.eclipse.aether.repository.RepositoryPolicy;
import org.eclipse.aether.spi.connector.RepositoryConnectorFactory;
import org.eclipse.aether.spi.connector.transport.TransporterFactory;
import org.eclipse.aether.transport.http.HttpTransporterFactory;
import org.eclipse.aether.util.artifact.DefaultArtifactTypeRegistry;

// Zero-DI Locking Bypasses
import org.eclipse.aether.impl.SyncContextFactory;
import org.eclipse.aether.SyncContext;
import org.eclipse.aether.artifact.Artifact;
import org.eclipse.aether.metadata.Metadata;

import java.io.File;
import java.util.Collection;

public class Booter {

    public static RepositorySystem newRepositorySystem() {
        DefaultServiceLocator locator = MavenRepositorySystemUtils.newServiceLocator();
        
        // 1. Explicit Programmatic Bindings (Zero-Reflection Path)
        locator.addService(RepositoryConnectorFactory.class, BasicRepositoryConnectorFactory.class);
        locator.addService(TransporterFactory.class, HttpTransporterFactory.class);

        // 2. Pass the Class blueprint instead of an initialized instance
        locator.setService(SyncContextFactory.class, NoopSyncContextFactory.class);

        locator.setErrorHandler(new DefaultServiceLocator.ErrorHandler() {
            @Override
            public void serviceCreationFailed(Class<?> type, Class<?> impl, Throwable exception) {
                System.err.println("[WARN]|Service creation failed for " + type.getName() + ": " + exception.getMessage());
            }
        });

        return locator.getService(RepositorySystem.class);
    }

    /**
     * Initializes a lightweight repo session mapped directly to local storage targets.
     */
    public static RepositorySystemSession newRepositorySystemSession(RepositorySystem system, File localRepoDir) {
        if (system == null) {
            throw new IllegalStateException("RepositorySystem initialization failed. Verify that core dependencies exist inside the daemon's active classpath.");
        }

        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();

        LocalRepository localRepo = new LocalRepository(localRepoDir);
        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));

        // CRITICAL FIX: Retrieve the default Maven registry (so 'pom' is preserved) and APPEND 'aar'
        org.eclipse.aether.artifact.ArtifactTypeRegistry existingRegistry = session.getArtifactTypeRegistry();
        DefaultArtifactTypeRegistry stereotypes;
        
        if (existingRegistry instanceof DefaultArtifactTypeRegistry) {
            stereotypes = (DefaultArtifactTypeRegistry) existingRegistry;
        } else {
            stereotypes = new DefaultArtifactTypeRegistry();
        }
        
        // includesDependencies = false (Enables transitive traversal for AARs)
        // addedToClasspath     = true  (Includes resolved artifacts in build path)
        stereotypes.add(new DefaultArtifactType("aar", "aar", "", "java", true, true));
        session.setArtifactTypeRegistry(stereotypes);

        session.setChecksumPolicy(RepositoryPolicy.CHECKSUM_POLICY_WARN);

        // Disable remote tracking listeners to keep stdout clean for the C++ IPC layer
        session.setTransferListener(null);
        session.setRepositoryListener(null);

        return session;
    }

    private static class NoopSyncContextFactory implements SyncContextFactory {
        @Override
        public SyncContext newInstance(RepositorySystemSession session, boolean shared) {
            return new SyncContext() {
                @Override
                public void acquire(Collection<? extends Artifact> artifacts,
                                    Collection<? extends Metadata> metadatas) {}

                @Override
                public void close() {}
            };
        }
    }
}