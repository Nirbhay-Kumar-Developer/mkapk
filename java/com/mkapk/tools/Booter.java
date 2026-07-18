package com.mkapk.tools;

import org.apache.maven.repository.internal.MavenRepositorySystemUtils;
import org.eclipse.aether.DefaultRepositorySystemSession;
import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.connector.basic.BasicRepositoryConnectorFactory;
import org.eclipse.aether.impl.DefaultServiceLocator;
import org.eclipse.aether.repository.LocalRepository;
import org.eclipse.aether.spi.connector.RepositoryConnectorFactory;
import org.eclipse.aether.spi.connector.transport.TransporterFactory;
import org.eclipse.aether.transport.http.HttpTransporterFactory;

// Zero-DI Locking Bypasses
import org.eclipse.aether.impl.SyncContextFactory;
import org.eclipse.aether.SyncContext;
import org.eclipse.aether.artifact.Artifact;
import org.eclipse.aether.metadata.Metadata;

import java.io.File;
import java.util.Collection;

public class Booter {

        /**
     * Minimalist, programmatic Service Locator wrapper that cuts out Guice/Sisu lifecycle scans.
     */
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
     * Initializes a lightweight repo session mapped directly to local storage targets[span_7](start_span)[span_7](end_span).
     */
    public static RepositorySystemSession newRepositorySystemSession(RepositorySystem system, File localRepoDir) {
        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();

        LocalRepository localRepo = new LocalRepository(localRepoDir);
        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));

        // Disable remote tracking listeners to keep stdout clean for the C++ IPC layer[span_8](start_span)[span_8](end_span)
        session.setTransferListener(null);
        session.setRepositoryListener(null);

        return session;
    }

    /**
     * ============================================================================
     * PRIVATE NESTED CLASS: NoopSyncContextFactory
     * ============================================================================
     * Evicts standard multi-threading/multi-process locking mechanisms[span_9](start_span)[span_9](end_span). 
     * Signature matches the exact Type Erasure expected by Aether SPI[span_10](start_span)[span_10](end_span).
     */
    private static class NoopSyncContextFactory implements SyncContextFactory {
        @Override
        public SyncContext newInstance(RepositorySystemSession session, boolean shared) {
            return new SyncContext() {
                @Override
                public void acquire(Collection<? extends Artifact> artifacts,
                                    Collection<? extends Metadata> metadatas) {
                    // Fast path: No locking overhead on flash storage[span_11](start_span)[span_11](end_span)
                }

                @Override
                public void close() {
                    // Intentional no-op stub[span_12](start_span)[span_12](end_span)
                }
            };
        }
    }
}