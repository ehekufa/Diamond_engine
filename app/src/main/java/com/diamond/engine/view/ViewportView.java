package com.diamond.engine.view;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

import com.diamond.engine.jni.NativeBridge;
import com.diamond.engine.model.Engine;

/**
 * The 3D viewport.
 *
 * <p>A {@link SurfaceView} rather than a TextureView on purpose: the engine
 * renders on its own thread straight into the ANativeWindow, so the framework
 * must not need a GPU copy of the surface to composite it (a TextureView pulls
 * exactly that copy, plus a frame of latency).
 *
 * <p>Touch handling has two personalities:
 * <ul>
 *   <li>edit mode — one finger orbits, two fingers pan, pinch zooms, a tap
 *       selects the node under the pointer;</li>
 *   <li>play mode — touches are forwarded raw so blocks logic sees exactly what
 *       the device reported.</li>
 * </ul>
 * A joystick and two buttons are drawn while playing; they are Java-drawn
 * overlays because they must stay pixel-crisp above the GL surface.
 */
public class ViewportView extends SurfaceView implements SurfaceHolder.Callback {

    /** Receives state changes the activity has to reflect in its own UI. */
    public interface Listener {
        void onViewportReady();

        void onViewportError(String message);

        void onSelectionChanged(int nodeId);
    }

    private static final int MAX_POINTERS = 4;

    private final Paint hudPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint overlayPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float density;

    private Listener listener;
    private boolean attached;
    private boolean playing;

    // Pointer bookkeeping for orbit/pan/pinch.
    private final float[] lastX = new float[MAX_POINTERS];
    private final float[] lastY = new float[MAX_POINTERS];
    private final boolean[] active = new boolean[MAX_POINTERS];
    private int activeCount;
    private float pinchDistance;
    private float downX;
    private float downY;
    private boolean dragging;

    // On-screen gamepad (play mode).
    private float stickCenterX;
    private float stickCenterY;
    private float stickRadius;
    private int stickPointer = -1;

    public ViewportView(Context context) {
        this(context, null);
    }

    public ViewportView(Context context, AttributeSet attrs) {
        super(context, attrs);
        density = context.getResources().getDisplayMetrics().density;
        getHolder().addCallback(this);
        setFocusable(true);
        setFocusableInTouchMode(true);

        hudPaint.setColor(0xFFE8EAF0);
        hudPaint.setTextSize(11 * density);
        hudPaint.setTypeface(Typeface.MONOSPACE);
        overlayPaint.setStyle(Paint.Style.STROKE);
        overlayPaint.setStrokeWidth(2 * density);
        overlayPaint.setColor(0x66FFFFFF);
    }

    public void setListener(Listener listener) {
        this.listener = listener;
    }

    public void setPlaying(boolean playing) {
        this.playing = playing;
        stickPointer = -1;
        invalidate();
    }

    // ---------------------------------------------------------- surface glue

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (!Engine.isReady()) {
            return;
        }
        Engine engine = Engine.get();
        if (engine.attachSurface(holder.getSurface())) {
            attached = true;
            if (listener != null) {
                listener.onViewportReady();
            }
        } else if (listener != null) {
            listener.onViewportError(engine.error());
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (!attached) {
            // Some devices deliver surfaceChanged before surfaceCreated.
            surfaceCreated(holder);
            if (!attached) {
                return;
            }
        }
        Engine.get().onSurfaceChanged(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        attached = false;
        if (Engine.isReady()) {
            Engine.get().detachSurface();
        }
    }

    // --------------------------------------------------------------- drawing

    @Override
    protected void onDraw(Canvas canvas) {
        // The engine paints the surface itself; only the HUD is drawn here.
        super.onDraw(canvas);
        drawHud(canvas);
    }

    private void drawHud(Canvas canvas) {
        if (!attached) {
            return;
        }
        Engine engine = Engine.get();
        String stats = engine.stats();
        if (stats != null && !stats.isEmpty()) {
            String[] parts = stats.split("\\|");
            String line = parts.length > 1 ? parts[0] + " · " + parts[1] + " fps" : parts[0];
            if (parts.length > 4) {
                line += " · " + parts[4] + " nodes";
            }
            canvas.drawText(line, 12 * density, 22 * density, hudPaint);
        }
        if (playing) {
            drawGamepad(canvas);
        }
    }

    private void layoutGamepad() {
        // Geometry is needed before the first draw when play mode starts from a
        // touch, so it is computed lazily instead of only inside onDraw().
        float height = getHeight();
        stickRadius = 56 * density;
        stickCenterX = stickRadius + 24 * density;
        stickCenterY = height - stickRadius - 24 * density;
    }

    private void drawGamepad(Canvas canvas) {
        float width = getWidth();
        layoutGamepad();
        canvas.drawCircle(stickCenterX, stickCenterY, stickRadius, overlayPaint);
        canvas.drawCircle(stickCenterX, stickCenterY, 18 * density, overlayPaint);

        float buttonRadius = 30 * density;
        canvas.drawCircle(width - buttonRadius - 24 * density,
                height - buttonRadius - 24 * density, buttonRadius, overlayPaint);
        canvas.drawCircle(width - buttonRadius * 3 - 36 * density,
                height - buttonRadius - 24 * density, buttonRadius, overlayPaint);
    }

    // ----------------------------------------------------------------- input

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (!Engine.isReady() || !attached) {
            return false;
        }
        if (playing) {
            return handlePlayTouch(event);
        }
        return handleEditTouch(event);
    }

    private boolean handlePlayTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        int pointerId = event.getPointerId(index);

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = event.getX(index);
                float y = event.getY(index);
                if (action == MotionEvent.ACTION_DOWN) {
                    // A fresh gesture: decide between stick and buttons.
                    if (x < getWidth() * 0.4f) {
                        stickPointer = pointerId;
                        updateStick(x, y);
                    } else if (y > getHeight() * 0.7f) {
                        NativeBridge.button(x > getWidth() * 0.8f ? 1 : 0, true);
                    }
                }
                NativeBridge.touch(NativeBridge.TOUCH_DOWN, pointerId, x, y);
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < event.getPointerCount(); i++) {
                    int id = event.getPointerId(i);
                    if (id == stickPointer) {
                        updateStick(event.getX(i), event.getY(i));
                    }
                    NativeBridge.touch(NativeBridge.TOUCH_MOVE, id, event.getX(i), event.getY(i));
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (pointerId == stickPointer) {
                    stickPointer = -1;
                    NativeBridge.joystick(0f, 0f);
                }
                if (action != MotionEvent.ACTION_CANCEL) {
                    NativeBridge.touch(NativeBridge.TOUCH_UP, pointerId,
                            event.getX(index), event.getY(index));
                }
                NativeBridge.button(0, false);
                NativeBridge.button(1, false);
                return true;
            }
            default:
                return false;
        }
    }

    private void updateStick(float x, float y) {
        if (stickRadius <= 0f) {
            layoutGamepad();
        }
        float dx = (x - stickCenterX) / stickRadius;
        float dy = (y - stickCenterY) / stickRadius;
        float length = (float) Math.sqrt(dx * dx + dy * dy);
        if (length > 1f) {
            dx /= length;
            dy /= length;
        }
        NativeBridge.joystick(dx, -dy);
    }

    private boolean handleEditTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                trackPointers(event, true);
                downX = event.getX();
                downY = event.getY();
                dragging = false;
                pinchDistance = pointerDistance(event);
                return true;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                trackPointers(event, true);
                pinchDistance = pointerDistance(event);
                dragging = true;
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (activeCount >= 2) {
                    float distance = pointerDistance(event);
                    // Zoom relative to the previous frame's finger spread.
                    if (pinchDistance > 0f && distance > 0f) {
                        NativeBridge.cameraZoom(distance / pinchDistance);
                    }
                    pinchDistance = distance;
                    // Midpoint movement pans.
                    NativeBridge.cameraPan((event.getX(0) - lastX[0]) / 400f,
                            (event.getY(0) - lastY[0]) / 400f);
                    trackPointers(event, false);
                    dragging = true;
                } else if (activeCount == 1) {
                    int pointerId = event.getPointerId(0);
                    float dx = event.getX(0) - lastX[pointerId];
                    float dy = event.getY(0) - lastY[pointerId];
                    if (Math.abs(dx) + Math.abs(dy) > 2 * density) {
                        dragging = true;
                    }
                    if (dragging) {
                        NativeBridge.cameraOrbit(dx * 0.4f, dy * 0.4f);
                    }
                    lastX[pointerId] = event.getX(0);
                    lastY[pointerId] = event.getY(0);
                }
                return true;
            }
            case MotionEvent.ACTION_UP: {
                if (!dragging) {
                    handleTap(event.getX(), event.getY());
                }
                clearPointers();
                return true;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                trackPointers(event, false);
                pinchDistance = pointerDistance(event);
                if (activeCount <= 1) {
                    dragging = false;
                }
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                clearPointers();
                return true;
            }
            default:
                return false;
        }
    }

    private void handleTap(float x, float y) {
        // Native picking mutates selection; re-read it so the tree highlights
        // the node the user actually touched.
        NativeBridge.touch(NativeBridge.TOUCH_DOWN, 0, x, y);
        NativeBridge.touch(NativeBridge.TOUCH_UP, 0, x, y);
        int node = NativeBridge.selected();
        if (listener != null) {
            listener.onSelectionChanged(node);
        }
    }

    private void trackPointers(MotionEvent event, boolean includeAll) {
        clearPointers();
        int count = Math.min(event.getPointerCount(), MAX_POINTERS);
        for (int i = 0; i < count; i++) {
            int id = event.getPointerId(i);
            if (id < 0 || id >= MAX_POINTERS) {
                continue;
            }
            active[id] = true;
            lastX[id] = event.getX(i);
            lastY[id] = event.getY(i);
            activeCount++;
        }
        if (!includeAll) {
            // MOVE already carries the current positions; nothing else to do.
        }
    }

    private void clearPointers() {
        for (int i = 0; i < MAX_POINTERS; i++) {
            active[i] = false;
        }
        activeCount = 0;
    }

    private float pointerDistance(MotionEvent event) {
        if (event.getPointerCount() < 2) {
            return 0f;
        }
        float dx = event.getX(0) - event.getX(1);
        float dy = event.getY(0) - event.getY(1);
        return (float) Math.sqrt(dx * dx + dy * dy);
    }
}
