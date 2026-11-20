package com.diamond.engine.model;

import com.diamond.engine.jni.NativeBridge;

/**
 * Java-side façade over the native engine.
 *
 * <p>Responsibilities kept here, deliberately out of the native core:
 * <ul>
 *   <li>the lifecycle handshake (create → attach surface → pause/resume →
 *       detach → shutdown) so no activity has to remember the order;</li>
 *   <li>a snapshot of the scene tree for the RecyclerView adapter, refreshed
 *       only when the tree actually changes;</li>
 *   <li>turning native status codes into exceptions where a failure is a bug
 *       and into UI messages where it is user input.</li>
 * </ul>
 */
public final class Engine {

    /** Thrown when the native layer rejects a call that should have succeeded. */
    public static final class EngineException extends RuntimeException {
        public final int code;

        EngineException(int code, String message) {
            super(message + " (code " + code + ")");
            this.code = code;
        }
    }

    /** One row of the scene tree: a node with its depth for indentation. */
    public static final class NodeRow {
        public final int id;
        public final int depth;
        public final int type;
        public final String name;
        public final boolean selected;

        NodeRow(int id, int depth, int type, String name, boolean selected) {
            this.id = id;
            this.depth = depth;
            this.type = type;
            this.name = name;
            this.selected = selected;
        }
    }

    private static Engine instance;

    private final String filesDir;
    private final String cacheDir;
    private boolean initialised;
    private boolean surfaceAttached;
    private int sceneRevision;
    private NodeRow[] rows = new NodeRow[0];
    private int cachedSelected = -1;

    private Engine(String filesDir, String cacheDir) {
        this.filesDir = filesDir;
        this.cacheDir = cacheDir;
    }

    /**
     * Creates (once) and initialises the engine for {@code filesDir}.
     *
     * <p>Throws instead of crashing when the native library is absent for this
     * device's ABI: without this check the very first {@code NativeBridge}
     * reference dies with {@link UnsatisfiedLinkError} and the process is gone
     * before the activity can explain anything.
     */
    public static synchronized Engine create(String filesDir, String cacheDir, int apiLevel) {
        if (instance != null && instance.initialised) {
            return instance;
        }
        if (!NativeBridge.isLoaded()) {
            throw new EngineException(NativeBridge.ERR_UNSUPPORTED, NativeBridge.loadError());
        }
        Engine engine = new Engine(filesDir, cacheDir);
        int status = NativeBridge.init(filesDir, cacheDir, apiLevel);
        if (status != NativeBridge.OK) {
            throw new EngineException(status, NativeBridge.lastError());
        }
        engine.initialised = true;
        instance = engine;
        return engine;
    }

    public static synchronized Engine get() {
        if (instance == null) {
            throw new IllegalStateException("Engine.create() has not been called");
        }
        return instance;
    }

    public static synchronized boolean isReady() {
        return instance != null && instance.initialised;
    }

    public String banner() {
        return NativeBridge.versionBanner();
    }

    public String error() {
        return NativeBridge.lastError();
    }

    // --------------------------------------------------------------- surfaces

    /**
     * Attaches a Surface (from a SurfaceView/TextureView holder). Returns false
     * and leaves a message in {@link #error()} when the GPU refused the surface;
     * the caller should show it and offer OpenGL ES.
     */
    public boolean attachSurface(Object surface) {
        if (surfaceAttached) {
            NativeBridge.detachSurface();
            surfaceAttached = false;
        }
        surfaceAttached = NativeBridge.attachSurface(surface);
        return surfaceAttached;
    }

    public void detachSurface() {
        if (!surfaceAttached) {
            return;
        }
        NativeBridge.detachSurface();
        surfaceAttached = false;
    }

    public void onSurfaceChanged(int width, int height) {
        NativeBridge.surfaceResized(width, height);
    }

    public void onPause() {
        NativeBridge.onPause();
    }

    public void onResume() {
        NativeBridge.onResume();
    }

    public boolean isSurfaceAttached() {
        return surfaceAttached;
    }

    public void shutdown() {
        if (!initialised) {
            return;
        }
        detachSurface();
        NativeBridge.shutdown();
        initialised = false;
        synchronized (Engine.class) {
            if (instance == this) {
                instance = null;
            }
        }
    }

    // -------------------------------------------------------------- rendering

    public int renderMode() {
        return NativeBridge.getRenderMode();
    }

    public void setRenderMode(int mode) {
        int status = NativeBridge.setRenderMode(mode);
        if (status != NativeBridge.OK) {
            throw new EngineException(status, NativeBridge.lastError());
        }
    }

    public boolean vulkanSupported() {
        return NativeBridge.vulkanSupported();
    }

    public void setViewMode(int mode) {
        NativeBridge.setViewMode(mode);
    }

    public int viewMode() {
        return NativeBridge.getViewMode();
    }

    public void setTool(int tool) {
        NativeBridge.setTool(tool);
    }

    public int tool() {
        return NativeBridge.getTool();
    }

    public void setDensity(float density) {
        NativeBridge.setDensity(density);
    }

    // ------------------------------------------------------------------ scene

    public int addPrimitive(int primitive) {
        int node = NativeBridge.addPrimitive(primitive);
        if (node > 0) {
            invalidateScene();
        }
        return node;
    }

    public int addGroup() {
        int node = NativeBridge.addNode(NativeBridge.NODE_EMPTY, 0, "Group");
        if (node > 0) {
            invalidateScene();
        }
        return node;
    }

    public void deleteNode(int node) {
        NativeBridge.deleteNode(node);
        invalidateScene();
    }

    public void duplicateNode(int node) {
        NativeBridge.duplicateNode(node);
        invalidateScene();
    }

    public void newScene() {
        NativeBridge.newScene();
        invalidateScene();
    }

    public int importObj(String text, String name) {
        int node = NativeBridge.importObjAsNode(text, name);
        if (node > 0) {
            invalidateScene();
        }
        return node;
    }

    public void setSprite(int node, android.graphics.Bitmap bitmap) {
        int status = NativeBridge.setNodeImage(node, bitmap);
        if (status != NativeBridge.OK) {
            throw new EngineException(status, NativeBridge.lastError());
        }
        invalidateScene();
    }

    public void rename(int node, String name) {
        NativeBridge.nodeRename(node, name);
        invalidateScene();
    }

    public void reparent(int node, int parent) {
        NativeBridge.nodeReparent(node, parent);
        invalidateScene();
    }

    public void select(int node) {
        NativeBridge.select(node);
        cachedSelected = node;
    }

    public int selected() {
        cachedSelected = NativeBridge.selected();
        return cachedSelected;
    }

    public void focusSelected() {
        int node = selected();
        if (node > 0) {
            NativeBridge.cameraFocusNode(node);
        }
    }

    public void invalidateScene() {
        sceneRevision++;
        rows = null;
    }

    public int sceneRevision() {
        return sceneRevision;
    }

    /** Flattened scene tree, ready for the list adapter. Cached per revision. */
    public NodeRow[] sceneRows() {
        if (rows != null) {
            return rows;
        }
        int count = NativeBridge.nodeCount();
        NodeRow[] built = new NodeRow[count];
        int selected = NativeBridge.selected();
        for (int i = 0; i < count; i++) {
            int node = NativeBridge.nodeAt(i);
            if (node <= 0) {
                continue;
            }
            built[i] = new NodeRow(node, NativeBridge.nodeDepth(node),
                    NativeBridge.nodeType(node), NativeBridge.nodeName(node),
                    node == selected);
        }
        rows = built;
        return rows;
    }

    // ----------------------------------------------------------------- blocks

    public int blockCount() {
        return NativeBridge.blockCount();
    }

    public float[] blockPosition(int block) {
        return NativeBridge.blockPos(block);
    }

    public void moveBlock(int block, float x, float y) {
        NativeBridge.blockMove(block, x, y);
    }

    public int addBlock(int kind, int node, int param, float x, float y, String text,
                        float a, float b, float c) {
        return NativeBridge.blockAdd(kind, node, param, x, y, text, a, b, c);
    }

    public void removeBlock(int block) {
        NativeBridge.blockRemove(block);
    }

    public int linkBlocks(int from, int to) {
        return NativeBridge.blockLink(from, to);
    }

    public String blocksAsLua() {
        return NativeBridge.blocksToLua();
    }

    public String blocksAsJson() {
        return NativeBridge.blocksExport();
    }

    /** Returns null when valid, otherwise the compiler's message. */
    public String checkLua(String source) {
        return NativeBridge.luacCheck(source);
    }

    public String compileLua(String source) {
        return NativeBridge.luacCompile(source);
    }

    public String luacVersion() {
        return NativeBridge.luacVersion();
    }

    // ------------------------------------------------------------------- play

    public boolean isPlaying() {
        return NativeBridge.playState() == NativeBridge.PLAYING;
    }

    public void togglePlay() {
        if (isPlaying()) {
            NativeBridge.stop();
        } else {
            NativeBridge.play();
        }
    }

    public void stopPlay() {
        NativeBridge.stop();
    }

    // -------------------------------------------------------------- documents

    public String projectFile(String name) {
        String safe = name == null || name.isEmpty() ? "untitled" : name;
        if (!safe.endsWith(".diamond")) {
            safe = safe + ".diamond";
        }
        return filesDir + "/" + safe;
    }

    public void save(String name) {
        int status = NativeBridge.saveDiamond(projectFile(name));
        if (status != NativeBridge.OK) {
            throw new EngineException(status, NativeBridge.lastError());
        }
    }

    public void load(String path) {
        int status = NativeBridge.loadDiamond(path);
        if (status != NativeBridge.OK) {
            throw new EngineException(status, NativeBridge.lastError());
        }
        invalidateScene();
    }

    public String stats() {
        return NativeBridge.stats();
    }

    public String filesDir() {
        return filesDir;
    }

    public String cacheDir() {
        return cacheDir;
    }
}
