package com.motioncam.fuse;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.UriPermission;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.FrameLayout;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;
import org.json.JSONObject;

import org.qtproject.qt.android.bindings.QtActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.ByteBuffer;
import java.nio.channels.Channels;
import java.nio.channels.WritableByteChannel;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

public class MotionCamActivity extends QtActivity {
    private static final int CHOOSE_SIDECAR_DIRECTORY = 6302;
    private static final int CHOOSE_FINALIZE_DIRECTORY = 6303;
    private static final int CHOOSE_SEQUENCE_FOLDER = 6305;
    private static final String PREFS = "motioncam_documents";
    private static final String FINALIZE_TREE_KEY = "finalize_tree_uri";
    private static final String CUSTOM_FINALIZE_TREE_KEY = "custom_finalize_tree_uri";
    private String pendingSidecarImportRoot;
    private volatile String sidecarScanStatus = "idle";
    private PowerManager.WakeLock finalizeWakeLock;
    private boolean pendingFinalizeTree;
    private volatile String finalizeTreeSelectionStatus = "idle";
    private Uri activeFinalizeFolder;
    private volatile String sequenceFolderImportStatus = "idle";
    private final ArrayList<String> sequenceFolderImportPaths = new ArrayList<>();

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        applyEdgeToEdge();
    }

    private ImageView pageFadeOverlay;
    private volatile int pageFadeReadyGeneration;
    private volatile int pageFadeFinishedGeneration;
    private volatile int pageFadeGeneration;

    public boolean crossfadePage(int[] pixels, int width, int height,
                                 float from, float to, boolean hold) {
        final int generation = ++pageFadeGeneration;
        final Bitmap bitmap;
        try {
            bitmap = Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888);
        } catch (RuntimeException | OutOfMemoryError failure) {
            pageFadeFinishedGeneration = generation;
            runOnUiThread(() -> {
                if (pageFadeGeneration == generation) removePageCrossfade(pageFadeOverlay);
            });
            return false;
        }
        runOnUiThread(() -> {
            if (pageFadeGeneration != generation) return;
            removePageCrossfade(pageFadeOverlay);
            ViewGroup host = findViewById(android.R.id.content);
            if (host == null) { pageFadeFinishedGeneration = generation; return; }
            ImageView layer = new ImageView(this);
            layer.setScaleType(ImageView.ScaleType.FIT_XY);
            layer.setImageBitmap(bitmap);
            layer.setAlpha(from);
            layer.setClickable(hold);
            layer.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
            layer.setLayerType(View.LAYER_TYPE_HARDWARE, null);
            host.addView(layer, new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            pageFadeOverlay = layer;
            layer.postOnAnimation(() -> layer.postOnAnimation(() -> {
                if (pageFadeOverlay == layer && pageFadeGeneration == generation)
                    pageFadeReadyGeneration = generation;
            }));
            if (from != to)
                layer.animate().alpha(to).setDuration(160).withEndAction(() -> {
                    if (pageFadeOverlay != layer || pageFadeGeneration != generation) return;
                    pageFadeFinishedGeneration = generation;
                    if (!hold) removePageCrossfade(layer);
                }).start();
            else
                pageFadeFinishedGeneration = generation;
        });
        return true;
    }

    private void removePageCrossfade(ImageView layer) {
        if (layer == null || pageFadeOverlay != layer) return;
        pageFadeOverlay = null;
        pageFadeReadyGeneration = 0;
        layer.animate().cancel();
        if (layer.getParent() instanceof ViewGroup)
            ((ViewGroup) layer.getParent()).removeView(layer);
        layer.setImageDrawable(null);
    }

    public boolean pageCrossfadeReady() { return pageFadeReadyGeneration == pageFadeGeneration; }
    public boolean pageCrossfadeFinished() { return pageFadeFinishedGeneration == pageFadeGeneration; }

    public void revealPageCrossfade() {
        final int generation = pageFadeGeneration;
        runOnUiThread(() -> {
            if (pageFadeGeneration != generation) return;
            final ImageView layer = pageFadeOverlay;
            if (layer == null) return;
            layer.postOnAnimation(() -> layer.postOnAnimation(() -> {
                if (pageFadeOverlay != layer || pageFadeGeneration != generation) return;
                layer.setClickable(false);
                pageFadeFinishedGeneration = 0;
                layer.animate().alpha(0.0f).setDuration(160).withEndAction(() -> {
                    if (pageFadeOverlay != layer || pageFadeGeneration != generation) return;
                    pageFadeFinishedGeneration = generation;
                    removePageCrossfade(layer);
                }).start();
            }));
        });
    }

    public void finishPageCrossfade() {
        final int generation = pageFadeGeneration;
        runOnUiThread(() -> {
            if (pageFadeGeneration != generation) return;
            final ImageView layer = pageFadeOverlay;
            if (layer != null)
                layer.postOnAnimation(() -> layer.postOnAnimation(() -> {
                    if (pageFadeGeneration == generation) removePageCrossfade(layer);
                }));
        });
    }

    public String galleryPerfArgument(String name) {
        return getIntent().getStringExtra(name);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyEdgeToEdge();
    }

    private void applyEdgeToEdge() {
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attributes);
        }
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
        }
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(decor.getSystemUiVisibility()
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    public void setMotionCamLandscapeOnly(boolean landscapeOnly) {
        runOnUiThread(() -> setRequestedOrientation(landscapeOnly
                ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                : ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR));
    }

    public synchronized void beginFinalizeWork() {
        runOnUiThread(() -> getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
        if (finalizeWakeLock == null) {
            PowerManager power = (PowerManager) getSystemService(POWER_SERVICE);
            if (power != null)
                finalizeWakeLock = power.newWakeLock(
                        PowerManager.PARTIAL_WAKE_LOCK, "MotionCamFuse:Finalize");
        }
        if (finalizeWakeLock != null && !finalizeWakeLock.isHeld())
            finalizeWakeLock.acquire();
    }

    public synchronized void endFinalizeWork() {
        if (finalizeWakeLock != null && finalizeWakeLock.isHeld())
            finalizeWakeLock.release();
        runOnUiThread(() -> getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON));
    }

    public synchronized String prepareFinalizeOutput(String requestedFolderName,
            String mode, String importPath) {
        try {
            Uri output;
            Uri tree;
            if ("source".equals(mode)) {
                File imported = new File(importPath);
                String key = importDirectory(imported).getName();
                String sourceTree = getSharedPreferences(PREFS, MODE_PRIVATE)
                        .getString("source_tree:" + key, null);
                String sourceUri = getSharedPreferences(PREFS, MODE_PRIVATE)
                        .getString("source_uri:" + key, null);
                if (sourceTree == null || sourceUri == null)
                    return "error:Scan this clip's source directory before using it as an output destination";
                tree = Uri.parse(sourceTree);
                Uri sourceDocument=Uri.parse(sourceUri);
                if(DocumentsContract.isTreeUri(sourceDocument)){
                    output=DocumentsContract.buildDocumentUriUsingTree(tree,
                            DocumentsContract.getTreeDocumentId(sourceDocument));
                }else{
                    String documentId = DocumentsContract.getDocumentId(sourceDocument);
                    int slash = documentId.lastIndexOf('/');
                    if (slash < 0) throw new IOException("Could not find the source folder");
                    output = DocumentsContract.buildDocumentUriUsingTree(tree,
                            documentId.substring(0, slash));
                }
            } else {
                boolean custom = "custom".equals(mode) || mode.startsWith("content:");
                String key = custom ? CUSTOM_FINALIZE_TREE_KEY : FINALIZE_TREE_KEY;
                String saved = mode.startsWith("content:") ? mode
                        : getSharedPreferences(PREFS, MODE_PRIVATE).getString(key, null);
                tree = saved == null ? null : Uri.parse(saved);
                if (!hasWriteGrant(tree)) {
                    if (custom)
                        return "error:Choose a custom output folder in Preferences first";
                    if (!pendingFinalizeTree) {
                        pendingFinalizeTree = true;
                        finalizeTreeSelectionStatus = "choosing";
                        runOnUiThread(() -> requestTreeSelection(CHOOSE_FINALIZE_DIRECTORY,
                                "Select Documents for finalized output. Finalization will continue when you return."));
                    }
                    return "choose";
                }
                Uri root = DocumentsContract.buildDocumentUriUsingTree(tree,
                        DocumentsContract.getTreeDocumentId(tree));
                if (custom) {
                    output = root;
                } else {
                    output = ensureDirectory(tree, root, "mcFuse");
                }
            }
            String name = requestedFolderName == null || requestedFolderName.isEmpty()
                    ? "Finalized Clip" : requestedFolderName;
            Uri clip = null;
            for (int suffix = 1; suffix < 10000 && clip == null; suffix++) {
                String candidate = suffix == 1 ? name : name + " (" + suffix + ")";
                if (findChild(tree, output, candidate,
                        DocumentsContract.Document.MIME_TYPE_DIR) == null)
                    clip = DocumentsContract.createDocument(getContentResolver(), output,
                            DocumentsContract.Document.MIME_TYPE_DIR, candidate);
            }
            if (clip == null) throw new IOException("Could not create a unique output folder");
            activeFinalizeFolder = clip;
            return "ready:" + displayNameForUri(clip.toString());
        } catch (Exception error) {
            return "error:" + error.getMessage();
        }
    }

    private boolean hasWriteGrant(Uri tree) {
        if (tree == null) return false;
        for (UriPermission permission : getContentResolver().getPersistedUriPermissions())
            if (tree.equals(permission.getUri()) && permission.isWritePermission()) return true;
        return false;
    }

    private void requestTreeSelection(int requestCode, String message) {
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        requestTreeSelection(requestCode, DocumentsContract.buildDocumentUri(
                "com.android.externalstorage.documents", "primary:Documents"));
    }

    private void requestTreeSelection(int requestCode, Uri initial) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        if (initial != null) intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, initial);
        startActivityForResult(intent, requestCode);
    }

    public String finalizeTreeSelectionStatus() {
        return finalizeTreeSelectionStatus;
    }

    public String finalizeDirectoryPath(String uri) {
        Uri tree = Uri.parse(uri);
        String id = DocumentsContract.getTreeDocumentId(tree);
        if (id.startsWith("primary:")) return sourceDirectoryName(tree);
        int colon = id.indexOf(':');
        return colon < 0 ? sourceDirectoryName(tree)
                : id.substring(0, colon) + "/" + id.substring(colon + 1);
    }

    public String legacyCustomFinalizeDirectory() {
        String value = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString(CUSTOM_FINALIZE_TREE_KEY, null);
        return value == null ? "" : value;
    }

    public void requestDngSequenceFolderImport() {
        synchronized (sequenceFolderImportPaths) { sequenceFolderImportPaths.clear(); }
        sequenceFolderImportStatus = "choosing";
        runOnUiThread(this::launchSequenceFolderPicker);
    }

    private void launchSequenceFolderPicker() {
        requestTreeSelection(CHOOSE_SEQUENCE_FOLDER, (Uri) null);
    }

    public String dngSequenceFolderImportStatus() {
        return sequenceFolderImportStatus;
    }

    public synchronized void writeFinalizeOutputEntry(String name, ByteBuffer bytes)
            throws IOException {
        if (activeFinalizeFolder == null) throw new IOException("Output folder is not ready");
        if (name == null || name.isEmpty() || name.contains("/") || name.contains("\\"))
            throw new IOException("Invalid output filename");
        // An interrupted write must not leave a file that looks like a DNG.
        Uri document = DocumentsContract.createDocument(getContentResolver(),
                activeFinalizeFolder, "application/octet-stream", name + ".partial");
        if (document == null) throw new IOException("Could not create " + name);
        try {
            try (OutputStream output = getContentResolver().openOutputStream(document, "w")) {
                if (output == null) throw new IOException("Could not open " + name);
                WritableByteChannel channel = Channels.newChannel(output);
                while (bytes.hasRemaining()) channel.write(bytes);
                output.flush();
            }
            if (DocumentsContract.renameDocument(getContentResolver(), document, name) == null)
                throw new IOException("Could not finish " + name);
        } catch (IOException | RuntimeException error) {
            try { DocumentsContract.deleteDocument(getContentResolver(), document); }
            catch (Exception ignored) { }
            throw error;
        }
    }

    public synchronized void finishFinalizeOutput() {
        activeFinalizeFolder = null;
        Toast.makeText(this, "Finalized files saved to the selected folder",
                Toast.LENGTH_LONG).show();
    }

    public synchronized void cancelFinalizeOutput() {
        Uri folder = activeFinalizeFolder;
        activeFinalizeFolder = null;
        if (folder != null) {
            try { DocumentsContract.deleteDocument(getContentResolver(), folder); }
            catch (Exception ignored) { }
        }
    }

    public String sidecarScanStatus() { return sidecarScanStatus; }

    // The user chooses the source directory, which may differ for every clip.
    // Scan that tree for imported clip names and copy every adjacent sidecar.
    public void scanDirectoryForSidecars(String importRoot, String targetImportPath) {
        sidecarScanStatus = "choosing";
        runOnUiThread(() -> {
            if (pendingSidecarImportRoot != null) return;
            pendingSidecarImportRoot = importRoot;
            requestTreeSelection(CHOOSE_SIDECAR_DIRECTORY,
                    initialSourceDirectory(targetImportPath));
        });
    }

    private Uri initialSourceDirectory(String importPath) {
        if (importPath == null || importPath.isEmpty()) return null;
        String folder = importDirectory(new File(importPath)).getName();
        String tree = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_tree:" + folder, null);
        if (tree != null) return Uri.parse(tree);
        String source = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_uri:" + folder, null);
        if (source == null) return null;
        Uri uri = Uri.parse(source);
        try {
            String id = DocumentsContract.getDocumentId(uri);
            int slash = id.lastIndexOf('/');
            if (slash <= 0) return null;
            return DocumentsContract.buildDocumentUri(uri.getAuthority(),
                    id.substring(0, slash));
        } catch (RuntimeException error) {
            return null;
        }
    }

    // The document URI can contain an opaque ID instead of the filename.
    // Query the provider for the name shown in Android's file picker.
    public String displayNameForUri(String sourceUri) {
        Uri uri = Uri.parse(sourceUri);
        if (!"content".equals(uri.getScheme())) return "";
        try (Cursor cursor = getContentResolver().query(uri,
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst())
                return cursor.getString(0);
        } catch (RuntimeException ignored) {
            // The native side reports a useful import error if the provider
            // cannot supply a name with a supported file extension.
        }
        return "";
    }

    public String sourceDirectoryForUri(String sourceUri) {
        Uri uri = Uri.parse(sourceUri);
        if (DocumentsContract.isDocumentUri(this, uri)) {
            try {
                String id = DocumentsContract.getDocumentId(uri);
                if (id.startsWith("primary:")) {
                    String path = id.substring("primary:".length());
                    int slash = path.lastIndexOf('/');
                    return "Internal storage/" + (slash < 0 ? "" : path.substring(0, slash));
                }
            } catch (RuntimeException ignored) { }
        }
        return uri.getAuthority() == null ? "Source document" : uri.getAuthority();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == CHOOSE_FINALIZE_DIRECTORY) {
            pendingFinalizeTree = false;
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                Uri tree = data.getData();
                try {
                    getContentResolver().takePersistableUriPermission(tree,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION |
                            Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                    getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                            .putString(FINALIZE_TREE_KEY, tree.toString()).apply();
                    finalizeTreeSelectionStatus = "ready";
                    Toast.makeText(this, "Finalized output destination saved",
                            Toast.LENGTH_LONG).show();
                } catch (SecurityException error) {
                    finalizeTreeSelectionStatus = "error";
                    Toast.makeText(this, "Could not access the selected folder",
                            Toast.LENGTH_LONG).show();
                }
            } else finalizeTreeSelectionStatus = "cancelled";
            return;
        }
        if (requestCode == CHOOSE_SEQUENCE_FOLDER) {
            if (resultCode != Activity.RESULT_OK || data == null || data.getData() == null) {
                synchronized (sequenceFolderImportPaths) {
                    sequenceFolderImportStatus = sequenceFolderImportPaths.isEmpty()
                            ? "cancelled" : "done:" + android.text.TextUtils.join("\n",
                                    sequenceFolderImportPaths);
                }
                return;
            }
            Uri tree = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(tree,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (SecurityException error) {
                sequenceFolderImportStatus = "error:Could not read the selected folder";
                return;
            }
            sequenceFolderImportStatus = "copying";
            beginFinalizeWork();
            new Thread(() -> {
                try {
                    File imported = copyDngSequenceFolder(tree);
                    synchronized (sequenceFolderImportPaths) {
                        sequenceFolderImportPaths.add(imported.getAbsolutePath());
                    }
                    sequenceFolderImportStatus = "choosing";
                    runOnUiThread(() -> {
                        Toast.makeText(this,
                                "Select another sequence folder, or press Back to finish",
                                Toast.LENGTH_LONG).show();
                        launchSequenceFolderPicker();
                    });
                } catch (Exception error) {
                    sequenceFolderImportStatus = "error:" + error.getMessage();
                } finally {
                    runOnUiThread(this::endFinalizeWork);
                }
            }, "mcFuse-dng-sequence-import").start();
            return;
        }
        if (requestCode == CHOOSE_SIDECAR_DIRECTORY) {
            String importRoot = pendingSidecarImportRoot;
            pendingSidecarImportRoot = null;
            if (resultCode != Activity.RESULT_OK || data == null ||
                    data.getData() == null || importRoot == null) {
                sidecarScanStatus = "cancelled";
                return;
            }
            Uri tree = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(tree,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (SecurityException error) {
                sidecarScanStatus = "error:Could not read the selected directory";
                return;
            }
            sidecarScanStatus = "scanning";
            new Thread(() -> {
                try {
                    int count = scanSidecarTree(tree, new File(importRoot));
                    sidecarScanStatus = "done:" + count;
                } catch (Exception error) {
                    sidecarScanStatus = "error:" + error.getMessage();
                }
            }, "mcFuse-sidecar-scan").start();
            return;
        }
    }

    private boolean isClipFile(String name) {
        if (name == null) return false;
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".mcraw") || lower.endsWith(".unspektra") ||
                lower.endsWith(".dng") || lower.endsWith(".mov") ||
                lower.endsWith(".mp4") || lower.endsWith(".mkv");
    }

    private int scanSidecarTree(Uri tree, File importRoot) throws IOException {
        File[] folders = importRoot.listFiles();
        if (folders == null) throw new IOException("Imported clips directory is missing");
        Map<String, ArrayList<File>> imports = new HashMap<>();
        for (File folder : folders) {
            if (!folder.isDirectory()) continue;
            File[] files = folder.listFiles();
            if (files == null) continue;
            for (File file : files) {
                if (file.isFile() && isClipFile(file.getName()))
                    imports.computeIfAbsent(file.getName().toLowerCase(Locale.ROOT),
                            ignored -> new ArrayList<>()).add(folder);
            }
        }
        Uri root = DocumentsContract.buildDocumentUriUsingTree(tree,
                DocumentsContract.getTreeDocumentId(tree));
        return scanSidecarFolder(tree, root, imports);
    }

    private int scanSidecarFolder(Uri tree, Uri folder,
            Map<String, ArrayList<File>> imports) throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
                DocumentsContract.getDocumentId(folder));
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE};
        ArrayList<String[]> entries = new ArrayList<>();
        try (Cursor cursor = getContentResolver().query(children, columns,
                null, null, null)) {
            if (cursor == null) throw new IOException("Could not list source directory");
            while (cursor.moveToNext())
                entries.add(new String[] {cursor.getString(0), cursor.getString(1),
                        cursor.getString(2)});
        }
        int copied = 0;
        for (String[] clip : entries) {
            if (!isClipFile(clip[1])) continue;
            ArrayList<File> candidates = imports.get(clip[1].toLowerCase(Locale.ROOT));
            if (candidates == null) continue;
            ArrayList<File> targets = new ArrayList<>();
            for (File candidate : candidates)
                if (matchesImportedDocument(candidate, tree, clip[0],
                        candidates.size() == 1)) targets.add(candidate);
            if (targets.isEmpty()) continue;
            String location = sourceDirectoryName(tree);
            for (File target : targets)
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                        .putString("source_tree:" + target.getName(), tree.toString())
                        .putString("source_location:" + target.getName(), location).apply();
            for (String[] entry : entries) {
                String sidecar = entry[1];
                if (sidecar == null || sidecar.equals(".") || sidecar.equals("..") ||
                        sidecar.contains("/") || sidecar.contains("\\")) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(entry[2]) ||
                        !isAdjacentSidecar(sidecar, clip[1])) continue;
                String lower = sidecar.toLowerCase(Locale.ROOT);
                Uri source = DocumentsContract.buildDocumentUriUsingTree(tree, entry[0]);
                for (File target : targets) {
                    File output = new File(target, sidecar);
                    File temporary = new File(target, sidecar + ".partial");
                    copyDocumentToFile(source, temporary, new byte[1024 * 1024]);
                    if (!temporary.renameTo(output))
                        throw new IOException("Could not save " + sidecar);
                    copied++;
                    if (lower.endsWith(".json"))
                        copied += copyReferencedSidecars(output, tree, folder, target);
                }
            }
        }
        return copied;
    }

    private int copyReferencedSidecars(File jsonFile, Uri tree, Uri sourceFolder,
            File targetFolder) throws IOException {
        if (jsonFile.length() > 8 * 1024 * 1024) return 0;
        JSONObject json;
        try {
            json = new JSONObject(new String(Files.readAllBytes(jsonFile.toPath()),
                    java.nio.charset.StandardCharsets.UTF_8));
        } catch (Exception ignored) {
            return 0; // Other JSON sidecars need not contain calibration paths.
        }
        int copied = 0;
        for (String key : new String[] {"dng_white", "dng_opcode", "dcp", "gyroflow"}) {
            String path = json.optString(key, "");
            if (path.isEmpty() || path.startsWith("/") || path.contains("\\")) continue;
            String[] parts = path.split("/");
            Uri parent = sourceFolder;
            File target = targetFolder;
            boolean valid = true;
            for (int i = 0; i < parts.length - 1; i++) {
                if (parts[i].isEmpty() || parts[i].equals(".") || parts[i].equals("..")) {
                    valid = false; break;
                }
                parent = findChild(tree, parent, parts[i],
                        DocumentsContract.Document.MIME_TYPE_DIR);
                if (parent == null) {valid = false; break;}
                target = new File(target, parts[i]);
            }
            String leaf = parts[parts.length - 1];
            if (!valid || leaf.isEmpty() || leaf.equals(".") || leaf.equals("..")) continue;
            Uri source = findChild(tree, parent, leaf, null);
            if (source == null) continue;
            if (!target.isDirectory() && !target.mkdirs())
                throw new IOException("Could not create referenced sidecar directory");
            File output = new File(target, leaf);
            copyDocumentToFile(source, output, new byte[1024 * 1024]);
            copied++;
        }
        return copied;
    }

    private String sourceDirectoryName(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        if (id.startsWith("primary:"))
            return "Internal storage/" + id.substring("primary:".length());
        Uri root = DocumentsContract.buildDocumentUriUsingTree(tree, id);
        try (Cursor cursor = getContentResolver().query(root,
                new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
        } catch (RuntimeException ignored) { }
        return id;
    }

    private File importDirectory(File imported) {
        return imported.isDirectory() ? imported : imported.getParentFile();
    }

    public String sourceDirectoryForImport(String importPath) {
        String folder = importDirectory(new File(importPath)).getName();
        String saved = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_location:" + folder, "");
        if (!saved.isEmpty()) return saved;
        // A single folder grant can cover multiple clips imported earlier.
        // Link each by the original document ID when available, otherwise by
        // an unambiguous filename match within that granted directory.
        File importFile = new File(importPath);
        Uri match = null;
        for (Map.Entry<String, ?> preference :
                getSharedPreferences(PREFS, MODE_PRIVATE).getAll().entrySet()) {
            if (!preference.getKey().startsWith("source_tree:") ||
                    !(preference.getValue() instanceof String)) continue;
            Uri tree = Uri.parse((String) preference.getValue());
            try {
                Uri root = DocumentsContract.buildDocumentUriUsingTree(tree,
                        DocumentsContract.getTreeDocumentId(tree));
                Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
                        DocumentsContract.getDocumentId(root));
                String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        DocumentsContract.Document.COLUMN_DISPLAY_NAME};
                try (Cursor cursor = getContentResolver().query(children, columns,
                        null, null, null)) {
                    if (cursor == null) continue;
                    while (cursor.moveToNext()) {
                        boolean legacyImport = getSharedPreferences(PREFS, MODE_PRIVATE)
                                .getString("source_uri:" + folder, null) == null;
                        if (!importFile.getName().equalsIgnoreCase(cursor.getString(1)) ||
                                !matchesImportedDocument(importFile.getParentFile(), tree,
                                        cursor.getString(0), legacyImport)) continue;
                        if (match != null && !match.equals(tree)) return "";
                        match = tree;
                    }
                }
            } catch (RuntimeException ignored) { }
        }
        if (match == null) return "";
        saved = sourceDirectoryName(match);
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putString("source_tree:" + folder, match.toString())
                .putString("source_location:" + folder, saved).apply();
        return saved;
    }

    private boolean isAdjacentSidecar(String sidecar, String clipName) {
        if (sidecar == null || isClipFile(sidecar)) return false;
        String lower = sidecar.toLowerCase(Locale.ROOT);
        String full = clipName.toLowerCase(Locale.ROOT);
        String stem = full.substring(0, full.lastIndexOf('.'));
        return lower.startsWith(stem + ".") || lower.startsWith(stem + "_") ||
                lower.startsWith(full + ".");
    }

    private void retireWorkingSidecars(File importFile) throws IOException {
        File folder = importFile.getParentFile();
        File[] files = folder.listFiles();
        if (files == null) return;
        File legacy = new File(folder, "legacy-sidecars");
        for (File file : files) {
            if (!file.isFile() || !isAdjacentSidecar(file.getName(),
                    importFile.getName())) continue;
            if (!legacy.isDirectory() && !legacy.mkdirs())
                throw new IOException("Could not preserve previous sidecars");
            File backup = new File(legacy, file.getName());
            if (backup.exists()) {
                if (!file.delete()) throw new IOException("Could not retire " + file.getName());
            } else if (!file.renameTo(backup)) {
                throw new IOException("Could not preserve " + file.getName());
            }
        }
    }

    // The granted source directory is authoritative. The private files are
    // only decoder shadows, refreshed before mount and on explicit refresh.
    public String syncSidecarsForImport(String importPath) {
        File imported = new File(importPath);
        File directory = importDirectory(imported);
        if (directory == null) return "";
        try {
            String value = getSharedPreferences(PREFS, MODE_PRIVATE)
                    .getString("source_tree:" + directory.getName(), null);
            // Keep the working copy until a source directory has been linked.
            if (value == null) return "";
            Uri tree = Uri.parse(value);
            Uri root = DocumentsContract.buildDocumentUriUsingTree(tree,
                    DocumentsContract.getTreeDocumentId(tree));
            Map<String, ArrayList<File>> imports = new HashMap<>();
            File[] clips = imported.isDirectory() ? imported.listFiles() : new File[] {imported};
            if (clips == null) throw new IOException("Could not read imported sequence");
            for (File clip : clips) {
                if (!clip.isFile() || !isClipFile(clip.getName())) continue;
                retireWorkingSidecars(clip);
                ArrayList<File> targets = new ArrayList<>();
                targets.add(directory);
                imports.put(clip.getName().toLowerCase(Locale.ROOT), targets);
            }
            scanSidecarFolder(tree, root, imports);
            return "";
        } catch (Exception error) {
            return "Could not refresh source sidecars: " + error.getMessage();
        }
    }

    public void registerImportedClip(String importPath, String sourceUri) {
        String folder = importDirectory(new File(importPath)).getName();
        Uri uri = Uri.parse(sourceUri);
        try {
            getContentResolver().takePersistableUriPermission(uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) { }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putString("source_uri:" + folder, sourceUri).apply();
    }

    public String sourceUriForImport(String importPath) {
        File file = new File(importPath);
        File parent = importDirectory(file);
        if (parent == null) return "";
        return getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_uri:" + parent.getName(), "");
    }

    public int openSourceUriFd(String sourceUri) {
        if (sourceUri == null || sourceUri.isEmpty()) return -1;
        try (ParcelFileDescriptor descriptor = getContentResolver()
                .openFileDescriptor(Uri.parse(sourceUri), "r")) {
            return descriptor == null ? -1 : descriptor.detachFd();
        } catch (IOException | SecurityException error) {
            return -1;
        }
    }

    private boolean matchesImportedDocument(File target, Uri tree,
            String documentId, boolean uniqueName) {
        String source = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_uri:" + target.getName(), null);
        if (source == null) return uniqueName;
        Uri uri = Uri.parse(source);
        if (!tree.getAuthority().equals(uri.getAuthority())) return uniqueName;
        try {
            return documentId.equals(DocumentsContract.getDocumentId(uri)) || uniqueName;
        } catch (RuntimeException error) {
            return uniqueName;
        }
    }

    public String writeSidecarToSource(String importPath, String name,
            String contents) {
        if (name == null || name.contains("/") || name.contains("\\"))
            return "Invalid sidecar filename";
        String folder = importDirectory(new File(importPath)).getName();
        String value = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_tree:" + folder, null);
        if (value == null) return "Scan the clip's source directory first";
        Uri tree = Uri.parse(value);
        try {
            Uri root = DocumentsContract.buildDocumentUriUsingTree(tree,
                    DocumentsContract.getTreeDocumentId(tree));
            Uri document = findChild(tree, root, name, null);
            if (document == null) document = DocumentsContract.createDocument(
                    getContentResolver(), root, "application/json", name);
            if (document == null) throw new IOException("Could not create " + name);
            try (OutputStream output = getContentResolver().openOutputStream(document, "wt")) {
                if (output == null) throw new IOException("Could not open " + name);
                output.write(contents.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
            return "";
        } catch (Exception error) {
            return "Could not write " + name + ": " + error.getMessage();
        }
    }

    public String editSidecarInExternalApp(String importPath, String name) {
        if (name == null || name.contains("/") || name.contains("\\"))
            return "Invalid sidecar filename";
        String folder = importDirectory(new File(importPath)).getName();
        String value = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_tree:" + folder, null);
        if (value == null) return "Link the clip's source directory first";
        Uri tree = Uri.parse(value);
        Uri root = DocumentsContract.buildDocumentUriUsingTree(tree,
                DocumentsContract.getTreeDocumentId(tree));
        Uri document = findChild(tree, root, name, null);
        if (document == null) return "Source JSON file not found";
        Intent edit = new Intent(Intent.ACTION_EDIT);
        edit.setDataAndType(document, "text/plain");
        edit.setClipData(ClipData.newUri(getContentResolver(), name, document));
        edit.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        runOnUiThread(() -> {
            try {
                startActivity(Intent.createChooser(edit, "Edit JSON"));
            } catch (android.content.ActivityNotFoundException error) {
                Toast.makeText(this, "No text editor can open this JSON",
                        Toast.LENGTH_LONG).show();
            }
        });
        return "";
    }

    private Uri ensureDirectory(Uri tree, Uri parent, String name) throws IOException {
        Uri directory = findChild(tree, parent, name, DocumentsContract.Document.MIME_TYPE_DIR);
        if (directory == null) directory = DocumentsContract.createDocument(
                getContentResolver(), parent, DocumentsContract.Document.MIME_TYPE_DIR, name);
        if (directory == null) throw new IOException("Could not create " + name + " folder");
        return directory;
    }

    private Uri findChild(Uri tree, Uri parent, String name, String mime) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(
                tree, DocumentsContract.getDocumentId(parent));
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor cursor = getContentResolver().query(children, columns,
                null, null, null)) {
            if (cursor == null) return null;
            while (cursor.moveToNext()) {
                if (name.equals(cursor.getString(1)) &&
                        (mime == null || mime.equals(cursor.getString(2))))
                    return DocumentsContract.buildDocumentUriUsingTree(tree,
                            cursor.getString(0));
            }
        }
        return null;
    }

    private static void copyStream(InputStream input, OutputStream output, byte[] buffer)
            throws IOException {
        int length;
        while ((length = input.read(buffer)) != -1) output.write(buffer, 0, length);
    }

    private void copyDocumentToFile(Uri source, File target, byte[] buffer) throws IOException {
        try (InputStream input = getContentResolver().openInputStream(source);
             OutputStream output = new FileOutputStream(target)) {
            if (input == null) throw new IOException("Could not read " + target.getName());
            copyStream(input, output, buffer);
        }
    }

    private File copyDngSequenceFolder(Uri tree) throws IOException {
        Uri root = DocumentsContract.buildDocumentUriUsingTree(tree,
                DocumentsContract.getTreeDocumentId(tree));
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
                DocumentsContract.getDocumentId(root));
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE};
        ArrayList<String[]> entries = new ArrayList<>();
        try (Cursor cursor = getContentResolver().query(children, columns,
                null, null, null)) {
            if (cursor == null) throw new IOException("Could not read the selected folder");
            while (cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2)) ||
                        name == null || name.contains("/") || name.contains("\\")) continue;
                entries.add(new String[] {cursor.getString(0), name});
            }
        }
        ArrayList<String[]> frames = new ArrayList<>();
        for (String[] entry : entries)
            if (entry[1].toLowerCase(Locale.ROOT).endsWith(".dng")) frames.add(entry);
        Collections.sort(frames, Comparator.comparing(entry -> entry[1].toLowerCase(Locale.ROOT)));
        if (frames.size() < 2)
            throw new IOException("The selected folder must contain a DNG sequence");

        UriPermission selected = null;
        for (UriPermission permission : getContentResolver().getPersistedUriPermissions())
            if (tree.equals(permission.getUri())) { selected = permission; break; }
        if (selected == null || !selected.isReadPermission())
            throw new IOException("The selected folder is no longer accessible");

        String folderToken = UUID.randomUUID().toString();
        File destination = new File(new File(getFilesDir(), "imports"), folderToken);
        if (!destination.mkdirs()) throw new IOException("Could not create the import folder");
        byte[] buffer = new byte[1024 * 1024];
        for (String[] entry : entries) {
            Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, entry[0]);
            File target = new File(destination, entry[1]);
            copyDocumentToFile(document, target, buffer);
        }
        String location = sourceDirectoryName(tree);
        getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                .putString("source_tree:" + folderToken, tree.toString())
                .putString("source_uri:" + folderToken, tree.toString())
                .putString("source_location:" + folderToken, location).apply();
        return destination;
    }

}
