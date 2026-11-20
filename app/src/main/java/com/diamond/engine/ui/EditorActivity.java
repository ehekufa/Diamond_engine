package com.diamond.engine.ui;

import android.app.Activity;
import android.content.Intent;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import com.diamond.engine.R;
import com.diamond.engine.jni.NativeBridge;
import com.diamond.engine.model.Engine;
import com.diamond.engine.view.ViewportView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * The editor window.
 *
 * <p>Three build modes share this one screen, chosen by which containers are
 * present in the layout: a viewport plus a scene tree (landscape), a viewport
 * plus a bottom sheet (portrait) and the log console (toggled). Keeping a single
 * activity avoids the configuration churn that comes with fragment swapping on
 * low-end devices, where an activity restart costs a full scene reload.
 */
public final class EditorActivity extends Activity implements ViewportView.Listener {

    private static final String TAG = "DiamondEditor";

    private Engine engine;
    private ViewportView viewport;
    private TextView statusLine;
    private TextView logConsole;
    private ListView sceneTree;
    private SceneTreeAdapter treeAdapter;
    private int logSequence;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_editor);

        viewport = findViewById(R.id.viewport);
        statusLine = findViewById(R.id.status_line);
        logConsole = findViewById(R.id.log_console);
        sceneTree = findViewById(R.id.scene_tree);
        viewport.setListener(this);

        String filesDir = getFilesDir().getAbsolutePath();
        String cacheDir = getCacheDir().getAbsolutePath();
        try {
            engine = Engine.create(filesDir, cacheDir, Build.VERSION.SDK_INT);
        } catch (Engine.EngineException error) {
            // Nothing can render without the native core; say so and finish
            // instead of leaving a black window behind.
            Log.e(TAG, "engine init failed", error);
            // The activity has no useful state without the native core, so show
            // why and let the user back out instead of leaving a black window.
            showFatal(getString(R.string.error_no_engine) + "\n" + error.getMessage());
            return;
        }

        AssetManager assets = getAssets();
        NativeBridge.installAssets(assets);
        treeAdapter = new SceneTreeAdapter(this, engine);
        if (sceneTree != null) {
            sceneTree.setAdapter(treeAdapter);
            sceneTree.setOnItemClickListener((parent, view, position, id) -> {
                Engine.NodeRow row = treeAdapter.getItem(position);
                if (row != null) {
                    engine.select(row.id);
                    refreshTree();
                    updateStatus();
                }
            });
        }
        NativeBridge.logWrite("app", engine.banner());
        updateStatus();
        wireControls();
        handleIntent(getIntent());
    }

    private void wireControls() {
        findViewById(R.id.button_play).setOnClickListener(v -> {
            engine.togglePlay();
            viewport.setPlaying(engine.isPlaying());
            updateStatus();
        });
        findViewById(R.id.button_add_cube).setOnClickListener(v ->
                engine.addPrimitive(NativeBridge.PRIM_CUBE));
        findViewById(R.id.button_add_group).setOnClickListener(v -> engine.addGroup());
        findViewById(R.id.button_delete).setOnClickListener(v -> {
            int selected = engine.selected();
            if (selected > 0) {
                engine.deleteNode(selected);
                updateStatus();
            }
        });
        findViewById(R.id.button_view_2d).setOnClickListener(v -> {
            engine.setViewMode(NativeBridge.VIEW_2D);
            updateStatus();
        });
        findViewById(R.id.button_view_3d).setOnClickListener(v -> {
            engine.setViewMode(NativeBridge.VIEW_3D);
            updateStatus();
        });
        findViewById(R.id.button_new).setOnClickListener(v -> {
            engine.newScene();
            updateStatus();
        });
        findViewById(R.id.button_save).setOnClickListener(v ->
                saveWithFeedback("untitled"));
        findViewById(R.id.button_log).setOnClickListener(v -> {
            logConsole.setVisibility(logConsole.getVisibility() == View.VISIBLE
                    ? View.GONE : View.VISIBLE);
            refreshLog();
        });
    }

    private void handleIntent(Intent intent) {
        if (intent == null || intent.getData() == null) {
            return;
        }
        Uri data = intent.getData();
        if (!Intent.ACTION_VIEW.equals(intent.getAction())) {
            return;
        }
        try {
            byte[] project = readAll(getContentResolver().openInputStream(data));
            int status = NativeBridge.deserialize(project);
            if (status == NativeBridge.OK) {
                engine.invalidateScene();
                NativeBridge.logWrite("io", "opened " + data.getLastPathSegment());
            } else {
                NativeBridge.logWrite("io", "could not open project: " + NativeBridge.lastError());
            }
        } catch (IOException error) {
            NativeBridge.logWrite("io", "read failed: " + error.getMessage());
        }
        updateStatus();
    }

    private static byte[] readAll(InputStream input) throws IOException {
        if (input == null) {
            throw new IOException("no stream");
        }
        try (InputStream stream = input) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int read;
            while ((read = stream.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            return out.toByteArray();
        }
    }

    private void saveWithFeedback(String name) {
        try {
            engine.save(name);
            toast(getString(R.string.toast_saved, engine.projectFile(name)));
        } catch (Engine.EngineException error) {
            toast(getString(R.string.toast_save_failed, error.getMessage()));
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntent(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (engine != null) {
            engine.onResume();
        }
    }

    @Override
    protected void onPause() {
        if (engine != null) {
            engine.onPause();
        }
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (engine != null) {
            engine.shutdown();
            engine = null;
        }
        super.onDestroy();
    }

    // ------------------------------------------------------- viewport events

    @Override
    public void onViewportReady() {
        updateStatus();
    }

    @Override
    public void onViewportError(String message) {
        // The engine already fell back to OpenGL ES 3.0 when it could; what is
        // left is a genuinely unusable GPU, so tell the user rather than retry.
        NativeBridge.logWrite("render", message);
        toast(getString(R.string.toast_render_failed, message));
        updateStatus();
    }

    @Override
    public void onSelectionChanged(int nodeId) {
        refreshTree();
        updateStatus();
    }

    /** Rebuilds the list only when the engine reports a new scene revision. */
    private void refreshTree() {
        if (treeAdapter == null) {
            return;
        }
        // The adapter caches the flattened tree per scene revision, so this is a
        // cheap repaint unless the tree actually changed.
        treeAdapter.notifyDataSetChanged();
    }

    // ---------------------------------------------------------------- footer

    private void updateStatus() {
        if (engine == null || statusLine == null) {
            return;
        }
        String stats = engine.stats();
        String[] parts = stats == null ? new String[0] : stats.split("\\|");
        StringBuilder line = new StringBuilder();
        if (parts.length > 0) {
            line.append(parts[0]);
        }
        if (parts.length > 1) {
            line.append("  ").append(parts[1]).append(" fps");
        }
        if (parts.length > 4) {
            line.append("  ").append(parts[4]).append(" nodes");
        }
        if (parts.length > 5) {
            line.append("  ").append(parts[5]).append(" blocks");
        }
        int selected = engine.selected();
        if (selected > 0) {
            line.append("  sel #").append(selected);
        }
        statusLine.setText(line.toString());
        refreshTree();
        refreshLog();
    }

    private void refreshLog() {
        if (logConsole == null || logConsole.getVisibility() != View.VISIBLE) {
            return;
        }
        int[] meta = new int[2];
        String lines = NativeBridge.logPoll(logSequence, meta);
        if (meta[0] > 0) {
            logSequence = meta[0];
        }
        if (lines == null || lines.isEmpty()) {
            return;
        }
        logConsole.append(lines);
        if (logConsole.length() > 20000) {
            logConsole.setText(logConsole.getText().subSequence(logConsole.length() - 20000,
                    logConsole.length()));
        }
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
    }

    private void showFatal(String message) {
        TextView error = findViewById(R.id.fatal_error);
        if (error != null) {
            error.setText(message);
            error.setVisibility(View.VISIBLE);
        }
        ViewGroup root = findViewById(R.id.editor_root);
        if (root != null) {
            for (int i = 0; i < root.getChildCount(); i++) {
                View child = root.getChildAt(i);
                if (child.getId() != R.id.fatal_error) {
                    child.setVisibility(View.GONE);
                }
            }
        }
    }
}
