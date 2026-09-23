package com.aae.androidlib.storage;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

import bin.nt.aae.Aae;

/**
 * SAF / storage bridge. All native-engine filesystem requirements are
 * isolated here: content:// URIs are staged through cache files, and
 * results are copied back out through the ContentResolver.
 *
 * <p>Rules:
 * <ul>
 *   <li>Never convert an arbitrary content:// URI to a filesystem path.
 *   <li>Native input = cache/staging files; native output = cache file
 *       copied to the SAF destination Uri.
 *   <li>Caller owns cleanup via {@link #deleteRecursive(File)} or the
 *       staging-dir helpers.
 * </ul>
 */
public final class SafHelper {

    private SafHelper() {
    }

    /** One staged input ready for {@code Aae.create/add}: archive name + local file. */
    public static final class StagedInput {
        public final String entryName;
        public final File file; // null = explicit directory entry
        public final boolean isDirectory;

        public StagedInput(String entryName, File file, boolean isDirectory) {
            this.entryName = entryName;
            this.file = file;
            this.isDirectory = isDirectory;
        }
    }

    // -- permissions ------------------------------------------------------

    public static void takePersistablePermissions(Context ctx, Intent data) {
        if (data == null || data.getData() == null) {
            return;
        }
        takePersistablePermissions(ctx, data.getData(), data.getFlags());
    }

    public static void takePersistablePermissions(Context ctx, Uri uri, int flags) {
        if (ctx == null || uri == null) {
            return;
        }
        try {
            int take = flags & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            if (take == 0) {
                take = Intent.FLAG_GRANT_READ_URI_PERMISSION;
            }
            ctx.getContentResolver().takePersistableUriPermission(uri, take);
        } catch (Exception ignored) {
            // Not all providers offer persistable permissions; staging still works.
        }
    }

    // -- names ------------------------------------------------------------

    public static String displayName(ContentResolver cr, Uri uri) {
        String name = queryDisplayName(cr, uri);
        if (name != null) {
            return name;
        }
        return "archive-" + System.currentTimeMillis();
    }

    /**
     * Display name of the root of a document tree (ACTION_OPEN_DOCUMENT_TREE).
     *
     * <p>A direct query on the tree Uri returns no DISPLAY_NAME on most
     * providers, which used to fall back to "archive-&lt;millis&gt;" — so a
     * compressed folder lost its real name (MT Manager keeps it, e.g.
     * {@code test-password/}). Resolution order:
     * <ol>
     *   <li>Direct query (works on some providers).</li>
     *   <li>Query the tree root's document Uri (answers on most providers).</li>
     *   <li>Tail of the tree document id, usually "volume:FolderName".</li>
     * </ol>
     */
    public static String treeDisplayName(ContentResolver cr, Uri treeUri) {
        String direct = queryDisplayName(cr, treeUri);
        if (direct != null) {
            return direct;
        }
        try {
            String rootId = DocumentsContract.getTreeDocumentId(treeUri);
            Uri doc = DocumentsContract.buildDocumentUriUsingTree(treeUri, rootId);
            String name = queryDisplayName(cr, doc);
            if (name != null) {
                return name;
            }
            int cut = Math.max(rootId.lastIndexOf(':'), rootId.lastIndexOf('/'));
            String tail = cut >= 0 ? rootId.substring(cut + 1) : rootId;
            if (tail.length() > 0) {
                return tail;
            }
        } catch (Exception ignored) {
        }
        return "folder-" + System.currentTimeMillis();
    }

    private static String queryDisplayName(ContentResolver cr, Uri uri) {
        Cursor c = null;
        try {
            c = cr.query(uri, null, null, null, null);
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) {
                    String name = c.getString(idx);
                    if (name != null && name.length() > 0) {
                        return name;
                    }
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (c != null) {
                try {
                    c.close();
                } catch (Exception ignored) {
                }
            }
        }
        return null;
    }

    /**
     * Last-modified millis of a document, or 0 when the provider does not
     * expose it. Used to stamp staged copies so archives keep real mtimes
     * (MT Manager pattern) instead of copy-time stamps.
     */
    public static long lastModified(ContentResolver cr, Uri uri) {
        Cursor c = null;
        try {
            c = cr.query(uri,
                    new String[]{DocumentsContract.Document.COLUMN_LAST_MODIFIED},
                    null, null, null);
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(DocumentsContract.Document.COLUMN_LAST_MODIFIED);
                if (idx >= 0) {
                    try {
                        long m = c.getLong(idx);
                        return m > 0 ? m : 0;
                    } catch (Exception ignored) {
                    }
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (c != null) {
                try {
                    c.close();
                } catch (Exception ignored) {
                }
            }
        }
        return 0;
    }

    private static void stampMtime(ContentResolver cr, Uri uri, File dst) {
        long m = lastModified(cr, uri);
        if (m > 0) {
            try {
                dst.setLastModified(m);
            } catch (Exception ignored) {
            }
        }
    }

    /**
     * Creates a file inside a tree Uri (for per-file compress mode).
     * Throws IOException when the provider refuses.
     */
    public static Uri createFileInTree(Context ctx, Uri treeUri, String mime, String name)
            throws IOException {
        ContentResolver cr = ctx.getContentResolver();
        String docId = DocumentsContract.getTreeDocumentId(treeUri);
        Uri dirUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
        Uri out;
        try {
            out = DocumentsContract.createDocument(cr, dirUri, mime, name);
        } catch (Exception e) {
            throw new IOException("provider refused file creation: " + name
                    + " (" + e.getMessage() + ")", e);
        }
        if (out == null) {
            throw new IOException("provider refused file creation: " + name);
        }
        return out;
    }

    /** Best-effort SAF delete (for "delete source after compression"). */
    public static boolean deleteDocument(ContentResolver cr, Uri uri) {
        try {
            return DocumentsContract.deleteDocument(cr, uri);
        } catch (Exception ignored) {
            return false;
        }
    }

    /**
     * Makes a URI display name safe as a single archive entry segment.
     * Falls back to fileN when empty/unsafe.
     */
    public static String sanitizeSegment(String raw, String fallback) {
        if (raw == null) {
            return fallback;
        }
        // Strip path separators and backslashes; trim dots/spaces that break zips.
        String s = raw.replace('/', '_').replace('\\', '_').trim();
        while (s.startsWith(".")) {
            s = s.substring(1);
        }
        while (s.endsWith(".") || s.endsWith(" ")) {
            s = s.substring(0, s.length() - 1);
        }
        if (s.length() == 0 || s.equals(".") || s.equals("..")) {
            return fallback;
        }
        if (s.length() > 120) {
            s = s.substring(0, 120);
        }
        return s;
    }

    /** Deduplicates entry names: foo.txt, foo (1).txt, ... */
    public static String deduplicate(String base, Set<String> used) {
        if (!used.contains(base)) {
            used.add(base);
            return base;
        }
        int dot = base.lastIndexOf('.');
        String stem = dot > 0 ? base.substring(0, dot) : base;
        String ext = dot > 0 ? base.substring(dot) : "";
        for (int i = 1; i < 1000; i++) {
            String cand = stem + " (" + i + ")" + ext;
            if (!used.contains(cand)) {
                used.add(cand);
                return cand;
            }
        }
        String cand = stem + " (" + System.currentTimeMillis() + ")" + ext;
        used.add(cand);
        return cand;
    }

    // -- copy -------------------------------------------------------------

    public static void copyUriToFile(ContentResolver cr, Uri uri, File dst) throws IOException {
        File parent = dst.getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }
        InputStream in = null;
        OutputStream out = null;
        try {
            in = cr.openInputStream(uri);
            if (in == null) {
                throw new IOException("cannot open for read: " + uri);
            }
            out = new FileOutputStream(dst);
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) >= 0) {
                out.write(buf, 0, n);
            }
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (Exception ignored) {
                }
            }
            if (out != null) {
                try {
                    out.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    public static void copyFileToUri(ContentResolver cr, File src, Uri dst) throws IOException {
        OutputStream out = null;
        java.io.FileInputStream in = null;
        try {
            in = new java.io.FileInputStream(src);
            out = cr.openOutputStream(dst, "w");
            if (out == null) {
                throw new IOException("cannot open for write: " + dst);
            }
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) >= 0) {
                out.write(buf, 0, n);
            }
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (Exception ignored) {
                }
            }
            if (out != null) {
                try {
                    out.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    // -- staging dirs -----------------------------------------------------

    public static File freshDir(File parent, String name) {
        File dir = new File(parent, name + "-" + System.currentTimeMillis());
        deleteRecursive(dir);
        dir.mkdirs();
        return dir;
    }

    public static File inboxDir(Context ctx) {
        File d = new File(ctx.getCacheDir(), "inbox");
        d.mkdirs();
        return d;
    }

    public static File stagingDir(Context ctx) {
        File d = new File(ctx.getCacheDir(), "staging");
        d.mkdirs();
        return d;
    }

    // -- multi-file input (ACTION_OPEN_DOCUMENT + EXTRA_ALLOW_MULTIPLE) ---

    /**
     * Stages multiple SAF documents into cache files and returns entry names.
     * Directories picked here are traversed one level via the tree API when
     * possible; otherwise they are skipped with the name recorded in
     * {@code skipped}.
     */
    public static List<StagedInput> stageMultipleFiles(Context ctx, List<Uri> uris,
            File stagingRoot, List<String> skipped) throws IOException {
        List<StagedInput> out = new ArrayList<StagedInput>();
        Set<String> used = new HashSet<String>();
        ContentResolver cr = ctx.getContentResolver();
        int fallback = 0;
        for (Uri uri : uris) {
            if (uri == null) {
                continue;
            }
            String raw = displayName(cr, uri);
            String mime = null;
            try {
                mime = cr.getType(uri);
            } catch (Exception ignored) {
            }
            boolean isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
            if (isDir) {
                // Recurse into the directory, preserving its name as prefix.
                String prefix = sanitizeSegment(raw, "folder-" + (++fallback));
                prefix = deduplicateDir(prefix, used);
                try {
                    collectTreeInto(ctx, uri, prefix, stagingRoot, out, used, skipped);
                } catch (Exception e) {
                    skipped.add(raw + ": " + e.getMessage());
                }
                continue;
            }
            String entry = sanitizeSegment(raw, "file-" + (++fallback));
            entry = deduplicate(entry, used);
            try {
                Aae.checkEntryName(entry);
            } catch (Exception e) {
                skipped.add(raw + ": unsafe name");
                continue;
            }
            File dst = new File(stagingRoot, "f-" + (fallback) + "-" + entry.replace('/', '_'));
            try {
                copyUriToFile(cr, uri, dst);
                stampMtime(cr, uri, dst);
            } catch (Exception e) {
                skipped.add(raw + ": unreadable (" + e.getMessage() + ")");
                continue;
            }
            out.add(new StagedInput(entry, dst, false));
        }
        return out;
    }

    private static String deduplicateDir(String base, Set<String> used) {
        if (!used.contains(base) && !used.contains(base + "/")) {
            used.add(base);
            return base;
        }
        for (int i = 1; i < 1000; i++) {
            String cand = base + " (" + i + ")";
            if (!used.contains(cand)) {
                used.add(cand);
                return cand;
            }
        }
        return base + " (" + System.currentTimeMillis() + ")";
    }

    // -- folder input (ACTION_OPEN_DOCUMENT_TREE) --------------------------

    /**
     * Recursively stages a SAF tree into cache files, preserving relative
     * structure under {@code entryPrefix}. Returns staged inputs (files and
     * explicit directory entries). Inaccessible documents are recorded in
     * {@code skipped}, never thrown.
     */
    public static List<StagedInput> stageTree(Context ctx, Uri treeUri, String entryPrefix,
            File stagingRoot, List<String> skipped) throws IOException {
        List<StagedInput> out = new ArrayList<StagedInput>();
        Set<String> used = new HashSet<String>();
        String rootId = DocumentsContract.getTreeDocumentId(treeUri);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, rootId);
        // Root dir entry itself (so empty folders survive).
        if (entryPrefix != null && entryPrefix.length() > 0) {
            out.add(new StagedInput(entryPrefix, null, true));
            used.add(entryPrefix);
        }
        walkTree(ctx, children, entryPrefix == null ? "" : entryPrefix,
                stagingRoot, out, used, skipped, 0);
        return out;
    }

    private static void collectTreeInto(Context ctx, Uri treeUri, String prefix, File stagingRoot,
            List<StagedInput> out, Set<String> used, List<String> skipped) throws IOException {
        List<String> localSkipped = new ArrayList<String>();
        // treeUri here may be a plain document Uri for a directory; build children via doc id.
        String docId;
        try {
            docId = DocumentsContract.getDocumentId(treeUri);
        } catch (Exception e) {
            try {
                docId = DocumentsContract.getTreeDocumentId(treeUri);
            } catch (Exception e2) {
                skipped.add(prefix + ": cannot list directory");
                return;
            }
        }
        Uri children;
        try {
            children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, docId);
        } catch (Exception e) {
            skipped.add(prefix + ": cannot list directory");
            return;
        }
        out.add(new StagedInput(prefix, null, true));
        used.add(prefix);
        walkTree(ctx, children, prefix, stagingRoot, out, used, localSkipped, 0);
        skipped.addAll(localSkipped);
    }

    private static void walkTree(Context ctx, Uri childrenUri, String prefix, File stagingRoot,
            List<StagedInput> out, Set<String> used, List<String> skipped, int depth)
            throws IOException {
        if (depth > 32) {
            skipped.add(prefix + ": directory too deep, skipped");
            return;
        }
        ContentResolver cr = ctx.getContentResolver();
        Cursor c = null;
        try {
            c = cr.query(childrenUri, new String[]{
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null);
        } catch (Exception e) {
            skipped.add(prefix + ": cannot list (" + e.getMessage() + ")");
            return;
        }
        if (c == null) {
            skipped.add(prefix + ": provider returned no listing");
            return;
        }
        try {
            int idCol = c.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID);
            int nameCol = c.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME);
            int mimeCol = c.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE);
            while (c.moveToNext()) {
                String docId;
                String rawName;
                String mime;
                try {
                    docId = c.getString(idCol);
                    rawName = c.getString(nameCol);
                    mime = c.getString(mimeCol);
                } catch (Exception e) {
                    skipped.add(prefix + ": bad row, skipped");
                    continue;
                }
                boolean isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
                String seg = sanitizeSegment(rawName, isDir ? "folder" : "file");
                String rel = prefix.length() == 0 ? seg : prefix + "/" + seg;
                // Deduplicate full relative path.
                if (used.contains(rel)) {
                    int dot = seg.lastIndexOf('.');
                    String stem = dot > 0 && !isDir ? seg.substring(0, dot) : seg;
                    String ext = dot > 0 && !isDir ? seg.substring(dot) : "";
                    boolean placed = false;
                    for (int i = 1; i < 100; i++) {
                        String candSeg = stem + " (" + i + ")" + ext;
                        String cand = prefix.length() == 0 ? candSeg : prefix + "/" + candSeg;
                        if (!used.contains(cand)) {
                            rel = cand;
                            seg = candSeg;
                            placed = true;
                            break;
                        }
                    }
                    if (!placed) {
                        skipped.add(rawName + ": duplicate name, skipped");
                        continue;
                    }
                }
                used.add(rel);
                Uri docUri;
                try {
                    docUri = DocumentsContract.buildDocumentUriUsingTree(childrenUri, docId);
                } catch (Exception e) {
                    skipped.add(rawName + ": cannot address, skipped");
                    continue;
                }
                if (isDir) {
                    out.add(new StagedInput(rel, null, true));
                    Uri subChildren;
                    try {
                        subChildren = DocumentsContract.buildChildDocumentsUriUsingTree(
                                childrenUri, docId);
                    } catch (Exception e) {
                        skipped.add(rel + ": cannot list, skipped");
                        continue;
                    }
                    walkTree(ctx, subChildren, rel, stagingRoot, out, used, skipped, depth + 1);
                } else {
                    try {
                        Aae.checkEntryName(rel);
                    } catch (Exception e) {
                        skipped.add(rawName + ": unsafe name, skipped");
                        continue;
                    }
                    File dst = new File(stagingRoot,
                            "t-" + System.currentTimeMillis() + "-" + Math.abs(rel.hashCode()));
                    try {
                        copyUriToFile(cr, docUri, dst);
                        stampMtime(cr, docUri, dst);
                    } catch (Exception e) {
                        skipped.add(rel + ": unreadable (" + e.getMessage() + ")");
                        continue;
                    }
                    out.add(new StagedInput(rel, dst, false));
                }
            }
        } finally {
            try {
                c.close();
            } catch (Exception ignored) {
            }
        }
    }

    // -- extraction to SAF tree --------------------------------------------

    /**
     * Copies an extracted staging directory into a SAF tree Uri, creating
     * files via the tree API. Returns count of files copied; failures are
     * collected into {@code errors} instead of aborting the whole copy.
     */
    public static int copyDirToTree(Context ctx, File stagingDir, Uri treeUri,
            List<String> errors) {
        File[] kids = stagingDir.listFiles();
        if (kids == null) {
            return 0;
        }
        int count = 0;
        for (File kid : kids) {
            try {
                count += copyOneToTree(ctx, kid, treeUri, errors);
            } catch (Exception e) {
                errors.add(kid.getName() + ": " + e.getMessage());
            }
        }
        return count;
    }

    private static int copyOneToTree(Context ctx, File src, Uri treeUri, List<String> errors)
            throws IOException {
        ContentResolver cr = ctx.getContentResolver();
        String docId = DocumentsContract.getTreeDocumentId(treeUri);
        Uri dirUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
        return copyOneToDirUri(ctx, cr, src, dirUri, treeUri, errors);
    }

    private static int copyOneToDirUri(Context ctx, ContentResolver cr, File src, Uri dirUri,
            Uri treeUri, List<String> errors) throws IOException {
        if (src.isDirectory()) {
            Uri subDir = createDir(cr, dirUri, src.getName());
            if (subDir == null) {
                errors.add(src.getName() + ": cannot create directory on provider");
                return 0;
            }
            int n = 0;
            File[] kids = src.listFiles();
            if (kids != null) {
                for (File k : kids) {
                    try {
                        n += copyOneToDirUri(ctx, cr, k, subDir, treeUri, errors);
                    } catch (Exception e) {
                        errors.add(k.getName() + ": " + e.getMessage());
                    }
                }
            }
            return n;
        }
        String mime = mimeFor(src.getName());
        Uri fileUri = createFile(cr, dirUri, mime, src.getName());
        if (fileUri == null) {
            // Provider may reject create; try to find an existing entry and overwrite.
            errors.add(src.getName() + ": provider refused file creation");
            return 0;
        }
        copyFileToUri(cr, src, fileUri);
        return 1;
    }

    private static Uri createDir(ContentResolver cr, Uri parent, String name) {
        try {
            return DocumentsContract.createDocument(cr, parent,
                    DocumentsContract.Document.MIME_TYPE_DIR, name);
        } catch (Exception e) {
            return null;
        }
    }

    private static Uri createFile(ContentResolver cr, Uri parent, String mime, String name) {
        try {
            return DocumentsContract.createDocument(cr, parent, mime, name);
        } catch (Exception e) {
            return null;
        }
    }

    public static String mimeFor(String name) {
        String n = name.toLowerCase();
        if (n.endsWith(".txt") || n.endsWith(".log") || n.endsWith(".md")) {
            return "text/plain";
        }
        if (n.endsWith(".zip")) {
            return "application/zip";
        }
        if (n.endsWith(".json")) {
            return "application/json";
        }
        if (n.endsWith(".png")) {
            return "image/png";
        }
        if (n.endsWith(".jpg") || n.endsWith(".jpeg")) {
            return "image/jpeg";
        }
        return "application/octet-stream";
    }

    // -- misc ---------------------------------------------------------------

    public static void deleteRecursive(File f) {
        if (f == null || !f.exists()) {
            return;
        }
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) {
                for (File k : kids) {
                    deleteRecursive(k);
                }
            }
        }
        try {
            f.delete();
        } catch (Exception ignored) {
        }
    }

    public static String namesOf(File dir, int max) {
        File[] kids = dir.listFiles();
        if (kids == null || kids.length == 0) {
            return "(empty)";
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < kids.length && i < max; i++) {
            if (i > 0) {
                sb.append(", ");
            }
            sb.append(kids[i].getName());
        }
        if (kids.length > max) {
            sb.append(", ...");
        }
        return sb.toString();
    }
}
