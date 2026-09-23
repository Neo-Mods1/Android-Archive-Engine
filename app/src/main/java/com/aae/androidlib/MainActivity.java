package com.aae.androidlib;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.content.Context;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.text.method.HideReturnsTransformationMethod;
import android.text.method.PasswordTransformationMethod;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.List;

import bin.nt.aae.Aae;
import bin.nt.aae.AaeArchiveInfo;
import bin.nt.aae.AaeCapabilities;
import bin.nt.aae.AaeEncryption;
import bin.nt.aae.AaeEntry;
import bin.nt.aae.AaeException;
import bin.nt.aae.AaeFormat;
import bin.nt.aae.AaeInput;
import bin.nt.aae.AaeMethod;
import bin.nt.aae.AaeProgressListener;
import bin.nt.aae.AaeSession;
import bin.nt.aae.AaeWriteOptions;
import bin.nt.aae.NativeAaeSession;
import com.aae.androidlib.archive.AaeRunner;
import com.aae.androidlib.storage.SafHelper;

/**
 * Two-tab archive utility.
 *
 * <p>Tab 1 (Testing) validates the engine: pick/open/probe/list/read/
 * extract, password handling, format info, round-trip matrix, diagnostics.
 * Tab 2 (Compression) picks SAF inputs + outputs and configures the
 * archive through an inline MT-style block (filename, format, level,
 * password, split, switches); the confirmed settings run on the worker and
 * support every writable format (zip, tar, layered tar, single-file
 * streams), per-file mode, atomic publish through SAF, and optional
 * delete-afterwards.
 *
 * <p>Layers: this Activity is UI only. Archive work runs on
 * {@link AaeRunner}'s single worker thread; SAF bridging lives in
 * {@link SafHelper}; capability truth lives in {@link AaeCapabilities};
 * JNI details stay in {@code bin.nt.aae.NativeAaeSession}. The UI never
 * touches libzip/sevenzip concepts directly.
 */
public class MainActivity extends Activity {

    private static final int REQ_PICK_ARCHIVE = 1001;
    private static final int REQ_EXTRACT_TREE = 1002;
    private static final int REQ_COMPRESS_FILES = 1003;
    private static final int REQ_COMPRESS_FOLDER = 1004;
    private static final int REQ_COMPRESS_OUTPUT = 1005;
    private static final int REQ_COMPRESS_OUTPUT_TREE = 1006;

    // -- shared ------------------------------------------------------------
    private TextView tvEngine;

    // -- test tab ----------------------------------------------------------
    private View tabTest;
    private TextView tvFile;
    private TextView tvLog;
    private TextView tvTestStatus;
    private TextView tvExtractDir;
    private EditText etPassword;
    private ProgressBar pbTest;
    private Button btnProbe;
    private Button btnList;
    private Button btnExtract;
    private Button btnRead;
    private Button btnTypes;
    private Button btnCancelOp;
    private final StringBuilder logBuf = new StringBuilder();
    private File currentFile;
    private String currentName = "";
    private Uri extractTreeUri;
    private AaeRunner.OpHandle currentOp;

    // -- compress tab ------------------------------------------------------
    // The tab picks inputs + outputs and configures the archive through an
    // inline MT-style block (filename, format, level, password, split,
    // switches); the collected Config runs through runCompressJob below.
    private View tabCompress;
    private TextView tvInputSummary;
    private TextView tvOutputSummary;
    private TextView tvCompressStatus;
    private TextView tvCompressLog;
    private ProgressBar pbCompress;
    private Button btnCompress;
    private Button btnCancelCompress;
    private final StringBuilder compressLogBuf = new StringBuilder();

    private final List<Uri> compressFileUris = new ArrayList<Uri>();
    private final List<Uri> compressFolderUris = new ArrayList<Uri>();
    private final List<String> compressFolderNames = new ArrayList<String>();
    private Uri compressOutputUri;
    private Uri compressOutputTreeUri;
    private CompressConfig pendingConfig;
    private AaeRunner.OpHandle compressOp;

    // -- inline MT-style create block (Compress tab, not a dialog) --------
    /** Validated archive settings collected from the tab's MT-style block. */
    private static final class CompressConfig {
        String fileName;
        AaeCapabilities.FormatCaps format;
        AaeMethod zipMethod = AaeMethod.DEFLATE;
        String mtLevel = AaeCapabilities.MT_NORMAL;
        String password; // null when empty
        boolean perFile;
        boolean deleteSource;
    }

    private static final class FormatRow {
        final AaeCapabilities.FormatCaps caps;
        final String label;
        final boolean enabled;
        final String why;

        FormatRow(AaeCapabilities.FormatCaps caps, String label, boolean enabled,
                String why) {
            this.caps = caps;
            this.label = label;
            this.enabled = enabled;
            this.why = why;
        }

        @Override
        public String toString() {
            return label;
        }
    }

    private static final class FormatAdapter extends ArrayAdapter<FormatRow> {
        FormatAdapter(Context context, List<FormatRow> rows) {
            super(context, android.R.layout.simple_spinner_item, rows);
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        }

        @Override
        public boolean isEnabled(int position) {
            FormatRow r = getItem(position);
            return r == null || r.enabled;
        }

        @Override
        public View getDropDownView(int position, View convertView, ViewGroup parent) {
            TextView tv = (TextView) super.getDropDownView(position, convertView, parent);
            FormatRow r = getItem(position);
            if (r != null && !r.enabled) {
                tv.setTextColor(0xFF999999);
            }
            return tv;
        }
    }

    private static final String[] SPLIT_OPTIONS =
            {"None", "10 MB", "25 MB", "50 MB", "100 MB", "Custom..."};
    private static final String[] ZIP_METHODS = {"Deflate", "BZip2", "XZ (LZMA)", "ZSTD"};
    private static final AaeMethod[] ZIP_METHOD_VALUES =
            {AaeMethod.DEFLATE, AaeMethod.BZIP2, AaeMethod.XZ, AaeMethod.ZSTD};

    private final List<FormatRow> formatRows = new ArrayList<FormatRow>();
    private EditText etFileName;
    private Spinner spFormat;
    private Spinner spLevel;
    private Spinner spMethod;
    private View methodRow;
    private EditText etCompressPassword;
    private Spinner spSplit;
    private EditText etSplitMb;
    private Switch swPerFile;
    private Switch swDeleteSource;
    private TextView tvCompressNote;
    private TextView tvCompressError;
    private String currentExt = "zip";
    private boolean showingPassword;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvEngine = (TextView) findViewById(R.id.tvEngine);
        tabTest = findViewById(R.id.tabTest);
        tabCompress = findViewById(R.id.tabCompress);

        Button btnTabTest = (Button) findViewById(R.id.btnTabTest);
        Button btnTabCompress = (Button) findViewById(R.id.btnTabCompress);
        btnTabTest.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showTab(true);
            }
        });
        btnTabCompress.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showTab(false);
            }
        });

        setupTestTab();
        setupCompressTab();
        showTab(true);

        AaeRunner.submit(new AaeRunner.Job() {
            @Override
            public void run(AaeRunner.OpHandle op) {
                final String version = Aae.version();
                final boolean avail = Aae.isAvailable();
                final String formats = Aae.supportedFormats().toString();
                AaeRunner.postUi(new Runnable() {
                    @Override
                    public void run() {
                        tvEngine.setText("engine: " + version);
                        log("engine: " + version);
                        log("native: " + avail + " formats=" + formats);
                    }
                });
            }
        }, null);
    }

    private void showTab(boolean test) {
        tabTest.setVisibility(test ? View.VISIBLE : View.GONE);
        tabCompress.setVisibility(test ? View.GONE : View.VISIBLE);
    }

    // ================= TEST TAB =================

    private void setupTestTab() {
        tvFile = (TextView) findViewById(R.id.tvFile);
        tvLog = (TextView) findViewById(R.id.tvLog);
        tvTestStatus = (TextView) findViewById(R.id.tvTestStatus);
        tvExtractDir = (TextView) findViewById(R.id.tvExtractDir);
        etPassword = (EditText) findViewById(R.id.etPassword);
        pbTest = (ProgressBar) findViewById(R.id.pbTest);
        btnProbe = (Button) findViewById(R.id.btnProbe);
        btnList = (Button) findViewById(R.id.btnList);
        btnExtract = (Button) findViewById(R.id.btnExtract);
        btnRead = (Button) findViewById(R.id.btnRead);
        btnTypes = (Button) findViewById(R.id.btnTypes);
        btnCancelOp = (Button) findViewById(R.id.btnCancelOp);

        setTestButtonsEnabled(false);

        ((Button) findViewById(R.id.btnPick)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                        i.addCategory(Intent.CATEGORY_OPENABLE);
                        i.setType("*/*");
                        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                        startActivityForResult(i, REQ_PICK_ARCHIVE);
                    }
                });
        testBind(R.id.btnProbe, new Runnable() {
            @Override
            public void run() {
                doProbe();
            }
        });
        testBind(R.id.btnList, new Runnable() {
            @Override
            public void run() {
                doList();
            }
        });
        testBind(R.id.btnExtract, new Runnable() {
            @Override
            public void run() {
                doExtract();
            }
        });
        testBind(R.id.btnRead, new Runnable() {
            @Override
            public void run() {
                doRead();
            }
        });
        testBind(R.id.btnTypes, new Runnable() {
            @Override
            public void run() {
                doTypeTests();
            }
        });
        ((Button) findViewById(R.id.btnFormats)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        doFormats();
                    }
                });
        ((Button) findViewById(R.id.btnExtractDir)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                        startActivityForResult(i, REQ_EXTRACT_TREE);
                    }
                });
        btnCancelOp.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (currentOp != null) {
                    currentOp.cancel();
                }
            }
        });
        ((Button) findViewById(R.id.btnClear)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        logBuf.setLength(0);
                        tvLog.setText("");
                        setTestStatus("");
                    }
                });
    }

    private void setTestButtonsEnabled(boolean enabled) {
        btnProbe.setEnabled(enabled);
        btnList.setEnabled(enabled);
        btnExtract.setEnabled(enabled);
        btnRead.setEnabled(enabled);
    }

    private void testBind(int id, final Runnable task) {
        ((Button) findViewById(id)).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startTestOp(task);
            }
        });
    }

    private void startTestOp(final Runnable task) {
        setTestBusy(true);
        currentOp = AaeRunner.submit(new AaeRunner.Job() {
            @Override
            public void run(AaeRunner.OpHandle op) {
                task.run();
            }
        }, new AaeRunner.Done() {
            @Override
            public void onDone(AaeRunner.OpHandle op) {
                setTestBusy(false);
            }
        });
        // Forward cancellation into progress listeners via currentOp.
    }

    private void setTestBusy(final boolean busy) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                btnCancelOp.setEnabled(busy);
                pbTest.setVisibility(busy ? View.VISIBLE : View.GONE);
                if (busy) {
                    pbTest.setProgress(0);
                }
            }
        });
    }

    private void setTestProgress(final int pct) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (pbTest.getVisibility() == View.VISIBLE) {
                    pbTest.setProgress(pct);
                }
            }
        });
    }

    private String testPassword() {
        String p = etPassword.getText().toString();
        return p.length() == 0 ? null : p;
    }

    private boolean needFile() {
        if (currentFile == null) {
            setTestStatus("Select an archive first.");
            log("select an archive first");
            return false;
        }
        return true;
    }

    private void doProbe() {
        if (!needFile()) {
            return;
        }
        try {
            AaeFormat f = Aae.probe(currentFile);
            AaeArchiveInfo info = Aae.info(currentFile, testPassword());
            final String msg = "probe: " + f + " entries=" + info.entryCount
                    + " size=" + info.totalSize + " packed=" + info.totalPackedSize
                    + " encrypted=" + info.hasEncryptedEntries;
            log(msg);
            log("  comment=[" + info.comment + "]");
            setTestStatus("");
        } catch (AaeException e) {
            fail("probe/info failed: " + describe(e), e);
        }
    }

    private void doList() {
        if (!needFile()) {
            return;
        }
        try {
            List<AaeEntry> entries = Aae.listEntries(currentFile, testPassword());
            AaeFormat fmt = Aae.probe(currentFile);
            log("list: " + entries.size() + " entries [" + fmt + "]");
            int shown = 0;
            for (AaeEntry e : entries) {
                if (shown >= 50) {
                    log("  ... (" + (entries.size() - shown) + " more)");
                    break;
                }
                String how = e.method == -1 ? fmt.toString().toLowerCase()
                        : methodName(e.method);
                log("  " + (e.isDirectory ? "d " : "f ")
                        + e.path + " sz=" + e.size
                        + " method=" + how
                        + (e.encrypted ? " ENCRYPTED" : ""));
                shown++;
            }
            setTestStatus("");
        } catch (AaeException e) {
            fail("list failed: " + describe(e), e);
        }
    }

    private void doExtract() {
        if (!needFile()) {
            return;
        }
        try {
            final long t0 = System.currentTimeMillis();
            final File previewRoot = new File(getCacheDir(), "extract");
            final File out = new File(previewRoot,
                    safeFileName(stripExt(currentName.isEmpty() ? "archive" : currentName)));
            SafHelper.deleteRecursive(out);
            out.mkdirs();
            Aae.extractAll(currentFile, out, testPassword(), progressBridge());
            if (extractTreeUri != null) {
                List<String> errors = new ArrayList<String>();
                int n = SafHelper.copyDirToTree(this, out, extractTreeUri, errors);
                log("extract OK (" + n + " files) in "
                        + (System.currentTimeMillis() - t0) + "ms -> SAF folder");
                for (String err : errors) {
                    log("  copy WARN: " + err);
                }
                if (!errors.isEmpty()) {
                    setTestStatus("Extracted with " + errors.size() + " copy warnings.");
                } else {
                    setTestStatus("");
                }
            } else {
                log("extract OK -> cache preview in "
                        + (System.currentTimeMillis() - t0) + "ms");
                log("  top-level: " + SafHelper.namesOf(out, 8));
                log("  (pick an Output folder to copy extracts to SAF)");
                setTestStatus("");
            }
        } catch (final AaeException e) {
            if (e.kind == AaeException.Kind.CANCELLED) {
                setTestStatus("Cancelled.");
                log("extract cancelled");
            } else {
                fail("extract failed: " + describe(e), e);
            }
        }
    }

    private AaeProgressListener progressBridge() {
        return new AaeProgressListener() {
            private int lastPct = -1;

            @Override
            public boolean onProgress(int fi, int fc, long done, long total) {
                if (currentOp != null && currentOp.isCancelled()) {
                    return false;
                }
                if (total > 0) {
                    int pct = (int) (done * 100 / total);
                    if (pct != lastPct && pct % 5 == 0) {
                        lastPct = pct;
                        setTestProgress(pct);
                    }
                }
                return true;
            }
        };
    }

    private void doRead() {
        if (!needFile()) {
            return;
        }
        try {
            List<AaeEntry> entries = Aae.listEntries(currentFile, testPassword());
            AaeEntry text = null;
            for (AaeEntry e : entries) {
                if (!e.isDirectory && e.path.endsWith(".txt") && e.size > 0
                        && e.size < 65536) {
                    text = e;
                    break;
                }
            }
            if (text == null) {
                for (AaeEntry e : entries) {
                    if (!e.isDirectory && e.size > 0 && e.size < 65536) {
                        text = e;
                        break;
                    }
                }
            }
            if (text == null) {
                setTestStatus("No small file entry found.");
                log("read: no small file entry found");
                return;
            }
            byte[] data = Aae.readBytes(currentFile, text.path, 65536, testPassword());
            String preview;
            try {
                preview = new String(data, "UTF-8");
            } catch (Exception e) {
                preview = "(binary, " + data.length + "b)";
            }
            if (preview.length() > 300) {
                preview = preview.substring(0, 300) + "...";
            }
            log("read [" + text.path + "] " + data.length + "b:");
            log("  " + preview.replace("\n", " | "));
            setTestStatus("");
        } catch (AaeException e) {
            fail("read failed: " + describe(e), e);
        }
    }

    private void doFormats() {
        log("supported (native): " + Aae.supportedFormats());
        for (AaeCapabilities.FormatCaps fc : AaeCapabilities.readableFormats()) {
            StringBuilder sb = new StringBuilder();
            sb.append("  ").append(fc.displayName).append(" (.").append(fc.extension).append(")");
            if (fc.writable) {
                sb.append(" read/write methods=[");
                for (int i = 0; i < fc.methods.size(); i++) {
                    if (i > 0) {
                        sb.append(",");
                    }
                    sb.append(fc.methods.get(i).method);
                }
                sb.append("] encrypt=").append(fc.encryptionSupported);
            } else {
                sb.append(" read-only");
            }
            sb.append(" — ").append(fc.note);
            log(sb.toString());
        }
        setTestStatus("");
    }

    // -- per-type round-trip matrix (capability-driven, never offers the impossible) --

    private void doTypeTests() {
        log("== type tests ==");
        testOneType("STORE", AaeMethod.STORE, 0, AaeEncryption.NONE, null);
        testOneType("DEFLATE-6", AaeMethod.DEFLATE, 6, AaeEncryption.NONE, null);
        testOneType("BZIP2", AaeMethod.BZIP2, 0, AaeEncryption.NONE, null);
        testOneType("XZ-6", AaeMethod.XZ, 6, AaeEncryption.NONE, null);
        testOneType("ZSTD", AaeMethod.ZSTD, 0, AaeEncryption.NONE, null);
        testOneType("AES-256", AaeMethod.DEFLATE, 6, AaeEncryption.AES_256, "test123");
        // Negative tests: these MUST fail validation, proving the UI can never
        // send them (invalid method/level combos are rejected before JNI).
        testInvalidCombo();
        log("== type tests done ==");
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                setTestStatus("");
            }
        });
    }

    private void testOneType(String label, AaeMethod method, int level, AaeEncryption enc,
            String password) {
        File dir = new File(getCacheDir(), "typetest/" + label.replace(' ', '_'));
        SafHelper.deleteRecursive(dir);
        dir.mkdirs();
        try {
            String payload = "aae roundtrip " + label + " - 0123456789 abcdef\n";
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 200; i++) {
                sb.append(payload);
            }
            final String text = sb.toString();
            File src = new File(dir, "hello.txt");
            writeString(src, text);

            List<AaeInput> inputs = new ArrayList<AaeInput>();
            inputs.add(new AaeInput("docs/hello.txt", src));
            File zip = new File(dir, "t.zip");
            AaeWriteOptions opts;
            try {
                opts = new AaeWriteOptions(method, level, password, enc);
            } catch (IllegalArgumentException e) {
                log("[" + label + "] SKIP (invalid per capabilities): " + e.getMessage());
                return;
            }
            Aae.create(zip, inputs, opts, AaeFormat.ZIP);

            List<AaeEntry> entries = Aae.listEntries(zip, password);
            if (entries.size() != 2) { // docs/ + docs/hello.txt
                log("[" + label + "] FAIL: expected 2 entries, got " + entries.size());
                return;
            }
            byte[] back = Aae.readBytes(zip, "docs/hello.txt", 1 << 20, password);
            String roundtrip;
            try {
                roundtrip = new String(back, "UTF-8");
            } catch (Exception e) {
                log("[" + label + "] FAIL: decode: " + e);
                return;
            }
            if (!text.equals(roundtrip)) {
                log("[" + label + "] FAIL: content mismatch");
                return;
            }
            File outDir = new File(dir, "out");
            Aae.extractAll(zip, outDir, password, null);
            String fromDisk = readString(new File(outDir, "docs/hello.txt"));
            if (!text.equals(fromDisk)) {
                log("[" + label + "] FAIL: extracted file mismatch");
                return;
            }
            // Wrong-password check for encrypted case.
            if (enc != AaeEncryption.NONE) {
                try {
                    Aae.readBytes(zip, "docs/hello.txt", 1 << 20, "wrong-password");
                    log("[" + label + "] FAIL: wrong password unexpectedly succeeded");
                    return;
                } catch (AaeException e) {
                    if (e.kind != AaeException.Kind.PASSWORD_WRONG
                            && e.kind != AaeException.Kind.PASSWORD_REQUIRED) {
                        log("[" + label + "] FAIL: wrong password gave " + describe(e));
                        return;
                    }
                }
            }
            log("[" + label + "] PASS (" + zip.length() + "b)");
        } catch (AaeException e) {
            if (e.kind == AaeException.Kind.UNSUPPORTED_OPERATION
                    || e.kind == AaeException.Kind.UNSUPPORTED_FORMAT) {
                log("[" + label + "] SKIP: " + e.getMessage());
            } else {
                log("[" + label + "] FAIL: " + describe(e));
            }
        } catch (Exception e) {
            log("[" + label + "] FAIL: " + e);
        }
    }

    private void testInvalidCombo() {
        // STORE with a level must be rejected by AaeWriteOptions itself.
        try {
            new AaeWriteOptions(AaeMethod.STORE, 5, null, AaeEncryption.NONE);
            log("[invalid STORE/5] FAIL: unexpectedly accepted");
        } catch (IllegalArgumentException e) {
            log("[invalid STORE/5] PASS (rejected): " + e.getMessage());
        }
        // ZSTD level 99 must be rejected.
        try {
            new AaeWriteOptions(AaeMethod.ZSTD, 99, null, AaeEncryption.NONE);
            log("[invalid ZSTD/99] FAIL: unexpectedly accepted");
        } catch (IllegalArgumentException e) {
            log("[invalid ZSTD/99] PASS (rejected): " + e.getMessage());
        }
        // TAR accepts STORE/0 but nothing else.
        try {
            AaeCapabilities.validateWrite(AaeFormat.TAR,
                    new AaeWriteOptions(AaeMethod.STORE, 0, null, AaeEncryption.NONE));
            log("[tar STORE/0] PASS (accepted)");
        } catch (AaeException e) {
            log("[tar STORE/0] FAIL: unexpectedly rejected: " + describe(e));
        }
        try {
            AaeCapabilities.validateWrite(AaeFormat.TAR,
                    new AaeWriteOptions(AaeMethod.DEFLATE, 6, null, AaeEncryption.NONE));
            log("[invalid TAR DEFLATE] FAIL: unexpectedly accepted");
        } catch (AaeException e) {
            log("[invalid TAR DEFLATE] PASS (rejected): " + e.getMessage());
        }
        // Stream formats have no password concept.
        try {
            AaeCapabilities.validateWrite(AaeFormat.GZ,
                    new AaeWriteOptions(AaeMethod.DEFLATE, 6, "secret", AaeEncryption.NONE));
            log("[invalid GZ password] FAIL: unexpectedly accepted");
        } catch (AaeException e) {
            log("[invalid GZ password] PASS (rejected): " + e.getMessage());
        }
        // MT level mapping spot-checks.
        try {
            AaeWriteOptions o = AaeCapabilities.mtOptions(
                    AaeFormat.ZIP, AaeMethod.DEFLATE, AaeCapabilities.MT_NORMAL, null);
            log("[mt ZIP Normal] " + (o.method == AaeMethod.DEFLATE && o.level == 6
                    ? "PASS" : "FAIL: " + o.method + "/" + o.level));
            AaeWriteOptions t = AaeCapabilities.mtOptions(
                    AaeFormat.TAR_GZ, null, AaeCapabilities.MT_ULTRA, null);
            log("[mt TAR_GZ Ultra] " + (t.method == AaeMethod.DEFLATE && t.level == 9
                    ? "PASS" : "FAIL: " + t.method + "/" + t.level));
            AaeWriteOptions z = AaeCapabilities.mtOptions(
                    AaeFormat.ZIP, null, AaeCapabilities.MT_APK, null);
            log("[mt ZIP APK] " + (z.method == AaeMethod.STORE && z.level == 0
                    ? "PASS" : "FAIL: " + z.method + "/" + z.level));
        } catch (AaeException e) {
            log("[mt mapping] FAIL: " + describe(e));
        }
    }

    // ================= COMPRESS TAB =================

    private void setupCompressTab() {
        tvInputSummary = (TextView) findViewById(R.id.tvInputSummary);
        tvOutputSummary = (TextView) findViewById(R.id.tvOutputSummary);
        tvCompressStatus = (TextView) findViewById(R.id.tvCompressStatus);
        tvCompressLog = (TextView) findViewById(R.id.tvCompressLog);
        pbCompress = (ProgressBar) findViewById(R.id.pbCompress);
        btnCompress = (Button) findViewById(R.id.btnCompress);
        btnCancelCompress = (Button) findViewById(R.id.btnCancelCompress);
        etFileName = (EditText) findViewById(R.id.etFileName);
        spFormat = (Spinner) findViewById(R.id.spFormat);
        spLevel = (Spinner) findViewById(R.id.spLevel);
        spMethod = (Spinner) findViewById(R.id.spMethod);
        methodRow = findViewById(R.id.methodRow);
        etCompressPassword = (EditText) findViewById(R.id.etCompressPassword);
        spSplit = (Spinner) findViewById(R.id.spSplit);
        etSplitMb = (EditText) findViewById(R.id.etSplitMb);
        swPerFile = (Switch) findViewById(R.id.swPerFile);
        swDeleteSource = (Switch) findViewById(R.id.swDeleteSource);
        tvCompressNote = (TextView) findViewById(R.id.tvCompressNote);
        tvCompressError = (TextView) findViewById(R.id.tvCompressError);
        watchFileName();

        ArrayAdapter<String> splitAdapter = new ArrayAdapter<String>(this,
                android.R.layout.simple_spinner_item,
                java.util.Arrays.asList(SPLIT_OPTIONS));
        splitAdapter.setDropDownViewResource(
                android.R.layout.simple_spinner_dropdown_item);
        spSplit.setAdapter(splitAdapter);
        ArrayAdapter<String> methodAdapter = new ArrayAdapter<String>(this,
                android.R.layout.simple_spinner_item,
                java.util.Arrays.asList(ZIP_METHODS));
        methodAdapter.setDropDownViewResource(
                android.R.layout.simple_spinner_dropdown_item);
        spMethod.setAdapter(methodAdapter);
        spFormat.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                refreshFormat();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        spLevel.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                refreshMethodRow();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        spSplit.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                etSplitMb.setEnabled(pos == SPLIT_OPTIONS.length - 1);
                refreshNote();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        ((Button) findViewById(R.id.btnShowPassword)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        togglePassword();
                    }
                });
        ((Button) findViewById(R.id.btnClearInput)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        compressFileUris.clear();
                        compressFolderUris.clear();
                        compressFolderNames.clear();
                        updateCompressInputSummary();
                        refreshFormatRows();
                        refreshFormat();
                    }
                });

        ((Button) findViewById(R.id.btnAddFiles)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                        i.addCategory(Intent.CATEGORY_OPENABLE);
                        i.setType("*/*");
                        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                        startActivityForResult(i, REQ_COMPRESS_FILES);
                    }
                });
        ((Button) findViewById(R.id.btnAddFolder)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                        startActivityForResult(i, REQ_COMPRESS_FOLDER);
                    }
                });
        ((Button) findViewById(R.id.btnPickOutput)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        pickCompressOutputFile(pendingName());
                    }
                });
        ((Button) findViewById(R.id.btnPickOutputTree)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                        startActivityForResult(i, REQ_COMPRESS_OUTPUT_TREE);
                    }
                });
        btnCompress.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                doCompress();
            }
        });
        btnCancelCompress.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (compressOp != null) {
                    compressOp.cancel();
                }
            }
        });
        updateCompressInputSummary();
        updateCompressOutputSummary();
        refreshFormatRows();
        refreshFormat();
        resetFileName();
    }

    // MT block logic (moved inline from the old dialog draft) -------------

    // Rebuilds the format rows: single-file codecs are only enabled for
    // exactly one file and no folders (MT greys them out otherwise).
    private void refreshFormatRows() {
        formatRows.clear();
        boolean singleOk = compressFileUris.size() == 1 && compressFolderUris.isEmpty();
        for (AaeCapabilities.FormatCaps fc : AaeCapabilities.writableFormats()) {
            boolean enabled = true;
            String why = "";
            if (AaeCapabilities.isSingleFileFormat(fc.format) && !singleOk) {
                enabled = false;
                why = "needs exactly one file";
            }
            formatRows.add(new FormatRow(fc, fc.displayName, enabled, why));
        }
        spFormat.setAdapter(new FormatAdapter(this, formatRows));
    }

    private FormatRow selectedRow() {
        int pos = spFormat.getSelectedItemPosition();
        if (pos < 0 || pos >= formatRows.size()) {
            pos = 0;
        }
        return formatRows.get(pos);
    }

    private void refreshFormat() {
        if (spFormat == null || etFileName == null) {
            return;
        }
        FormatRow row = selectedRow();
        String ext = row.caps.extension;
        // Swap the extension in place, keeping the user's basename edit.
        String text = etFileName.getText().toString();
        String oldSuffix = "." + currentExt;
        String base = text.endsWith(oldSuffix)
                ? text.substring(0, text.length() - oldSuffix.length()) : text;
        if (base.length() == 0) {
            base = suggestedBase();
        }
        currentExt = ext;
        if (!text.endsWith("." + ext)) {
            syncingName = true;
            etFileName.setText(base + "." + ext);
            etFileName.setSelection(0, base.length());
            syncingName = false;
        }
        List<String> levels = AaeCapabilities.mtLevelsFor(row.caps.format);
        ArrayAdapter<String> levelAdapter = new ArrayAdapter<String>(this,
                android.R.layout.simple_spinner_item, levels);
        levelAdapter.setDropDownViewResource(
                android.R.layout.simple_spinner_dropdown_item);
        spLevel.setAdapter(levelAdapter);
        for (int i = 0; i < levels.size(); i++) {
            if (AaeCapabilities.MT_NORMAL.equals(levels.get(i))) {
                spLevel.setSelection(i);
                break;
            }
        }
        refreshMethodRow();
        refreshNote();
    }

    private void refreshMethodRow() {
        if (methodRow == null || spFormat == null) {
            return;
        }
        FormatRow row = selectedRow();
        boolean zip = row.caps != null && row.caps.format == AaeFormat.ZIP;
        methodRow.setVisibility(zip ? View.VISIBLE : View.GONE);
    }

    private void refreshNote() {
        if (tvCompressNote == null || spFormat == null) {
            return;
        }
        FormatRow row = selectedRow();
        StringBuilder sb = new StringBuilder();
        sb.append(row.caps.note);
        if (!row.enabled) {
            sb.append(" [Disabled: ").append(row.why).append("]");
        }
        if (row.caps.format == AaeFormat.ZIP && passwordField().length() > 0) {
            sb.append(" Password uses AES-256 (WinZip-AES).");
        }
        if (spSplit.getSelectedItemPosition() != 0) {
            if (sb.length() > 0) {
                sb.append(" ");
            }
            sb.append("Split archives are not supported by the engine yet.");
        }
        tvCompressNote.setText(sb.toString());
    }

    private String passwordField() {
        return etCompressPassword == null ? "" : etCompressPassword.getText().toString();
    }

    private void togglePassword() {
        showingPassword = !showingPassword;
        int sel = passwordField().length();
        if (showingPassword) {
            etCompressPassword.setInputType(InputType.TYPE_CLASS_TEXT
                    | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
            etCompressPassword.setTransformationMethod(
                    HideReturnsTransformationMethod.getInstance());
        } else {
            etCompressPassword.setInputType(InputType.TYPE_CLASS_TEXT
                    | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            etCompressPassword.setTransformationMethod(
                    PasswordTransformationMethod.getInstance());
        }
        etCompressPassword.setSelection(sel);
    }

    // Suggests the filename from the current inputs (MT pre-fills the same
    // way) without clobbering a name the user already typed: any manual
    // edit flips fileNameAuto off (via the watcher below); programmatic
    // extension swaps hold syncingName so they never count as edits.
    private boolean fileNameAuto = true;
    private boolean syncingName;

    private void watchFileName() {
        etFileName.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                if (!syncingName) {
                    fileNameAuto = false;
                }
            }
        });
    }

    private void resetFileName() {
        if (!fileNameAuto) {
            return;
        }
        String base = suggestedBase();
        syncingName = true;
        etFileName.setText(base + "." + currentExt);
        etFileName.setSelection(0, base.length());
        syncingName = false;
    }

    private void showFail(String msg) {
        tvCompressError.setText(msg == null ? "" : msg);
    }

    // Collects + validates the tab block into a runnable Config.
    private CompressConfig collectConfig() {
        showFail("");
        refreshNote();
        FormatRow row = selectedRow();
        if (!row.enabled) {
            showFail("Format unavailable: " + row.why + ".");
            return null;
        }
        String name = etFileName.getText().toString().replace('/', '_')
                .replace('\\', '_').trim();
        if (name.length() == 0) {
            showFail("Enter a file name.");
            return null;
        }
        if (!name.endsWith("." + currentExt)) {
            name = name + "." + currentExt;
        }
        List<String> levels = AaeCapabilities.mtLevelsFor(row.caps.format);
        int levelPos = spLevel.getSelectedItemPosition();
        if (levelPos < 0 || levelPos >= levels.size()) {
            showFail("Pick a compression level.");
            return null;
        }
        AaeMethod zipMethod = AaeMethod.DEFLATE;
        if (row.caps.format == AaeFormat.ZIP) {
            int mp = spMethod.getSelectedItemPosition();
            if (mp >= 0 && mp < ZIP_METHOD_VALUES.length) {
                zipMethod = ZIP_METHOD_VALUES[mp];
            }
        }
        String password = passwordField();
        if (password.length() == 0) {
            password = null;
        }
        if (spSplit.getSelectedItemPosition() != 0) {
            if (spSplit.getSelectedItemPosition() == SPLIT_OPTIONS.length - 1) {
                try {
                    if (Integer.parseInt(etSplitMb.getText().toString().trim()) <= 0) {
                        showFail("Split size must be a positive number of MB.");
                        return null;
                    }
                } catch (NumberFormatException e) {
                    showFail("Split size must be a positive number of MB.");
                    return null;
                }
            }
            showFail("Split archives are not supported by the engine yet (pick None).");
            return null;
        }
        boolean perFile = swPerFile.isChecked();
        if (perFile && compressOutputTreeUri == null) {
            showFail("Pick an output folder first (per-file mode writes one archive per item).");
            return null;
        }
        if (!perFile && compressOutputUri == null) {
            showFail("Pick an output file first.");
            return null;
        }
        try {
            AaeCapabilities.validateWrite(row.caps.format, AaeCapabilities.mtOptions(
                    row.caps.format, zipMethod, levels.get(levelPos), password));
        } catch (AaeException e) {
            showFail(e.getMessage());
            return null;
        } catch (IllegalArgumentException e) {
            showFail(e.getMessage());
            return null;
        }
        CompressConfig cfg = new CompressConfig();
        cfg.fileName = name;
        cfg.format = row.caps;
        cfg.zipMethod = zipMethod;
        cfg.mtLevel = levels.get(levelPos);
        cfg.password = password;
        cfg.perFile = perFile;
        cfg.deleteSource = swDeleteSource.isChecked();
        return cfg;
    }

    // Suggested archive basename for the filename field: the single input's
    // name, or "archive" for multi-input (MT pre-fills the same way).
    private String suggestedBase() {
        if (compressFileUris.size() == 1 && compressFolderUris.isEmpty()) {
            String raw = SafHelper.displayName(getContentResolver(), compressFileUris.get(0));
            String clean = SafHelper.sanitizeSegment(raw, "archive");
            return stripExt(clean);
        }
        if (compressFileUris.isEmpty() && compressFolderUris.size() == 1
                && !compressFolderNames.isEmpty()) {
            return compressFolderNames.get(0);
        }
        return "archive";
    }

    private String pendingName() {
        return pendingConfig != null ? pendingConfig.fileName : "archive.zip";
    }

    private void pickCompressOutputFile(String name) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(SafHelper.mimeFor(name));
        i.putExtra(Intent.EXTRA_TITLE, name);
        i.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, REQ_COMPRESS_OUTPUT);
    }

    private void updateCompressInputSummary() {
        int files = compressFileUris.size();
        int folders = compressFolderUris.size();
        if (files == 0 && folders == 0) {
            tvInputSummary.setText("no input selected");
            return;
        }
        StringBuilder sb = new StringBuilder();
        sb.append(files).append(files == 1 ? " file" : " files");
        sb.append(" + ").append(folders).append(folders == 1 ? " folder" : " folders");
        if (!compressFolderNames.isEmpty()) {
            sb.append(" (");
            for (int i = 0; i < compressFolderNames.size() && i < 3; i++) {
                if (i > 0) {
                    sb.append(", ");
                }
                sb.append(compressFolderNames.get(i));
            }
            if (compressFolderNames.size() > 3) {
                sb.append(", ...");
            }
            sb.append(")");
        }
        tvInputSummary.setText(sb.toString());
    }

    private void updateCompressOutputSummary() {
        StringBuilder sb = new StringBuilder();
        if (compressOutputUri == null && compressOutputTreeUri == null) {
            sb.append("no output selected");
        } else {
            if (compressOutputUri != null) {
                sb.append("file: ").append(SafHelper.displayName(
                        getContentResolver(), compressOutputUri));
            }
            if (compressOutputTreeUri != null) {
                if (sb.length() > 0) {
                    sb.append('\n');
                }
                sb.append("folder: ").append(SafHelper.displayName(
                        getContentResolver(), compressOutputTreeUri));
            }
        }
        tvOutputSummary.setText(sb.toString());
    }

    // "Compress" validates the MT block; its Config either runs at once
    // (output already picked) or arms a picker whose result resumes the run.
    private void doCompress() {
        if (compressFileUris.isEmpty() && compressFolderUris.isEmpty()) {
            setCompressStatus("Add input files or a folder first.");
            return;
        }
        CompressConfig config = collectConfig();
        if (config == null) {
            return;
        }
        startConfigRun(config);
    }

    private void startConfigRun(CompressConfig config) {
        if (config.perFile) {
            if (compressOutputTreeUri == null) {
                pendingConfig = config;
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                        | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                        | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                startActivityForResult(i, REQ_COMPRESS_OUTPUT_TREE);
                return;
            }
        } else if (compressOutputUri == null) {
            pendingConfig = config;
            pickCompressOutputFile(config.fileName);
            return;
        }
        pendingConfig = null;
        runCompressJob(config);
    }

    private void runCompressJob(final CompressConfig config) {
        final AaeWriteOptions options;
        try {
            options = AaeCapabilities.mtOptions(config.format.format, config.zipMethod,
                    config.mtLevel, config.password);
            AaeCapabilities.validateWrite(config.format.format, options);
        } catch (IllegalArgumentException e) {
            setCompressStatus("Invalid settings: " + e.getMessage());
            return;
        } catch (AaeException e) {
            setCompressStatus("Unsupported combination: " + e.getMessage());
            return;
        }
        setCompressBusy(true);
        setCompressStatus("");
        compressLog("compress: " + config.format.displayName + " / " + config.mtLevel
                + (config.perFile ? " / per-file" : "")
                + (config.password == null ? "" : " / password"));
        compressOp = AaeRunner.submit(new AaeRunner.Job() {
            @Override
            public void run(AaeRunner.OpHandle op) throws Exception {
                if (config.perFile) {
                    runPerFileJob(op, config, options);
                } else {
                    runSingleJob(op, config, options);
                }
            }
        }, new AaeRunner.FailAware() {
            @Override
            public void onDone(AaeRunner.OpHandle op) {
                setCompressBusy(false);
            }

            @Override
            public void onFail(AaeRunner.OpHandle op, Exception e) {
                setCompressBusy(false);
                if (e instanceof AaeException
                        && ((AaeException) e).kind == AaeException.Kind.CANCELLED) {
                    setCompressStatus("Cancelled.");
                    compressLog("cancelled");
                } else {
                    setCompressStatus("Failed: " + e.getMessage());
                    compressLog("FAILED: " + e);
                }
            }
        });
    }

    // One archive from all inputs (MT default mode).
    private void runSingleJob(AaeRunner.OpHandle op, CompressConfig config,
            AaeWriteOptions options) throws Exception {
        File jobRoot = SafHelper.freshDir(getCacheDir(), "compress");
        File staging = new File(jobRoot, "in");
        staging.mkdirs();
        final File tmpOut = new File(jobRoot, "out." + config.format.extension);
        List<SafHelper.StagedInput> staged = stageAllInputs(op, staging);
        sortStaged(staged);
        // Create directly through a session so staged cache files keep their
        // archive-relative names (no re-flattening of renamed temp files).
        commitStaged(op, config, options, staged, tmpOut, null);
        if (op.isCancelled()) {
            SafHelper.deleteRecursive(jobRoot);
            throw new AaeException(AaeException.Kind.CANCELLED, "cancelled");
        }
        // Publish through SAF (never assume the destination is a file path).
        try {
            SafHelper.copyFileToUri(getContentResolver(), tmpOut, compressOutputUri);
        } catch (Exception e) {
            SafHelper.deleteRecursive(jobRoot);
            throw new AaeException(AaeException.Kind.IO,
                    "cannot write output (" + e.getMessage() + ")", e);
        }
        final long size = tmpOut.length();
        final int count = staged.size();
        SafHelper.deleteRecursive(new File(jobRoot, "in"));
        tmpOut.delete();
        if (config.deleteSource) {
            final int gone = deleteCompressSources();
            AaeRunner.postUi(new Runnable() {
                @Override
                public void run() {
                    compressLog("deleted " + gone + " source(s)");
                }
            });
        }
        AaeRunner.postUi(new Runnable() {
            @Override
            public void run() {
                pbCompress.setProgress(100);
                setCompressStatus("");
                compressLog("OK: " + count + " entries -> " + size + "b");
            }
        });
    }

    // One archive per top-level pick (MT "compress each independently").
    // Folder keeps its tree (and name) inside its archive; a file lands at
    // the archive root. Archives are created inside the picked output folder.
    private void runPerFileJob(AaeRunner.OpHandle op, CompressConfig config,
            AaeWriteOptions options) throws Exception {
        File jobRoot = SafHelper.freshDir(getCacheDir(), "compress");
        List<String> skipped = new ArrayList<String>();
        int doneGroups = 0;
        int failedGroups = 0;
        List<Uri> pickedFiles = new ArrayList<Uri>(compressFileUris);
        List<Uri> pickedTrees = new ArrayList<Uri>(compressFolderUris);
        List<String> pickedNames = new ArrayList<String>(compressFolderNames);
        // Stage + commit per pick so each archive maps to its own sources
        // (for per-group delete-afterwards).
        for (int i = 0; i < pickedFiles.size(); i++) {
            if (op.isCancelled()) {
                throw new AaeException(AaeException.Kind.CANCELLED, "cancelled");
            }
            Uri uri = pickedFiles.get(i);
            File staging = new File(jobRoot, "in-" + i);
            staging.mkdirs();
            List<Uri> one = new ArrayList<Uri>();
            one.add(uri);
            List<SafHelper.StagedInput> staged;
            try {
                staged = SafHelper.stageMultipleFiles(this, one, staging, skipped);
            } catch (Exception e) {
                skipped.add(SafHelper.displayName(getContentResolver(), uri) + ": " + e);
                continue;
            }
            if (staged.isEmpty()) {
                continue;
            }
            String label = stripExt(SafHelper.sanitizeSegment(
                    SafHelper.displayName(getContentResolver(), uri), "file-" + i));
            List<Uri> sources = new ArrayList<Uri>();
            sources.add(uri);
            if (commitOneGroup(op, config, options, jobRoot, i, label, staged, skipped)) {
                doneGroups++;
                if (config.deleteSource) {
                    deleteUris(sources);
                }
            } else {
                failedGroups++;
            }
            postSkipped(skipped);
            skipped.clear();
        }
        for (int i = 0; i < pickedTrees.size(); i++) {
            if (op.isCancelled()) {
                throw new AaeException(AaeException.Kind.CANCELLED, "cancelled");
            }
            Uri tree = pickedTrees.get(i);
            String top = i < pickedNames.size() ? pickedNames.get(i) : "folder";
            top = SafHelper.sanitizeSegment(top, "folder");
            File staging = new File(jobRoot, "in-t" + i);
            staging.mkdirs();
            List<SafHelper.StagedInput> staged;
            try {
                staged = SafHelper.stageTree(this, tree, top, staging, skipped);
            } catch (Exception e) {
                skipped.add(top + ": " + e.getMessage());
                continue;
            }
            if (staged.isEmpty()) {
                continue;
            }
            List<Uri> sources = new ArrayList<Uri>();
            sources.add(tree);
            if (commitOneGroup(op, config, options, jobRoot, 1000 + i, top, staged,
                    skipped)) {
                doneGroups++;
                if (config.deleteSource) {
                    deleteUris(sources);
                }
            } else {
                failedGroups++;
            }
            postSkipped(skipped);
            skipped.clear();
        }
        SafHelper.deleteRecursive(jobRoot);
        final int okGroups = doneGroups;
        final int badGroups = failedGroups;
        if (okGroups == 0) {
            throw new AaeException(AaeException.Kind.IO,
                    "no archives created (" + badGroups + " failed)");
        }
        AaeRunner.postUi(new Runnable() {
            @Override
            public void run() {
                pbCompress.setProgress(100);
                setCompressStatus(badGroups == 0 ? "" : badGroups + " item(s) failed.");
                compressLog("OK: " + okGroups + " archive(s)"
                        + (badGroups == 0 ? "" : ", " + badGroups + " failed"));
            }
        });
    }

    // Stages every picked input (files + folders) for single-archive mode.
    private List<SafHelper.StagedInput> stageAllInputs(AaeRunner.OpHandle op, File staging)
            throws Exception {
        List<String> skipped = new ArrayList<String>();
        List<SafHelper.StagedInput> staged = new ArrayList<SafHelper.StagedInput>();
        if (!compressFileUris.isEmpty()) {
            staged.addAll(SafHelper.stageMultipleFiles(
                    this, new ArrayList<Uri>(compressFileUris), staging, skipped));
        }
        for (int i = 0; i < compressFolderUris.size(); i++) {
            if (op.isCancelled()) {
                throw new AaeException(AaeException.Kind.CANCELLED, "cancelled");
            }
            Uri tree = compressFolderUris.get(i);
            String top = i < compressFolderNames.size()
                    ? compressFolderNames.get(i) : "folder";
            top = SafHelper.sanitizeSegment(top, "folder");
            staged.addAll(SafHelper.stageTree(this, tree, top, staging, skipped));
        }
        postSkippedSync(skipped);
        if (staged.isEmpty()) {
            throw new AaeException(AaeException.Kind.IO,
                    "no readable input (all " + skipped.size() + " skipped)");
        }
        return staged;
    }

    // MT writes entries sorted; keep the engine output deterministic too.
    private static void sortStaged(List<SafHelper.StagedInput> staged) {
        for (int i = 1; i < staged.size(); i++) {
            SafHelper.StagedInput key = staged.get(i);
            int j = i - 1;
            while (j >= 0 && staged.get(j).entryName.compareTo(key.entryName) > 0) {
                staged.set(j + 1, staged.get(j));
                j--;
            }
            staged.set(j + 1, key);
        }
    }

    private void postSkipped(final List<String> skipped) {
        final List<String> copy = new ArrayList<String>(skipped);
        AaeRunner.postUi(new Runnable() {
            @Override
            public void run() {
                for (String s : copy) {
                    compressLog("skip: " + s);
                }
            }
        });
    }

    private void postSkippedSync(List<String> skipped) {
        for (String s : skipped) {
            compressLog("skip: " + s);
        }
    }

    // Commits one staged group into tmpOut through a native session.
    // progressBase/progressScale map the group onto the shared progress bar;
    // here the whole bar (single mode) — per-file callers pass their slice.
    private void commitStaged(AaeRunner.OpHandle op, CompressConfig config,
            AaeWriteOptions options, List<SafHelper.StagedInput> staged, File tmpOut,
            int[] progress) throws Exception {
        AaeSession session = null;
        try {
            session = NativeAaeSession.openNative(
                    tmpOut, config.format.format, options.password, options.encryption,
                    true, true);
            int done = 0;
            final int total = staged.size();
            for (SafHelper.StagedInput si : staged) {
                if (op.isCancelled()) {
                    throw new AaeException(AaeException.Kind.CANCELLED, "cancelled");
                }
                if (si.isDirectory) {
                    session.addDirectory(si.entryName);
                } else {
                    session.addFile(si.entryName, si.file, options);
                }
                done++;
                final int pct = progress == null
                        ? (total == 0 ? 100 : done * 100 / total)
                        : progress[0] + done * (progress[1] - progress[0]) / total;
                AaeRunner.postUi(new Runnable() {
                    @Override
                    public void run() {
                        pbCompress.setProgress(pct);
                    }
                });
            }
            session.close();
            session = null;
        } catch (AaeException e) {
            if (session != null) {
                session.discard();
            }
            tmpOut.delete();
            throw e;
        }
    }

    // Commits + publishes one per-file group into the output tree.
    private boolean commitOneGroup(AaeRunner.OpHandle op, CompressConfig config,
            AaeWriteOptions options, File jobRoot, int tag, String label,
            List<SafHelper.StagedInput> staged, List<String> skipped) {
        sortStaged(staged);
        File tmpOut = new File(jobRoot, "out-" + tag + "." + config.format.extension);
        String archiveName =
                SafHelper.sanitizeSegment(label, "archive") + "." + config.format.extension;
        try {
            commitStaged(op, config, options, staged, tmpOut, null);
        } catch (Exception e) {
            skipped.add(label + ": " + e.getMessage());
            return false;
        }
        if (op.isCancelled()) {
            return false;
        }
        try {
            Uri dst = SafHelper.createFileInTree(this, compressOutputTreeUri,
                    SafHelper.mimeFor(archiveName), archiveName);
            SafHelper.copyFileToUri(getContentResolver(), tmpOut, dst);
        } catch (Exception e) {
            skipped.add(label + ": cannot write output (" + e.getMessage() + ")");
            tmpOut.delete();
            return false;
        }
        final long size = tmpOut.length();
        tmpOut.delete();
        final String doneName = archiveName;
        AaeRunner.postUi(new Runnable() {
            @Override
            public void run() {
                compressLog("OK: " + doneName + " (" + size + "b)");
            }
        });
        return true;
    }

    private int deleteCompressSources() {
        List<Uri> all = new ArrayList<Uri>(compressFileUris);
        all.addAll(compressFolderUris);
        return deleteUris(all);
    }

    private int deleteUris(List<Uri> uris) {
        int gone = 0;
        for (Uri u : uris) {
            if (u != null && SafHelper.deleteDocument(getContentResolver(), u)) {
                gone++;
            }
        }
        return gone;
    }

    private void setCompressBusy(final boolean busy) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                btnCompress.setEnabled(!busy);
                btnCancelCompress.setEnabled(busy);
                pbCompress.setVisibility(busy ? View.VISIBLE : View.GONE);
                if (busy) {
                    pbCompress.setProgress(0);
                }
            }
        });
    }

    private void setCompressStatus(final String s) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                tvCompressStatus.setText(s);
            }
        });
    }

    // ================= PICK RESULTS =================

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) {
            return;
        }
        if (requestCode == REQ_PICK_ARCHIVE) {
            final Uri uri = data.getData();
            if (uri == null) {
                return;
            }
            SafHelper.takePersistablePermissions(this, data);
            setTestBusy(true);
            AaeRunner.submit(new AaeRunner.Job() {
                @Override
                public void run(AaeRunner.OpHandle op) {
                    try {
                        File inbox = SafHelper.inboxDir(MainActivity.this);
                        String name = SafHelper.displayName(getContentResolver(), uri);
                        final File dst = new File(inbox,
                                safeFileName(name) + "-" + System.currentTimeMillis());
                        SafHelper.copyUriToFile(getContentResolver(), uri, dst);
                        currentFile = dst;
                        currentName = name;
                        AaeRunner.postUi(new Runnable() {
                            @Override
                            public void run() {
                                tvFile.setText(currentName + "  [" + currentFile.length() + "b]");
                                setTestButtonsEnabled(true);
                                setTestStatus("");
                                log("picked: " + currentName + " (" + dst.length() + "b)");
                            }
                        });
                    } catch (final Exception e) {
                        AaeRunner.postUi(new Runnable() {
                            @Override
                            public void run() {
                                setTestStatus("Pick failed: " + e.getMessage());
                                log("pick failed: " + e);
                            }
                        });
                    } finally {
                        AaeRunner.postUi(new Runnable() {
                            @Override
                            public void run() {
                                setTestBusy(false);
                            }
                        });
                    }
                }
            }, null);
        } else if (requestCode == REQ_EXTRACT_TREE) {
            extractTreeUri = data.getData();
            SafHelper.takePersistablePermissions(this, data);
            if (extractTreeUri != null) {
                tvExtractDir.setText("output: "
                        + SafHelper.displayName(getContentResolver(), extractTreeUri));
                log("extract folder: " + extractTreeUri);
            }
        } else if (requestCode == REQ_COMPRESS_FILES) {
            if (data.getClipData() != null) {
                ClipData clip = data.getClipData();
                for (int i = 0; i < clip.getItemCount(); i++) {
                    Uri u = clip.getItemAt(i).getUri();
                    if (u != null) {
                        SafHelper.takePersistablePermissions(this, u,
                                data.getFlags() | Intent.FLAG_GRANT_READ_URI_PERMISSION);
                        compressFileUris.add(u);
                    }
                }
            } else if (data.getData() != null) {
                Uri u = data.getData();
                SafHelper.takePersistablePermissions(this, data);
                compressFileUris.add(u);
            }
            updateCompressInputSummary();
            refreshFormatRows();
            refreshFormat();
            resetFileName();
            compressLog(compressFileUris.size() + " file(s) selected");
        } else if (requestCode == REQ_COMPRESS_FOLDER) {
            Uri tree = data.getData();
            if (tree != null) {
                SafHelper.takePersistablePermissions(this, data);
                compressFolderUris.add(tree);
                compressFolderNames.add(
                        SafHelper.treeDisplayName(getContentResolver(), tree));
                updateCompressInputSummary();
                refreshFormatRows();
                refreshFormat();
                resetFileName();
                compressLog("folder added: " + compressFolderNames.get(
                        compressFolderNames.size() - 1));
            }
        } else if (requestCode == REQ_COMPRESS_OUTPUT) {
            compressOutputUri = data.getData();
            SafHelper.takePersistablePermissions(this, data);
            updateCompressOutputSummary();
            if (compressOutputUri != null) {
                compressLog("output: " + SafHelper.displayName(
                        getContentResolver(), compressOutputUri));
                if (pendingConfig != null && !pendingConfig.perFile) {
                    CompressConfig cfg = pendingConfig;
                    pendingConfig = null;
                    runCompressJob(cfg);
                }
            }
        } else if (requestCode == REQ_COMPRESS_OUTPUT_TREE) {
            compressOutputTreeUri = data.getData();
            SafHelper.takePersistablePermissions(this, data);
            updateCompressOutputSummary();
            if (compressOutputTreeUri != null) {
                compressLog("output folder: " + SafHelper.displayName(
                        getContentResolver(), compressOutputTreeUri));
                if (pendingConfig != null && pendingConfig.perFile) {
                    CompressConfig cfg = pendingConfig;
                    pendingConfig = null;
                    runCompressJob(cfg);
                }
            }
        }
    }

    // ================= helpers =================

    private void log(final String s) {
        android.util.Log.i("AaeTester", s);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                logBuf.append(s).append('\n');
                tvLog.setText(logBuf.toString());
            }
        });
    }

    private void compressLog(final String s) {
        android.util.Log.i("AaeCompress", s);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                compressLogBuf.append(s).append('\n');
                tvCompressLog.setText(compressLogBuf.toString());
            }
        });
    }

    private void setTestStatus(final String s) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                tvTestStatus.setText(s);
            }
        });
    }

    private void fail(String msg, AaeException e) {
        log(msg);
        setTestStatus(friendlyError(e));
    }

    /** Human-readable error instead of a raw log dump. */
    private static String describe(AaeException e) {
        return e.kind + ": " + e.getMessage();
    }

    private static String friendlyError(AaeException e) {
        switch (e.kind) {
            case PASSWORD_REQUIRED:
                return "This archive needs a password — enter it and retry.";
            case PASSWORD_WRONG:
                return "Wrong password.";
            case NOT_ARCHIVE:
                return "Not a supported archive.";
            case UNSUPPORTED_FORMAT:
                return "Unsupported format: " + e.getMessage();
            case UNSUPPORTED_OPERATION:
                return "Not supported: " + e.getMessage();
            case ENTRY_NOT_FOUND:
                return "Entry not found: " + e.getMessage();
            case UNSAFE_NAME:
                return "Unsafe entry name blocked: " + e.getMessage();
            case TOO_LARGE:
                return "Entry too large for preview — extract it instead.";
            case CANCELLED:
                return "Cancelled.";
            default:
                return "Failed: " + e.getMessage();
        }
    }

    private static String methodName(int m) {
        if (m == 0) {
            return "STORE";
        } else if (m == 8) {
            return "DEFLATE";
        } else if (m == 12) {
            return "BZIP2";
        } else if (m == 95) {
            return "XZ";
        } else if (m == 93) {
            return "ZSTD";
        } else if (m == -1) {
            return "?";
        }
        return "m" + m;
    }

    private static String stripExt(String n) {
        int d = n.lastIndexOf('.');
        return d > 0 ? n.substring(0, d) : n;
    }

    private static String safeFileName(String n) {
        if (n == null || n.length() == 0) {
            return "archive";
        }
        String s = n.replace('/', '_').replace('\\', '_');
        if (s.length() > 80) {
            s = s.substring(0, 80);
        }
        return s;
    }

    private static void writeString(File f, String s) throws Exception {
        FileOutputStream out = null;
        try {
            out = new FileOutputStream(f);
            out.write(s.getBytes("UTF-8"));
        } finally {
            if (out != null) {
                try {
                    out.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    private static String readString(File f) throws Exception {
        java.io.FileInputStream in = null;
        try {
            in = new java.io.FileInputStream(f);
            byte[] buf = new byte[(int) f.length()];
            int off = 0;
            int n;
            while (off < buf.length && (n = in.read(buf, off, buf.length - off)) >= 0) {
                off += n;
            }
            return new String(buf, "UTF-8");
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (Exception ignored) {
                }
            }
        }
    }
}
