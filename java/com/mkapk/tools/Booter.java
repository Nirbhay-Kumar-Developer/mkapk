package com.mkapk.tools;

import org.apache.maven.repository.internal.MavenRepositorySystemUtils;
import org.eclipse.aether.DefaultRepositorySystemSession;
import org.eclipse.aether.RepositorySystem;
import org.eclipse.aether.RepositorySystemSession;
import org.eclipse.aether.artifact.DefaultArtifactType;
import org.eclipse.aether.repository.LocalRepository;
import org.eclipse.aether.repository.RepositoryPolicy;
import org.eclipse.aether.supplier.RepositorySystemSupplier;
import org.eclipse.aether.util.artifact.DefaultArtifactTypeRegistry;

import java.io.File;

public class Booter {

    /**
     * Initializes RepositorySystem using Resolver 2.x Supplier mechanism.
     */
    public static RepositorySystem newRepositorySystem() {
        return new RepositorySystemSupplier().get();
    }

    /**
     * Initializes a repository session mapped directly to local storage targets.
     */
    public static RepositorySystemSession newRepositorySystemSession(RepositorySystem system, File localRepoDir) {
        if (system == null) {
            throw new IllegalStateException("RepositorySystem initialization failed. Verify that core dependencies exist inside the daemon's active classpath.");
        }

        DefaultRepositorySystemSession session = MavenRepositorySystemUtils.newSession();

        LocalRepository localRepo = new LocalRepository(localRepoDir);
        session.setLocalRepositoryManager(system.newLocalRepositoryManager(session, localRepo));

        org.eclipse.aether.artifact.ArtifactTypeRegistry existingRegistry = session.getArtifactTypeRegistry();
        DefaultArtifactTypeRegistry stereotypes;

        if (existingRegistry instanceof DefaultArtifactTypeRegistry) {
            stereotypes = (DefaultArtifactTypeRegistry) existingRegistry;
        } else {
            stereotypes = new DefaultArtifactTypeRegistry();
        }

        stereotypes.add(new DefaultArtifactType("aar", "aar", "", "java", true, true));
        session.setArtifactTypeRegistry(stereotypes);

        session.setChecksumPolicy(RepositoryPolicy.CHECKSUM_POLICY_WARN);
        session.setTransferListener(null);
        session.setRepositoryListener(null);

        return session;
    }
}
