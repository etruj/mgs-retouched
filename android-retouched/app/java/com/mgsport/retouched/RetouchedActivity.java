package com.mgsport.retouched;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

/* METAL GEAR SOLID RETOUCHED: the native game (libmgst.so) in a NativeActivity that can also go
 * full screen: no status or navigation bar, drawing into the camera cut-out area. The native side
 * calls setImmersive() when the "Full screen" setting changes, and exportSave()/importSave() to move
 * SAVE.data to and from a file the player picks (Android's file picker: Downloads, Drive, USB, ...). */
public class RetouchedActivity extends NativeActivity implements Runnable {
    /* NativeActivity loads libmgst.so itself, but only System.loadLibrary makes its JNI functions
     * (nativeResult) visible to Java; the second load returns the same library */
    static { System.loadLibrary("mgst"); }

    private volatile boolean immersive = true;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        apply();
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) apply();
    }

    @SuppressWarnings("deprecation")
    private void apply() {
        View d = getWindow().getDecorView();
        d.setSystemUiVisibility(immersive
            ? View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
              | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
            : 0);
    }

    /* called from native code (any thread) */
    public void setImmersive(boolean on) {
        immersive = on;
        runOnUiThread(this);
    }

    @Override public void run() { apply(); }

    /* ---- save transfer ---- */
    private static final int REQ_EXPORT = 41, REQ_IMPORT = 42;
    private static final int IO_EXPORTED = 1, IO_IMPORTED = 2, IO_FAILED = 3, IO_CANCELLED = 4;
    private volatile byte[] pendingExport;
    private native void nativeResult(int kind, byte[] data);

    public void exportSave(byte[] data) {
        pendingExport = data;
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/octet-stream");
        i.putExtra(Intent.EXTRA_TITLE, "MGS_Touch_SAVE.data");
        startActivityForResult(i, REQ_EXPORT);
    }
    public void importSave() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_IMPORT);
    }
    public void restartGame() {
        Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (i != null) { i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK); startActivity(i); }
        System.exit(0);
    }
    @Override protected void onActivityResult(int req, int result, Intent data) {
        super.onActivityResult(req, result, data);
        if (req != REQ_EXPORT && req != REQ_IMPORT) return;
        Uri uri = data != null ? data.getData() : null;
        if (result != RESULT_OK || uri == null) { nativeResult(IO_CANCELLED, null); return; }
        try {
            if (req == REQ_EXPORT) {
                OutputStream o = getContentResolver().openOutputStream(uri, "wt");
                o.write(pendingExport); o.close();
                nativeResult(IO_EXPORTED, null);
            } else {
                InputStream in = getContentResolver().openInputStream(uri);
                ByteArrayOutputStream b = new ByteArrayOutputStream(); byte[] buf = new byte[4096]; int n;
                while ((n = in.read(buf)) > 0 && b.size() <= 65536) b.write(buf, 0, n);
                in.close();
                nativeResult(IO_IMPORTED, b.toByteArray());
            }
        } catch (Exception e) {
            nativeResult(IO_FAILED, null);
        }
    }
}
