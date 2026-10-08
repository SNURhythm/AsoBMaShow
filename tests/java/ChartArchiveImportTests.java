package com.snurhythm.asobmashow;

public final class ChartArchiveImportTests {
    static void require(boolean value) { if (!value) throw new AssertionError(); }
    static final class Backend implements ChartArchiveImport.Backend {
        String inspection = "non-solid";
        int choice = ChartArchiveImport.USE_ARCHIVE;
        boolean persistable = true, persisted, failPersist, cancelled, cancelAfterPersist;
        int copies, discards, permissionReleases, choices;
        String choiceKind;
        boolean directOffered;
        public String referencePath() { return "@androidarchive@/id/test.zip"; }
        public String uri() { return "content://source"; }
        public String inspect(String path, String uri) { return path.equals("staged.zip") ? "non-solid" : inspection; }
        public String stage() { ++copies; return "staged.zip"; }
        public boolean hasPermission() { return persisted; }
        public boolean canPersist() { return persistable; }
        public boolean persist() { if (failPersist) return false; persisted = true; cancelled = cancelAfterPersist; return true; }
        public int choose(String kind, boolean direct) { ++choices; choiceKind=kind; directOffered=direct; return cancelled ? ChartArchiveImport.CANCEL : choice; }
        public void checkpoint() throws Exception { if (cancelled) throw new Exception("cancelled"); }
        public void discard(String path, String newlyGrantedUri) { ++discards; if (!newlyGrantedUri.isEmpty()) ++permissionReleases; }
    }
    public static void main(String[] args) throws Exception {
        Backend direct = new Backend();
        var result = ChartArchiveImport.run(direct);
        require(direct.copies == 0 && direct.directOffered && result.path.equals("@androidarchive@/id/test.zip"));
        require(result.uri.equals("content://source") && result.newGrant && direct.discards == 0);
        Backend solid = new Backend(); solid.inspection="solid"; solid.choice=ChartArchiveImport.EXTRACT;
        result=ChartArchiveImport.run(solid);
        require(!solid.directOffered && solid.choiceKind.equals("solid") && result.uri.isEmpty() && solid.copies==0);
        Backend extract = new Backend(); extract.choice=ChartArchiveImport.EXTRACT;
        result=ChartArchiveImport.run(extract);
        require(extract.directOffered && result.uri.isEmpty() && extract.copies==0);
        Backend stream = new Backend(); stream.inspection="__STREAM_ONLY__:pipe"; stream.choice=ChartArchiveImport.EXTRACT;
        result=ChartArchiveImport.run(stream);
        require(stream.copies==1 && !stream.directOffered && result.path.equals("staged.zip"));
        Backend transientGrant = new Backend(); transientGrant.persistable=false; transientGrant.choice=ChartArchiveImport.EXTRACT;
        result=ChartArchiveImport.run(transientGrant);
        require(!transientGrant.directOffered && !transientGrant.persisted && result.uri.isEmpty());
        Backend cancelled = new Backend(); cancelled.choice=ChartArchiveImport.CANCEL;
        try { ChartArchiveImport.run(cancelled); throw new AssertionError(); } catch (ChartArchiveImport.Cancelled expected) {}
        require(cancelled.copies==0 && cancelled.discards==1);
        Backend corrupt = new Backend(); corrupt.inspection="__ERROR__:corrupt";
        try { ChartArchiveImport.run(corrupt); throw new AssertionError(); } catch (Exception expected) { require(expected.getMessage().equals("corrupt")); }
        require(corrupt.choices==0 && corrupt.copies==0 && corrupt.discards==1);
        Backend permission = new Backend(); permission.failPersist=true;
        try { ChartArchiveImport.run(permission); throw new AssertionError(); } catch (Exception expected) {}
        require(permission.discards==1 && permission.permissionReleases==0);
        Backend lateCancel = new Backend(); lateCancel.cancelAfterPersist=true;
        try { ChartArchiveImport.run(lateCancel); throw new AssertionError(); } catch (Exception expected) {}
        require(lateCancel.permissionReleases==1 && lateCancel.discards==1 && lateCancel.copies==0);
        Backend existing = new Backend(); existing.persisted=true;
        require(!ChartArchiveImport.run(existing).newGrant);
        java.util.Set<String> saved = new java.util.HashSet<>();
        ArchivePermissionOwnership.Store store = new ArchivePermissionOwnership.Store() {
            public java.util.Set<String> read() { return new java.util.HashSet<>(saved); }
            public boolean write(java.util.Set<String> value) { saved.clear(); saved.addAll(value); return true; }
        };
        int[] released = {0};
        require(ArchivePermissionOwnership.release(store, "preexisting", () -> ++released[0]));
        require(released[0]==0);
        require(ArchivePermissionOwnership.record(store, "import-owned"));
        require(saved.contains("import-owned"));
        // Recreate the store wrapper, as after Activity/process restart.
        ArchivePermissionOwnership.Store restored = new ArchivePermissionOwnership.Store() {
            public java.util.Set<String> read() { return new java.util.HashSet<>(saved); }
            public boolean write(java.util.Set<String> value) { saved.clear(); saved.addAll(value); return true; }
        };
        require(ArchivePermissionOwnership.release(restored, "import-owned", () -> ++released[0]));
        require(released[0]==1 && saved.isEmpty());
        require(ArchivePermissionOwnership.release(restored, "import-owned", () -> ++released[0]));
        require(released[0]==1);
        System.out.println("Chart archive import tests passed");
    }
}
