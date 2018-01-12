package com.lc.wifidisplaysink;

import java.lang.Math;
import android.view.*;
//import android.view.SurfaceView;
//import android.view.SurfaceHolder;
import android.content.Context;
import android.util.Log;
import android.util.AttributeSet;

import com.lc.wifidisplaysink.WifiDisplaySink;
import com.lc.wifidisplaysink.WifiDisplaySink.OnErrorListener;
import com.lc.wifidisplaysink.WifiDisplaySinkUtils;


public class WifiDisplaySinkView extends SurfaceView {
    private static final String TAG = "WifiDisplaySinkView";

    private String mSourceAddr = null;
    private int mSourcePort = 7236;
    private SurfaceHolder mSurfaceHolder = null;
    private Context mContext = null;
    private WifiDisplaySink mSink;
    private WifiDisplaySink.OnErrorListener mOnErrorListener;

    private int         mSurfaceWidth;
    private int         mSurfaceHeight;

    private void initView() {
        getHolder().addCallback(mSHCallback);
        getHolder().setType(SurfaceHolder.SURFACE_TYPE_PUSH_BUFFERS);
        setFocusable(true);
        setFocusableInTouchMode(true);
        requestFocus();
        Log.d(TAG, "WifiDisplaySink.create");
        mSink = WifiDisplaySink.create(mContext);
    }

    public WifiDisplaySinkView(Context context) {
        super(context);
        mContext = context;
        initView();
    }

    public WifiDisplaySinkView(Context context, AttributeSet attrs) {
        super(context, attrs, 0);
        mContext = context;
        initView();
    }

    public WifiDisplaySinkView(Context context, AttributeSet attrs, int defStyle) {
        super(context, attrs, defStyle);
        mContext = context;
        initView();
    }

    public void setSourceIpAddr(String sourceAddr, int sourcePort) {
        mSourceAddr = sourceAddr;
        mSourcePort = sourcePort;
        Log.d(TAG, "setSourceIpAddr");
    }

    public void setOnErrorListener(OnErrorListener l)
    {
        mOnErrorListener = l;
    }

    SurfaceHolder.Callback mSHCallback = new SurfaceHolder.Callback()
    {
        public void surfaceChanged(SurfaceHolder holder, int format,
                                    int w, int h)
        {
            Log.d(TAG, "surfaceChanged: size [" + w + " x " + h +"]");
            mSurfaceWidth = w;
            mSurfaceHeight = h;
        }

        public void surfaceCreated(SurfaceHolder holder)
        {
            Log.d(TAG, "surfaceCreated");
            mSurfaceHolder = holder;
            mSink.setDisplay(mSurfaceHolder);
            Log.d(TAG, "WifiDisplaySink.invoke");
            mSink.invoke(mSourceAddr, mSourcePort);
        }

        public void surfaceDestroyed(SurfaceHolder holder)
        {
            Log.d(TAG, "surfaceDestroyed");
            // after we return from this we can't use the surface any more
            mSurfaceHolder = null;
            //release(true);
			mSink.destory();
        }
    };
}
