package com.snurhythm.asobmashow;

import java.io.IOException;

/** Coordinates inspection and user choice without copying a seekable source. */
final class ChartArchiveImport {
    static final int CANCEL = 0, USE_ARCHIVE = 1, EXTRACT = 2;
    static final class Cancelled extends IOException {
        Cancelled() { super("Chart import cancelled."); }
    }
    static final class Result {
        final String path, uri;
        final boolean newGrant;
        Result(String path, String uri, boolean newGrant) {
            this.path=path; this.uri=uri; this.newGrant=newGrant;
        }
    }
    interface Backend {
        String referencePath();
        String uri();
        String inspect(String path, String uri) throws Exception;
        String stage() throws Exception;
        boolean hasPermission();
        boolean canPersist();
        boolean persist();
        int choose(String kind, boolean direct) throws Exception;
        void checkpoint() throws Exception;
        void discard(String path, String newlyGrantedUri);
    }
    static Result run(Backend backend) throws Exception {
        String path=backend.referencePath();
        boolean complete=false, newGrant=false, staged=false;
        try {
            backend.checkpoint();
            String inspection=backend.inspect(path, backend.uri());
            if (inspection.startsWith("__STREAM_ONLY__:")) {
                String copied=backend.stage();
                backend.discard(path, "");
                path=copied;
                staged=true;
                inspection=backend.inspect(path, "");
            }
            if (inspection.startsWith("__ERROR__:")) throw new IOException(inspection.substring(10));
            if (!inspection.equals("solid") && !inspection.equals("non-solid"))
                throw new IOException("Could not inspect archive.");
            backend.checkpoint();
            boolean canKeep=!staged && inspection.equals("non-solid")
                    && (backend.hasPermission() || backend.canPersist());
            int choice=backend.choose(inspection, canKeep);
            if (choice==CANCEL) throw new Cancelled();
            backend.checkpoint();
            String retainedUri="";
            if (choice==USE_ARCHIVE) {
                if (!canKeep) throw new IOException("This archive must be extracted.");
                if (!backend.hasPermission()) {
                    if (!backend.persist()) throw new IOException("Could not retain archive access. Select the archive with Import Archive or extract it.");
                    newGrant=true;
                }
                retainedUri=backend.uri();
            } else if (choice!=EXTRACT) {
                throw new Cancelled();
            }
            backend.checkpoint();
            complete=true;
            return new Result(path, retainedUri, newGrant);
        } finally {
            if (!complete) backend.discard(path, newGrant ? backend.uri() : "");
        }
    }
}
