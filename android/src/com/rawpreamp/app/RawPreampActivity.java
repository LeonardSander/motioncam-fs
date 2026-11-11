package com.rawpreamp.app;

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
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
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
import java.io.IOException;
import java.io.InputStream;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.nio.ByteBuffer;
import java.nio.channels.Channels;
import java.nio.channels.WritableByteChannel;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Set;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

public class RawPreampActivity extends QtActivity {
    private static final int CHOOSE_SIDECAR_DIRECTORY = 6302;
    private static final int CHOOSE_FINALIZE_DIRECTORY = 6303;
    private static final int CHOOSE_SEQUENCE_FOLDER = 6305;
    private static final String PREFS = "rawpreamp_documents";
    private static final String FINALIZE_TREE_KEY = "finalize_tree_uri";
    private static final String CUSTOM_FINALIZE_TREE_KEY = "custom_finalize_tree_uri";
    private String pendingSidecarImportRoot;
    private final ArrayList<String> pendingSidecarPaths = new ArrayList<>();
    private final java.util.HashSet<String> scannedSidecarTrees = new java.util.HashSet<>();
    private int pendingSidecarIndex;
    private int indexedSidecars;
    private volatile String sidecarScanStatus = "idle";
    private PowerManager.WakeLock finalizeWakeLock;
    private boolean pendingFinalizeTree;
    private volatile String finalizeTreeSelectionStatus = "idle";
    private Uri activeFinalizeFolder;
    private Uri activeFinalizeTree;
    private boolean activeFinalizeOwnsFolder;
    private final ArrayList<Uri> activeFinalizeDocuments = new ArrayList<>();
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

    public void setRawPreampLandscapeOnly(boolean landscapeOnly) {
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
                        PowerManager.PARTIAL_WAKE_LOCK, "RawPreamp:Finalize");
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
            String mode, String importPath, boolean createSubfolder) {
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
                Uri sourceDocument=sourceDocumentUri(sourceUri);
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
                    output = ensureDirectory(tree, root, "RawPreamp");
                }
            }
            String name = requestedFolderName == null || requestedFolderName.isEmpty()
                    ? "Finalized Clip" : requestedFolderName;
            Uri clip = null;
            String outputName = name;
            Set<String> existingNames = createSubfolder ? null : childNames(tree, output);
            for (int suffix = 1; suffix < 10000 && clip == null; suffix++) {
                String candidate = suffix == 1 ? name : name + " (" + suffix + ")";
                if (createSubfolder) {
                    if (findChild(tree, output, candidate,
                            DocumentsContract.Document.MIME_TYPE_DIR) == null)
                        clip = DocumentsContract.createDocument(getContentResolver(), output,
                                DocumentsContract.Document.MIME_TYPE_DIR, candidate);
                } else if (nativeOutputNameAvailable(existingNames, candidate)) {
                    clip = output;
                }
                if (clip != null) outputName = candidate;
            }
            if (clip == null) throw new IOException("Could not reserve a unique output name");
            activeFinalizeFolder = clip;
            activeFinalizeTree = tree;
            activeFinalizeOwnsFolder = createSubfolder;
            activeFinalizeDocuments.clear();
            return "ready:" + (createSubfolder ? displayNameForUri(clip.toString()) : outputName);
        } catch (Exception error) {
            return "error:" + error.getMessage();
        }
    }

    private Set<String> childNames(Uri tree, Uri folder) throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
                DocumentsContract.getDocumentId(folder));
        Set<String> names = new HashSet<>();
        try (Cursor cursor = getContentResolver().query(children,
                new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            if (cursor == null) throw new IOException("Could not list the output directory");
            while (cursor.moveToNext()) names.add(cursor.getString(0));
        }
        return names;
    }

    private boolean nativeOutputNameAvailable(Set<String> names, String base) {
        // Reserve one basename for the complete video/metadata set so a repeat
        // export never overwrites earlier files or splits matching sidecars.
        for (String extension : new String[]{".mov", ".mp4", ".json", "_gyroflow.json",
                ".gcsv", ".jsonl", ".wav", ".flac", ".opus", ".aac", ".mka"}) {
            if (names.contains(base + extension)) return false;
        }
        return true;
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

    // Pick a real device encoder, never Android's software codec fallback.
    // 10-bit input uses the direct NDK P010 adapter; 8-bit uses FFmpeg.
    public String selectNativeHardwareEncoder(String mime, int width, int height,
            double fps, int bitrate, int bitDepth) {
        if (width <= 0 || height <= 0 || width % 16 != 0 || height % 16 != 0)
            return ""; // Preserve exact dimensions and use a consistent aligned input contract.
        if (bitDepth != 8 && bitDepth != 10) return "";
        if (bitDepth == 10 && Build.VERSION.SDK_INT < 33) return "";
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            if (!info.isEncoder()) continue;
            String name = info.getName();
            if (Build.VERSION.SDK_INT >= 29) {
                if (!info.isHardwareAccelerated()) continue;
            } else if (name.startsWith("OMX.google.") || name.startsWith("c2.android.")) {
                continue;
            }
            try {
                MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(mime);
                MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
                if (video == null || !video.areSizeAndRateSupported(width, height, fps)
                        || !video.getBitrateRange().contains(bitrate)) continue;
                int profile = 0;
                if (bitDepth == 10) {
                    profile = "video/avc".equals(mime) ? MediaCodecInfo.CodecProfileLevel.AVCProfileHigh10
                            : "video/hevc".equals(mime) ? MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10
                            : MediaCodecInfo.CodecProfileLevel.AV1ProfileMain10;
                    boolean supported = false;
                    for (MediaCodecInfo.CodecProfileLevel level : caps.profileLevels)
                        if (level.profile == profile) supported = true;
                    if (!supported) continue;
                }
                String format = null;
                for (int color : caps.colorFormats) {
                    if (bitDepth == 10) {
                        if (color == MediaCodecInfo.CodecCapabilities.COLOR_FormatYUVP010) {
                            format = "p010le";
                            break;
                        }
                    } else {
                        if (color == MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar) {
                            format = "nv12";
                            break;
                        }
                        if (color == MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar)
                            format = "yuv420p";
                    }
                }
                if (format == null) continue;
                MediaCodecInfo.EncoderCapabilities encoder = caps.getEncoderCapabilities();
                String mode;
                if (encoder.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR))
                    mode = "vbr";
                else if (encoder.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                    mode = "cbr";
                else continue;
                return name + "|" + format + "|" + mode + "|" + profile;
            } catch (IllegalArgumentException ignored) {
                // The codec does not provide this MIME type.
            }
        }
        return "";
    }

    public synchronized void writeFinalizeOutputEntry(String name, ByteBuffer bytes)
            throws IOException {
        writeFinalizeEntry(name, output -> {
            WritableByteChannel channel = Channels.newChannel(output);
            while (bytes.hasRemaining()) channel.write(bytes);
        });
    }

    public synchronized void writeFinalizeOutputFile(String name, String path) throws IOException {
        writeFinalizeEntry(name, output -> {
            try (InputStream input = new FileInputStream(path)) {
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            }
        });
    }

    private interface FinalizeEntryWriter {
        void write(OutputStream output) throws IOException;
    }

    private void writeFinalizeEntry(String name, FinalizeEntryWriter writer) throws IOException {
        if (activeFinalizeFolder == null) throw new IOException("Output folder is not ready");
        if (name == null || name.isEmpty() || name.contains("/") || name.contains("\\"))
            throw new IOException("Invalid output filename");
        if (!activeFinalizeOwnsFolder && findChild(activeFinalizeTree, activeFinalizeFolder, name, null) != null)
            throw new IOException("An output file already exists: " + name);
        // Publish each file only after its complete contents have been written.
        Uri document = DocumentsContract.createDocument(getContentResolver(),
                activeFinalizeFolder, "application/octet-stream", name + ".partial-" + UUID.randomUUID());
        if (document == null) throw new IOException("Could not create " + name);
        if (!activeFinalizeOwnsFolder) activeFinalizeDocuments.add(document);
        try {
            try (OutputStream output = getContentResolver().openOutputStream(document, "w")) {
                if (output == null) throw new IOException("Could not open " + name);
                writer.write(output);
                output.flush();
            }
            Uri renamed = DocumentsContract.renameDocument(getContentResolver(), document, name);
            if (renamed == null) throw new IOException("Could not finish " + name);
            if (!activeFinalizeOwnsFolder) {
                activeFinalizeDocuments.remove(document);
                activeFinalizeDocuments.add(renamed);
            }
            document = renamed;
            if (!name.equals(displayNameForUri(document.toString())))
                throw new IOException("The output provider changed the finalized filename: " + name);
        } catch (IOException | RuntimeException error) {
            try {
                if (DocumentsContract.deleteDocument(getContentResolver(), document))
                    activeFinalizeDocuments.remove(document);
            } catch (Exception ignored) { }
            throw error;
        }
    }

    public synchronized void finishFinalizeOutput() {
        activeFinalizeFolder = null;
        activeFinalizeTree = null;
        activeFinalizeOwnsFolder = false;
        activeFinalizeDocuments.clear();
        Toast.makeText(this, "Finalized files saved to the selected folder",
                Toast.LENGTH_LONG).show();
    }

    public synchronized void cancelFinalizeOutput() {
        Uri folder = activeFinalizeFolder;
        boolean owned = activeFinalizeOwnsFolder;
        activeFinalizeFolder = null;
        activeFinalizeTree = null;
        activeFinalizeOwnsFolder = false;
        if (owned && folder != null) {
            try { DocumentsContract.deleteDocument(getContentResolver(), folder); }
            catch (Exception ignored) { }
        } else {
            // Direct exports share a user-selected directory. Never delete it:
            // roll back only documents created during this export.
            for (Uri document : activeFinalizeDocuments) {
                try { DocumentsContract.deleteDocument(getContentResolver(), document); }
                catch (Exception ignored) { }
            }
        }
        activeFinalizeDocuments.clear();
    }

    public String sidecarScanStatus() { return sidecarScanStatus; }

    // Reuse existing grants, then ask for the remaining source folders in turn.
    public void scanDirectoryForSidecars(String importRoot, String importPaths) {
        sidecarScanStatus = "scanning";
        runOnUiThread(() -> {
            if (pendingSidecarImportRoot != null) return;
            pendingSidecarImportRoot = importRoot;
            pendingSidecarPaths.clear();
            for (String path : importPaths.split("\n"))
                if (!path.isEmpty()) pendingSidecarPaths.add(path);
            beginSourceImportBatch();
            scannedSidecarTrees.clear();
            pendingSidecarIndex = 0;
            indexedSidecars = 0;
            sidecarScanStatus = "scanning";
            new Thread(this::scanNextSidecarDirectory, "RawPreamp-sidecar-scan").start();
        });
    }

    private void scanNextSidecarDirectory() {
        String result;
        try {
            while (pendingSidecarIndex < pendingSidecarPaths.size()) {
                String path = pendingSidecarPaths.get(pendingSidecarIndex++);
                Uri tree = grantedSourceTree(path);
                boolean granted = tree != null;
                if (granted) {
                    if (scannedSidecarTrees.add(tree.toString()))
                        indexedSidecars += scanSidecarTree(tree);
                    continue;
                }
                Uri initial = initialSourceDirectory(path);
                sidecarScanStatus = "choosing";
                runOnUiThread(() -> {
                    Toast.makeText(this, "Choose the source folder or a parent folder covering several clips for " +
                            new File(path).getName(), Toast.LENGTH_LONG).show();
                    requestTreeSelection(CHOOSE_SIDECAR_DIRECTORY, initial);
                });
                return;
            }
            result = "done:" + indexedSidecars;
        } catch (Exception error) {
            result = "error:" + error.getMessage();
        }
        pendingSidecarImportRoot = null;
        endSourceImportBatch();
        sidecarScanStatus = result;
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
            if (slash <= 0) return uri;
            return DocumentsContract.buildDocumentUri(uri.getAuthority(),
                    id.substring(0, slash));
        } catch (RuntimeException error) {
            return uri;
        }
    }

    // Qt may return a display-decoded URL. Re-encode each path segment without
    // turning the slashes inside a document ID into URI path separators.
    private Uri sourceDocumentUri(String value) {
        Uri uri = Uri.parse(value);
        Uri.Builder builder = uri.buildUpon().path(null);
        for (String segment : uri.getPathSegments()) builder.appendPath(segment);
        return builder.build();
    }

    // The document URI can contain an opaque ID instead of the filename.
    // Query the provider for the name shown in Android's file picker.
    public String displayNameForUri(String sourceUri) {
        Uri uri = sourceDocumentUri(sourceUri);
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
        Uri uri = sourceDocumentUri(sourceUri);
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
            sequenceFolderImportStatus = "indexing";
            beginFinalizeWork();
            new Thread(() -> {
                try {
                    File imported = indexDngSequenceFolder(tree);
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
            }, "RawPreamp-dng-sequence-import").start();
            return;
        }
        if (requestCode == CHOOSE_SIDECAR_DIRECTORY) {
            if (resultCode != Activity.RESULT_OK || data == null ||
                    data.getData() == null || pendingSidecarImportRoot == null) {
                pendingSidecarImportRoot = null;
                endSourceImportBatch();
                sidecarScanStatus = "cancelled";
                return;
            }
            Uri tree = data.getData();
            try {
                getContentResolver().takePersistableUriPermission(tree,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (SecurityException error) {
                pendingSidecarImportRoot = null;
                sidecarScanStatus = "error:Could not read the selected directory";
                endSourceImportBatch();
                return;
            }
            sourceReadGrants = getContentResolver().getPersistedUriPermissions();
            sidecarScanStatus = "scanning";
            new Thread(() -> {
                try {
                    if (scannedSidecarTrees.add(tree.toString()))
                        indexedSidecars += scanSidecarTree(tree);
                    scanNextSidecarDirectory();
                } catch (Exception error) {
                    pendingSidecarImportRoot = null;
                    sidecarScanStatus = "error:" + error.getMessage();
                    endSourceImportBatch();
                }
            }, "RawPreamp-sidecar-scan").start();
            return;
        }
    }

    private boolean isClipFile(String name) {
        if (name == null) return false;
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".mcraw") || lower.endsWith(".unspektra") || lower.endsWith(".mcpro") ||
                lower.endsWith(".zip") || lower.endsWith(".7z") || lower.endsWith(".dng") || lower.endsWith(".mov") ||
                lower.endsWith(".mp4") || lower.endsWith(".mkv");
    }

    private int scanSidecarTree(Uri tree) throws IOException {
        int indexed = 0;
        Map<String, Map<String, ArrayList<File>>> folders = new HashMap<>();
        Map<String, Uri> folderUris = new HashMap<>();
        for (String path : pendingSidecarPaths) {
            File imported = new File(path);
            Uri grant = grantedSourceTree(path);
            // Includes all clips covered by a newly selected parent grant.
            if (grant != null && !grant.equals(tree)) continue;
            // Unlinked legacy imports and providers with opaque IDs are matched
            // by the single root listing below, never by querying each clip.
            if (grant != null) sourceDirectoryForImport(path);
            if (isSequenceImport(imported)) {
                if (grant == null) continue;
                indexDngSequenceFolder(tree, imported);
                File[] entries = imported.listFiles();
                if (entries != null)
                    for (File entry : entries)
                        if (entry.isFile() && !isClipFile(entry.getName())) indexed++;
                continue;
            }
            Uri folder = grant == null ? DocumentsContract.buildDocumentUriUsingTree(tree,
                    DocumentsContract.getTreeDocumentId(tree)) : sourceFolder(tree, imported);
            String key = folder.toString();
            folderUris.put(key, folder);
            folders.computeIfAbsent(key, ignored -> new HashMap<>())
                    .computeIfAbsent(imported.getName().toLowerCase(Locale.ROOT),
                            ignored -> new ArrayList<>()).add(importDirectory(imported));
        }
        for (Map.Entry<String, Map<String, ArrayList<File>>> folder : folders.entrySet())
            indexed += scanSidecarFolder(tree, folderUris.get(folder.getKey()), folder.getValue());
        return indexed;
    }

    private int scanSidecarFolder(Uri tree, Uri folder,
            Map<String, ArrayList<File>> imports) throws IOException {
        ArrayList<String[]> entries = listSourceFolder(tree, folder);
        ArrayList<String[]> sidecars = new ArrayList<>();
        for (String[] entry : entries)
            if (!DocumentsContract.Document.MIME_TYPE_DIR.equals(entry[2]) &&
                    !isClipFile(entry[1])) sidecars.add(entry);
        android.content.SharedPreferences.Editor editor = getSharedPreferences(PREFS, MODE_PRIVATE).edit();
        int indexed = 0;
        for (String[] clip : entries) {
            if (!isClipFile(clip[1])) continue;
            ArrayList<File> candidates = imports.get(clip[1].toLowerCase(Locale.ROOT));
            if (candidates == null) continue;
            ArrayList<File> targets = new ArrayList<>();
            for (File candidate : candidates)
                if (matchesImportedDocument(candidate, tree, clip[0],
                        candidates.size() == 1)) targets.add(candidate);
            if (targets.isEmpty()) continue;
            for (File target : targets) retireWorkingSidecars(new File(target, clip[1]));
            String location = sourceDirectoryName(tree, folder);
            for (File target : targets)
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                        .putString("source_tree:" + target.getName(), tree.toString())
                        .putString("source_location:" + target.getName(), location).apply();
            for (String[] entry : sidecars) {
                String sidecar = entry[1];
                if (sidecar == null || sidecar.equals(".") || sidecar.equals("..") ||
                        sidecar.contains("/") || sidecar.contains("\\")) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(entry[2]) ||
                        !isAdjacentSidecar(sidecar, clip[1])) continue;
                String lower = sidecar.toLowerCase(Locale.ROOT);
                Uri source = DocumentsContract.buildDocumentUriUsingTree(tree, entry[0]);
                for (File target : targets) {
                    File output = new File(target, sidecar);
                    bindDocument(output, source, Long.parseLong(entry[3]),
                            Long.parseLong(entry[4]), editor);
                    indexed++;
                    if (lower.endsWith(".json")) {
                        // Referenced sidecars resolve this binding immediately.
                        editor.apply();
                        indexed += indexReferencedSidecars(output, tree, folder, target);
                    }
                }
            }
        }
        editor.apply();
        return indexed;
    }

    private int indexReferencedSidecars(File jsonFile, Uri tree, Uri sourceFolder,
            File targetFolder) throws IOException {
        if (jsonFile.length() > 8 * 1024 * 1024) return 0;
        JSONObject json;
        try {
            Uri uri = Uri.parse(sourceUriForImport(jsonFile.getAbsolutePath()));
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) return 0;
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0) {
                    if (bytes.size() + count > 8 * 1024 * 1024) return 0;
                    bytes.write(buffer, 0, count);
                }
                json = new JSONObject(bytes.toString("UTF-8"));
            }
        } catch (Exception ignored) {
            return 0; // Other JSON sidecars need not contain calibration paths.
        }
        int indexed = 0;
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
            bindDocument(output, source);
            indexed++;
        }
        return indexed;
    }

    private String sourceDirectoryName(Uri tree) {
        return sourceDirectoryName(tree, DocumentsContract.buildDocumentUriUsingTree(tree,
                DocumentsContract.getTreeDocumentId(tree)));
    }

    private String sourceDirectoryName(Uri tree, Uri folder) {
        String id = DocumentsContract.getDocumentId(folder);
        if (id.startsWith("primary:"))
            return "Internal storage/" + id.substring("primary:".length());
        try (Cursor cursor = getContentResolver().query(folder,
                new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) return cursor.getString(0);
        } catch (RuntimeException ignored) { }
        return id;
    }

    private boolean isSequenceImport(File imported) {
        if (imported.isDirectory()) return true;
        // A persisted sequence root remains a directory import even when its
        // private filename index is missing and must be rebuilt on restore.
        String token = sourceKey(imported);
        if (token == null || token.isEmpty() || token.contains("/")) return false;
        android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String source = prefs.getString("source_uri:" + token, "");
        return prefs.getBoolean("source_is_folder:" + token,
                !source.isEmpty() && DocumentsContract.isTreeUri(Uri.parse(source)));
    }

    private File importDirectory(File imported) {
        return isSequenceImport(imported) ? imported : imported.getParentFile();
    }

    private String documentIdentityId(Uri uri) {
        try { return DocumentsContract.getDocumentId(uri); }
        catch (IllegalArgumentException ignored) { return DocumentsContract.getTreeDocumentId(uri); }
    }

    // Document IDs let us reuse a parent grant without listing every saved
    // source folder. Never infer access by filename for document-backed imports.
    private Uri grantedSourceTree(String importPath) {
        File directory = importDirectory(new File(importPath));
        if (directory == null) return null;
        String token = directory.getName();
        String source = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_uri:" + token, "");
        String saved = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_tree:" + token, "");
        Uri original = source.isEmpty() ? null : Uri.parse(source);
        String originalId = null;
        if (original != null) {
            try { originalId = documentIdentityId(original); }
            catch (RuntimeException ignored) { return null; }
        }
        Uri best = null;
        Uri savedGrant = null;
        int bestLength = -1;
        java.util.List<UriPermission> grants = sourceReadGrants;
        if (grants == null) grants = getContentResolver().getPersistedUriPermissions();
        for (UriPermission permission : grants) {
            Uri tree = permission.getUri();
            if (!permission.isReadPermission() || !DocumentsContract.isTreeUri(tree)) continue;
            try {
                if (original == null) {
                    if (tree.toString().equals(saved)) return tree;
                    continue;
                }
                if (!tree.getAuthority().equals(original.getAuthority())) continue;
                String id = originalId;
                String root = DocumentsContract.getTreeDocumentId(tree);
                // Explicit associations also support providers with opaque IDs.
                if (tree.toString().equals(saved)) savedGrant = tree;
                if ((id.equals(root) || id.startsWith(root.endsWith(":") ? root : root + "/"))
                        && root.length() > bestLength) {
                    best = tree;
                    bestLength = root.length();
                }
            } catch (RuntimeException ignored) { }
        }
        return best != null ? best : savedGrant;
    }

    private Uri sourceFolder(Uri tree, File imported) {
        String source = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_uri:" + importDirectory(imported).getName(), "");
        String id = DocumentsContract.getTreeDocumentId(tree);
        if (!source.isEmpty()) {
            Uri original = Uri.parse(source);
            id = documentIdentityId(original);
            if (!isSequenceImport(imported)) {
                int slash = id.lastIndexOf('/');
                // ExternalStorageProvider puts root-level files after a colon.
                int end = slash >= 0 ? slash : "com.android.externalstorage.documents".equals(
                        tree.getAuthority()) ? id.indexOf(':') : -1;
                if (end >= 0) id = id.substring(0, slash >= 0 ? end : end + 1);
                else id = DocumentsContract.getTreeDocumentId(tree);
            }
        }
        return DocumentsContract.buildDocumentUriUsingTree(tree, id);
    }

    public String sourceDirectoryForImport(String importPath) {
        Uri tree = grantedSourceTree(importPath);
        if (tree == null) return "";
        String token = importDirectory(new File(importPath)).getName();
        String value = tree.toString();
        String savedTree = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_tree:" + token, "");
        String location = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_location:" + token, "");
        if (!value.equals(savedTree) || location.isEmpty()) {
            location = sourceDirectoryName(tree, sourceFolder(tree, new File(importPath)));
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putString("source_tree:" + token, value)
                    .putString("source_location:" + token, location).apply();
        }
        return location;
    }

    // Listings live only for one import/scan operation. A later import or
    // explicit refresh always observes additions, edits and removals.
    private final Map<String, ArrayList<String[]>> sourceListings = new HashMap<>();
    private boolean sourceImportBatch;
    private volatile java.util.List<UriPermission> sourceReadGrants;

    public synchronized void beginSourceImportBatch() {
        sourceListings.clear();
        sourceReadGrants = getContentResolver().getPersistedUriPermissions();
        sourceImportBatch = true;
    }

    public synchronized void endSourceImportBatch() {
        sourceImportBatch = false;
        sourceReadGrants = null;
        sourceListings.clear();
    }

    private synchronized ArrayList<String[]> listSourceFolder(Uri tree, Uri folder)
            throws IOException {
        String key = folder.toString();
        if (sourceImportBatch && sourceListings.containsKey(key)) return sourceListings.get(key);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
                DocumentsContract.getDocumentId(folder));
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                OpenableColumns.SIZE, DocumentsContract.Document.COLUMN_LAST_MODIFIED};
        ArrayList<String[]> entries = new ArrayList<>();
        try (Cursor cursor = getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null) throw new IOException("Could not list source directory");
            while (cursor.moveToNext()) entries.add(new String[] {cursor.getString(0),
                    cursor.getString(1), cursor.getString(2),
                    cursor.isNull(3) ? "-1" : cursor.getString(3),
                    cursor.isNull(4) ? "0" : cursor.getString(4)});
        }
        if (sourceImportBatch) sourceListings.put(key, entries);
        return entries;
    }

    private boolean isAdjacentSidecar(String sidecar, String clipName) {
        if (sidecar == null || isClipFile(sidecar)) return false;
        String lower = sidecar.toLowerCase(Locale.ROOT);
        String full = clipName.toLowerCase(Locale.ROOT);
        int dot = full.lastIndexOf('.');
        String stem = dot < 0 ? full : full.substring(0, dot);
        return lower.startsWith(stem + ".") || lower.startsWith(stem + "_") ||
                lower.startsWith(full + ".");
    }

    private void retireWorkingSidecars(File importFile) throws IOException {
        File[] files = importFile.getParentFile().listFiles();
        if (files == null) return;
        for (File file : files) {
            if (file.isFile() && isAdjacentSidecar(file.getName(), importFile.getName())) {
                String key = sourceKey(file);
                Files.deleteIfExists(file.toPath());
                if (key != null) getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                        .remove("document_uri:" + key).remove("document_size:" + key)
                        .remove("document_modified:" + key).apply();
            }
        }
    }

    // The granted source directory is authoritative; private entries only index URIs.
    public String syncSidecarsForImport(String importPath) {
        File imported = new File(importPath);
        File directory = importDirectory(imported);
        if (directory == null) return "";
        try {
            sourceDirectoryForImport(importPath);
            Uri tree = grantedSourceTree(importPath);
            // A single-document grant cannot enumerate adjacent sidecars.
            if (tree == null) return "";
            // Reindex sequences once, including sidecars, rather than rescanning
            // their entire directory for every individual frame.
            if (isSequenceImport(imported)) {
                indexDngSequenceFolder(tree, directory);
                return "";
            }
            Uri root = sourceFolder(tree, imported);
            Map<String, ArrayList<File>> imports = new HashMap<>();
            File[] clips = imported.isDirectory() ? imported.listFiles() : new File[] {imported};
            if (clips == null) throw new IOException("Could not read imported sequence");
            for (File clip : clips) {
                if (!clip.isFile() || !isClipFile(clip.getName())) continue;
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

    private String sourceKey(File file) {
        java.nio.file.Path root = new File(getFilesDir(), "imports").toPath().toAbsolutePath().normalize();
        java.nio.file.Path path = file.toPath().toAbsolutePath().normalize();
        return path.startsWith(root) ? root.relativize(path).toString() : null;
    }

    private void bindDocument(File target, Uri source, long size, long modified) throws IOException {
        android.content.SharedPreferences.Editor editor = getSharedPreferences(PREFS, MODE_PRIVATE).edit();
        bindDocument(target, source, size, modified, editor);
        editor.apply();
    }

    private void bindDocument(File target, Uri source, long size, long modified,
            android.content.SharedPreferences.Editor editor) throws IOException {
        String key = sourceKey(target);
        if (key == null) throw new IOException("Invalid document index path");
        if (!target.getParentFile().isDirectory() && !target.getParentFile().mkdirs())
            throw new IOException("Could not create document index");
        // Index files contain no document bytes. Remove old links first so
        // replacing a legacy entry can never truncate the original document.
        if (Files.isSymbolicLink(target.toPath()) || target.length() != 0)
            Files.deleteIfExists(target.toPath());
        if (!target.exists() && !target.createNewFile())
            throw new IOException("Could not create document index entry");
        editor.putString("document_uri:" + key, source.toString())
                .putLong("document_size:" + key, size)
                .putLong("document_modified:" + key, modified);
    }

    private void bindDocument(File target, Uri source) throws IOException {
        long size = -1, modified = 0;
        try (Cursor cursor = getContentResolver().query(source,
                new String[] {OpenableColumns.SIZE, DocumentsContract.Document.COLUMN_LAST_MODIFIED},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                if (!cursor.isNull(0)) size = cursor.getLong(0);
                if (!cursor.isNull(1)) modified = cursor.getLong(1);
            }
        }
        bindDocument(target, source, size, modified);
    }

    public String registerImportedClip(String importPath, String sourceUri) {
        File imported = new File(importPath);
        String folder = importDirectory(imported).getName();
        Uri uri = sourceDocumentUri(sourceUri);
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) { }
        try {
            bindDocument(imported, uri);
            getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                    .putString("source_uri:" + folder, uri.toString())
                    .putBoolean("source_is_folder:" + folder, false).apply();
            return "";
        } catch (Exception error) {
            return "Could not access source document: " + error.getMessage();
        }
    }

    private String sourceIdentity(String value) {
        if (!value.startsWith("content:")) {
            String folder = importDirectory(new File(value)).getName();
            value = getSharedPreferences(PREFS, MODE_PRIVATE).getString("source_uri:" + folder, "");
        }
        if (value.isEmpty()) return "";
        Uri uri = sourceDocumentUri(value);
        try {
            String id = documentIdentityId(uri);
            return uri.getAuthority() + ":" + id;
        } catch (RuntimeException error) {
            return uri.toString();
        }
    }

    public boolean sameImportedSource(String first, String second) {
        String identity = sourceIdentity(first);
        return !identity.isEmpty() && identity.equals(sourceIdentity(second));
    }

    public String importSourceLocation(String importPath) {
        File imported = new File(importPath);
        String folder = importDirectory(imported).getName();
        String location = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_location:" + folder, "");
        if (!location.isEmpty()) return location;
        String source = getSharedPreferences(PREFS, MODE_PRIVATE)
                .getString("source_uri:" + folder, "");
        return source.isEmpty() ? "Source document" : sourceDirectoryForUri(source);
    }

    public String importDisplayName(String importPath) {
        File imported = new File(importPath);
        if (!imported.isDirectory()) return imported.getName();
        String location = importSourceLocation(importPath);
        int slash = location.lastIndexOf('/');
        return slash < 0 ? location : location.substring(slash + 1);
    }

    // One native lookup resolves a document and its descriptor-cache revision.
    // The URI is encoded, so newlines unambiguously separate these fields.
    public String sourceDescriptorInfoForImport(String importPath) {
        String key = sourceKey(new File(importPath));
        if (key == null) return "";
        android.content.SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String uri = prefs.getString("document_uri:" + key, "");
        if (uri.isEmpty()) return "";
        return uri + "\n" + prefs.getLong("document_size:" + key, -1) + "\n" +
                prefs.getLong("document_modified:" + key, 0);
    }

    public String sourceUriForImport(String importPath) {
        File file = new File(importPath);
        String key = sourceKey(file);
        if (key == null) return "";
        return getSharedPreferences(PREFS, MODE_PRIVATE).getString("document_uri:" + key, "");
    }

    public long sourceSizeForImport(String importPath) {
        String key = sourceKey(new File(importPath));
        return key == null ? -1 : getSharedPreferences(PREFS, MODE_PRIVATE)
                .getLong("document_size:" + key, -1);
    }

    public String sourceRevisionForImport(String importPath) {
        String key = sourceKey(new File(importPath));
        if (key == null) return "";
        String uri = sourceUriForImport(importPath);
        if (uri.isEmpty()) return "";
        return uri + ":" + getSharedPreferences(PREFS, MODE_PRIVATE)
                .getLong("document_modified:" + key, 0);
    }

    public String refreshImportedSource(String importPath) {
        File file = new File(importPath);
        String key = sourceKey(file);
        if (key == null || key.isEmpty()) return "";
        String folder = key.split("/", 2)[0];
        String uri = getSharedPreferences(PREFS, MODE_PRIVATE).getString("source_uri:" + folder, "");
        if (uri.isEmpty()) return "";
        try {
            if (getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(
                    "source_is_folder:" + folder, DocumentsContract.isTreeUri(sourceDocumentUri(uri)))) {
                Uri tree = grantedSourceTree(importPath);
                if (tree == null) throw new IOException("The source folder is no longer accessible");
                indexDngSequenceFolder(tree, new File(new File(getFilesDir(), "imports"), folder));
            } else {
                bindDocument(file, sourceDocumentUri(uri));
            }
            return "";
        } catch (Exception error) {
            return "Could not access source: " + error.getMessage();
        }
    }

    public int openSourceUriFd(String sourceUri) {
        if (sourceUri == null || sourceUri.isEmpty()) return -1;
        try (ParcelFileDescriptor descriptor = getContentResolver()
                .openFileDescriptor(sourceDocumentUri(sourceUri), "r")) {
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
        if (!tree.getAuthority().equals(uri.getAuthority())) return false;
        try {
            String originalId = DocumentsContract.getDocumentId(uri);
            if (getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(
                    "source_is_folder:" + target.getName(), false))
                return originalId.equals(DocumentsContract.getTreeDocumentId(tree));
            return documentId.equals(originalId);
        } catch (RuntimeException error) {
            return false;
        }
    }

    public String writeSidecarToSource(String importPath, String name,
            String contents) {
        if (name == null || name.contains("/") || name.contains("\\"))
            return "Invalid sidecar filename";
        Uri tree = grantedSourceTree(importPath);
        if (tree == null) return "Grant access to the clip's source folder first";
        try {
            Uri root = sourceFolder(tree, new File(importPath));
            Uri document = findChild(tree, root, name, null);
            if (document == null) document = DocumentsContract.createDocument(
                    getContentResolver(), root, "application/json", name);
            if (document == null) throw new IOException("Could not create " + name);
            try (OutputStream output = getContentResolver().openOutputStream(document, "wt")) {
                if (output == null) throw new IOException("Could not open " + name);
                output.write(contents.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            }
            bindDocument(new File(importDirectory(new File(importPath)), name), document);
            return "";
        } catch (Exception error) {
            return "Could not write " + name + ": " + error.getMessage();
        }
    }

    public String editSidecarInExternalApp(String importPath, String name) {
        if (name == null || name.contains("/") || name.contains("\\"))
            return "Invalid sidecar filename";
        Uri tree = grantedSourceTree(importPath);
        if (tree == null) return "Grant access to the clip's source folder first";
        Uri root = sourceFolder(tree, new File(importPath));
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

    private File indexDngSequenceFolder(Uri tree) throws IOException {
        for (Map.Entry<String, ?> entry : getSharedPreferences(PREFS, MODE_PRIVATE).getAll().entrySet()) {
            if (!entry.getKey().startsWith("source_uri:") || !(entry.getValue() instanceof String)) continue;
            String token = entry.getKey().substring("source_uri:".length());
            if (getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean("source_is_folder:" + token, false) &&
                    sameImportedSource((String) entry.getValue(), tree.toString()))
                return indexDngSequenceFolder(tree, new File(new File(getFilesDir(), "imports"), token));
        }
        return indexDngSequenceFolder(tree, new File(new File(getFilesDir(), "imports"), UUID.randomUUID().toString()));
    }

    private File indexDngSequenceFolder(Uri tree, File destination) throws IOException {
        Uri root = sourceFolder(tree, destination);
        ArrayList<String[]> entries = new ArrayList<>();
        for (String[] entry : listSourceFolder(tree, root)) {
            String name = entry[1];
            if (DocumentsContract.Document.MIME_TYPE_DIR.equals(entry[2]) ||
                    name == null || name.contains("/") || name.contains("\\")) continue;
            entries.add(new String[] {entry[0], name, entry[3], entry[4]});
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

        String folderToken = destination.getName();
        if (!destination.isDirectory() && !destination.mkdirs())
            throw new IOException("Could not create the document index");
        java.util.HashSet<String> currentNames = new java.util.HashSet<>();
        for (String[] entry : entries) currentNames.add(entry[1]);
        android.content.SharedPreferences.Editor editor = getSharedPreferences(PREFS, MODE_PRIVATE).edit();
        for (String[] entry : entries) {
            Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, entry[0]);
            File target = new File(destination, entry[1]);
            bindDocument(target, document, Long.parseLong(entry[2]), Long.parseLong(entry[3]), editor);
        }
        // Removed source documents must disappear from the filename index too;
        // otherwise a cached sequence can retain frames that no longer exist.
        File[] previous = destination.listFiles();
        if (previous == null) throw new IOException("Could not read document index");
        for (File file : previous) {
            if (!file.isFile() || currentNames.contains(file.getName())) continue;
            String key = sourceKey(file);
            Files.deleteIfExists(file.toPath());
            editor.remove("document_uri:" + key).remove("document_size:" + key)
                    .remove("document_modified:" + key);
        }
        String location = sourceDirectoryName(tree, root);
        editor
                .putBoolean("source_is_folder:" + folderToken, true)
                .putString("source_tree:" + folderToken, tree.toString())
                .putString("source_uri:" + folderToken, DocumentsContract.buildTreeDocumentUri(
                        tree.getAuthority(), DocumentsContract.getDocumentId(root)).toString())
                .putString("source_location:" + folderToken, location).apply();
        return destination;
    }

}
