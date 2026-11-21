package com.diamond.engine.jni;

/**
 * The only bridge to {@code libengine.so}.
 *
 * <p>Every method here is a thin mapping onto the C ABI declared in
 * {@code cpp/include/diamond.h}; all real work (and all bounds checking) lives
 * in the native core. Callers in {@code ui/} should reach the engine through
 * {@link com.diamond.engine.model.Engine} rather than calling this class
 * directly, because that wrapper keeps the Java-side caches in step.
 *
 * <p>String arguments are UTF-8 and copied by the engine; none of the returned
 * strings are retained by Java, so nothing needs to be freed.
 *
 * <p>Error reporting follows a single convention: methods that can fail return
 * a status code ({@code 0} == {@code DIAMOND_OK}) and, when they take an
 * outcome path, leave a message retrievable via {@link #lastError()}.
 */
public final class NativeBridge {

    /**
     * Status codes, mirrored from {@code DiamondStatus} in {@code diamond.h}.
     *
     * <p>{@code tools/check_repo.py} compares every one of these with the C enum,
     * because a drift here is invisible at runtime: the engine returns 4 and Java
     * reports it as something else entirely.
     */
    public static final int OK = 0;
    public static final int ERR_ARG = 1;
    public static final int ERR_STATE = 2;
    public static final int ERR_MEMORY = 3;
    public static final int ERR_IO = 4;
    public static final int ERR_FORMAT = 5;
    public static final int ERR_GPU = 6;
    public static final int ERR_UNSUPPORTED = 7;
    public static final int ERR_PARSE = 8;
    public static final int ERR_LOGIC = 9;

    /** Render backends, mirrored from {@code diamond.h}. */
    public static final int RENDER_GLES3 = 1;
    public static final int RENDER_VULKAN = 2;
    public static final int RENDER_GLES2 = 3;
    public static final int RENDER_HYBRID = 4;

    /**
     * View modes, mirrored from {@code diamond.h}.
     *
     * <p>These must match {@code DiamondView} exactly: the native side rejects
     * anything outside {@code [VIEW_2D, VIEW_3D]}, so an off-by-one here makes
     * the corresponding button silently do nothing.
     */
    public static final int VIEW_2D = 0;
    public static final int VIEW_25D = 1;
    public static final int VIEW_3D = 2;

    /** Editor tools. */
    public static final int TOOL_SELECT = 0;
    public static final int TOOL_MOVE = 1;
    public static final int TOOL_ROTATE = 2;
    public static final int TOOL_SCALE = 3;

    /** Touch phases. */
    public static final int TOUCH_DOWN = 0;
    public static final int TOUCH_MOVE = 1;
    public static final int TOUCH_UP = 2;

    /** Play states. */
    public static final int STOPPED = 0;
    public static final int PLAYING = 1;
    public static final int PAUSED = 2;

    /** Block kinds, mirrored from {@code dlogic.h}. */
    public static final int BLOCK_ON_START = 0;
    public static final int BLOCK_ON_UPDATE = 1;
    public static final int BLOCK_ON_TOUCH = 2;
    public static final int BLOCK_ON_COLLIDE = 3;
    public static final int BLOCK_MOVE = 10;
    public static final int BLOCK_ROTATE = 11;
    public static final int BLOCK_SET_COLOR = 12;
    public static final int BLOCK_LOG = 13;
    public static final int BLOCK_SET_POS = 14;
    public static final int BLOCK_SET_SCALE = 15;
    public static final int BLOCK_IF_BUTTON = 20;
    public static final int BLOCK_IF_TIMER = 21;
    public static final int BLOCK_IF_COLLIDE = 22;
    public static final int BLOCK_IF_VAR = 23;

    /** Node types. */
    public static final int NODE_EMPTY = 0;
    public static final int NODE_SPRITE = 1;
    public static final int NODE_MESH = 2;
    public static final int NODE_LIGHT = 3;
    public static final int NODE_CAMERA = 4;

    /**
     * Primitives, mirrored from {@code DiamondPrimitive}.
     *
     * <p>Order matters: these are the engine's, not a convenient local order.
     * Getting it wrong is silent — "Cube" used to add a quad because Java had
     * the first two swapped.
     */
    public static final int PRIM_QUAD = 0;
    public static final int PRIM_CUBE = 1;
    public static final int PRIM_PYRAMID = 2;
    public static final int PRIM_GRID = 3;

    private NativeBridge() {
    }

    /**
     * Non-null when the native library could not be loaded at all.
     *
     * <p>{@code System.loadLibrary} inside a static initialiser throws
     * {@link UnsatisfiedLinkError}, and the JVM then wraps it in an
     * {@link ExceptionInInitializerError} on the *first* reference to any
     * member of this class — so even reading a constant would crash the app
     * before any UI existed. Loading it explicitly keeps the failure
     * inspectable, and the activity can show a message instead of dying.
     */
    private static final String LOAD_ERROR = loadEngine();

    private static String loadEngine() {
        try {
            // The engine is one .so; anything else in the APK is framework code.
            System.loadLibrary("engine");
            return null;
        } catch (UnsatisfiedLinkError | SecurityException error) {
            String detail = error.getMessage();
            return detail == null ? error.toString() : detail;
        }
    }

    /** True when {@code libengine.so} was found for this device's ABI. */
    public static boolean isLoaded() {
        return LOAD_ERROR == null;
    }

    /** Why the library is missing, or an empty string when it is not. */
    public static String loadError() {
        return LOAD_ERROR == null ? "" : LOAD_ERROR;
    }

    // ------------------------------------------------------------- lifecycle

    public static native int init(String filesDir, String cacheDir, int apiLevel);

    public static native void shutdown();

    public static native String versionBanner();

    /** Message from the last failing call, or an empty string. */
    public static native String lastError();

    public static native boolean installAssets(Object assetManager);

    // -------------------------------------------------------------- surfaces

    public static native boolean attachSurface(Object surface);

    public static native void detachSurface();

    public static native void surfaceResized(int width, int height);

    public static native void onPause();

    public static native void onResume();

    // ------------------------------------------------------------- rendering

    public static native int setRenderMode(int mode);

    public static native int getRenderMode();

    public static native boolean vulkanSupported();

    public static native String gpuVendor();

    public static native String gpuRenderer();

    public static native void setViewMode(int mode);

    public static native int getViewMode();

    public static native void setTool(int tool);

    public static native int getTool();

    public static native void setDensity(float density);

    // ---------------------------------------------------------------- camera

    public static native void cameraOrbit(float dx, float dy);

    public static native void cameraPan(float dx, float dy);

    public static native void cameraZoom(float factor);

    public static native void cameraReset();

    public static native void cameraFocusNode(int node);

    // ----------------------------------------------------------------- scene

    public static native int addPrimitive(int primitive);

    public static native int addNode(int type, int parent, String name);

    public static native int deleteNode(int node);

    public static native int duplicateNode(int node);

    public static native void newScene();

    public static native int importObjText(int node, String text);

    public static native int importObjAsNode(String text, String name);

    /** Uploads an {@code ARGB_8888} or {@code RGBA_8888} bitmap as the node's sprite. */
    public static native int setNodeImage(int node, android.graphics.Bitmap bitmap);

    public static native int nodeCount();

    public static native int nodeAt(int index);

    public static native int nodeType(int node);

    public static native int nodeParent(int node);

    public static native int nodeChildCount(int node);

    public static native int nodeDepth(int node);

    public static native String nodeName(int node);

    public static native void nodeRename(int node, String name);

    public static native int nodeReparent(int node, int parent);

    /** Returns {x, y, z, rx, ry, rz, sx, sy, sz}. */
    public static native float[] nodeTransform(int node);

    public static native void nodeSetTransform(int node, float[] trs);

    public static native void nodeSetColor(int node, int argb);

    public static native int nodeColor(int node);

    public static native void nodeSetCollision(int node, boolean enabled);

    public static native boolean nodeCollision(int node);

    /** Returns {minX, minY, minZ, maxX, maxY, maxZ} in world space, or null. */
    public static native float[] nodeAabb(int node);

    public static native void select(int node);

    public static native int selected();

    // ---------------------------------------------------------------- blocks

    public static native void blocksClear();

    public static native int blockAdd(int kind, int node, int param, float x, float y,
                                      String text, float a, float b, float c);

    public static native int blockRemove(int block);

    public static native void blockMove(int block, float x, float y);

    public static native int blockLink(int from, int to);

    public static native int blockUnlink(int link);

    public static native int blockCount();

    public static native int blockAt(int index);

    public static native int blockKind(int block);

    public static native int blockParam(int block);

    /** Returns {x, y}. */
    public static native float[] blockPos(int block);

    public static native String blockText(int block);

    public static native int linkCount();

    /** Fills {@code out = {from, to}} and returns the link id, or 0. */
    public static native int linkAt(int index, int[] out);

    public static native String blocksExport();

    public static native int blocksImport(String json);

    public static native String blocksToLua();

    public static native int blocksFromLua(String source);

    public static native String luacVersion();

    /** Returns null when the source is valid, otherwise the compiler message. */
    public static native String luacCheck(String source);

    /** Returns generated C for the source, or null with {@link #lastError()}. */
    public static native String luacCompile(String source);

    // ------------------------------------------------------------------ play

    public static native void play();

    public static native void pause();

    public static native void stop();

    public static native int playState();

    // ----------------------------------------------------------------- input

    public static native void touch(int phase, int pointer, float x, float y);

    public static native void button(int button, boolean pressed);

    public static native void joystick(float x, float y);

    /** Returns {x, y}. */
    public static native float[] joystickGet();

    // ------------------------------------------------------------- documents

    public static native int saveDiamond(String path);

    public static native int loadDiamond(String path);

    public static native byte[] serialize();

    public static native int deserialize(byte[] data);

    // ------------------------------------------------------------ file access

    public static native byte[] readAsset(String path);

    public static native byte[] readFile(String path);

    public static native boolean writeFile(String path, byte[] data);

    // ------------------------------------------------------------ diagnostics

    /** "MODE|fps|frameMs|rssMb|nodes|blocks" — one string, parsed by the UI. */
    public static native String stats();

    public static native void logWrite(String tag, String message);

    public static native int logCount();

    /**
     * Returns new log lines since {@code sinceSeq} and writes
     * {@code out = {nextSeq, lineCount}}.
     */
    public static native String logPoll(int sinceSeq, int[] out);
}
