package com.diamond.engine.ui;

import android.content.Context;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.TextView;

import com.diamond.engine.model.Engine;

/**
 * Flat list adapter for the scene hierarchy.
 *
 * <p>The tree is already flattened by the engine (depth-first order, each row
 * carrying its depth), so the adapter only has to indent. A framework
 * {@link BaseAdapter} is deliberate: a RecyclerView would add a dependency and
 * a scroll-animation cost for a list that is at most a few hundred rows.
 */
public final class SceneTreeAdapter extends BaseAdapter {

    private final Context context;
    private final Engine engine;
    private final float density;

    public SceneTreeAdapter(Context context, Engine engine) {
        this.context = context;
        this.engine = engine;
        this.density = context.getResources().getDisplayMetrics().density;
    }

    @Override
    public int getCount() {
        return engine.sceneRows().length;
    }

    @Override
    public Engine.NodeRow getItem(int position) {
        Engine.NodeRow[] rows = engine.sceneRows();
        return position >= 0 && position < rows.length ? rows[position] : null;
    }

    @Override
    public long getItemId(int position) {
        Engine.NodeRow row = getItem(position);
        return row == null ? 0L : row.id;
    }

    @Override
    public View getView(int position, View convertView, ViewGroup parent) {
        TextView row = convertView instanceof TextView
                ? (TextView) convertView
                : createRow(parent);
        Engine.NodeRow item = getItem(position);
        if (item == null) {
            row.setText("");
            return row;
        }
        row.setText(label(item));
        row.setPadding((int) ((12 + item.depth * 14) * density), row.getPaddingTop(),
                row.getPaddingRight(), row.getPaddingBottom());
        row.setTypeface(Typeface.MONOSPACE, item.selected ? Typeface.BOLD : Typeface.NORMAL);
        row.setBackgroundColor(item.selected ? 0xFF2A3C55 : 0x00000000);
        row.setTextColor(item.type == 0 ? 0xFF9AA4B2 : 0xFFE8EAF0);
        return row;
    }

    private TextView createRow(ViewGroup parent) {
        TextView text = new TextView(context);
        text.setLayoutParams(new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        text.setGravity(Gravity.CENTER_VERTICAL);
        text.setTextSize(13);
        text.setMinHeight((int) (44 * density));
        text.setPadding((int) (12 * density), 0, (int) (12 * density), 0);
        return text;
    }

    private static String label(Engine.NodeRow row) {
        String prefix;
        switch (row.type) {
            case 1:
                prefix = "S ";   // sprite
                break;
            case 2:
                prefix = "M ";   // mesh
                break;
            case 3:
                prefix = "L ";   // light
                break;
            case 4:
                prefix = "C ";   // camera
                break;
            default:
                prefix = "◇ ";   // empty / group
                break;
        }
        return prefix + row.name + "  #" + row.id;
    }
}
