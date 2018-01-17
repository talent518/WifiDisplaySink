package com.lc.wifidisplaysink;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Timer;
import java.util.TimerTask;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.ComponentName;
import android.os.IBinder;
import android.os.Bundle;
import android.util.Log;
import android.util.DisplayMetrics;
import android.widget.TextView;
import android.widget.ImageView;
import android.graphics.drawable.AnimationDrawable;
import android.graphics.Color;

import android.content.ServiceConnection;

public class WaitingConnectionActivity extends Activity {
	private static final String TAG = "WaitingConnectionActivity";

	private ImageView mConnectingImageView;
	private TextView mConnectingTextView;
	private AnimationDrawable mConnectingAnimation;

	@Override
	public void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_waiting_connection);

		mConnectingTextView = (TextView) findViewById(R.id.connecting_textview);
		mConnectingImageView = (ImageView) findViewById(R.id.connecting_imageview);

		mConnectingAnimation = (AnimationDrawable) mConnectingImageView.getBackground();
		mConnectingImageView.post(new Runnable() {
			@Override
			public void run() {
				mConnectingAnimation.start();
			}
		});

		Intent intent = new Intent(this, WifiDisplayService.class);
		startService(intent);

		intent = new Intent(this, WifiDisplayService.class);
		bindService(intent, mConn, Context.BIND_AUTO_CREATE);

		DisplayMetrics dm = getResources().getDisplayMetrics();
		Log.v(TAG, "DisplayMetrics.widthPixels = " + dm.widthPixels + ", DisplayMetrics.heightPixels" + dm.heightPixels);
	}

	WifiDisplayService mWifiDisplayService;

	private ServiceConnection mConn = new ServiceConnection() {
		public void onServiceConnected(ComponentName name, IBinder service) {
			Log.d(TAG, "onServiceConnected");
			if (service != null) {
				mWifiDisplayService = ((WifiDisplayService.WifiDisplayBinder) service).getService();
				if (mWifiDisplayService != null) {
					mConnectingTextView.setText("\n" + mWifiDisplayService.getName() + "\n\n" + getResources().getString(R.string.connecting_textview));
					mConnectingTextView.setTextColor(Color.parseColor("#ffffff00"));
				}
			}
		}

		public void onServiceDisconnected(ComponentName name) {
			Log.d(TAG, "onServiceDisconnected");
			mWifiDisplayService = null;
		}
	};
}
