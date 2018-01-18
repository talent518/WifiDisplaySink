package com.lc.wifidisplaysink;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.NetworkInfo;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.p2p.WifiP2pDevice;
import android.net.wifi.p2p.WifiP2pGroup;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.net.wifi.p2p.WifiP2pManager.ActionListener;
import android.net.wifi.p2p.WifiP2pManager.Channel;
import android.net.wifi.p2p.WifiP2pManager.ChannelListener;
import android.net.wifi.p2p.WifiP2pWfdInfo;
import android.os.Build;
import android.os.Binder;
import android.os.IBinder;
import android.os.Environment;
import android.os.Handler;
import android.provider.Settings;
import android.util.Base64;
import android.util.Log;
import android.widget.Toast;

public class WifiDisplayService extends Service {
	private static final String TAG = "WifiDisplayService";

	private BroadcastReceiver mReceiver;
	private WifiP2pManager mWifiP2pManager;
	private Channel mChannel;
	private List<WifiP2pDevice> mPeers = new ArrayList<WifiP2pDevice>();
	private final Handler mHandler = new Handler();

	private boolean mIsWiFiDirectEnabled;
	private boolean mConnected;
	private int mArpRetryCount = 0;
	private static final int MAX_ARP_RETRY_COUNT = 300;
	private int mP2pControlPort = -1;
	private String mP2pInterfaceName;
	private boolean mIsSinkOpened;

	private WifiManager mWifiManager;

	private String mName = null;

	public String getName() {
		return mName;
	}

	private Runnable mConnectingChecker = new Runnable() {
		@Override
		public void run() {
			p2pDiscoverPeers();
			
			mHandler.postDelayed(mConnectingChecker, 1*1000*30); //TODO: to give a resonable check time.
		}
	};

	private Runnable mRarpChecker = new Runnable() {
		@Override
		public void run() {
			RarpImpl rarp = new RarpImpl();
			String sourceIp = rarp.execRarp(mP2pInterfaceName);

			if (sourceIp == null) {
				if (++mArpRetryCount > MAX_ARP_RETRY_COUNT) {
					Log.e(TAG, "failed to do RARP, no source IP found. still trying ....");
				}
				mHandler.postDelayed(mRarpChecker, 200);
			} else {
				if (!mIsSinkOpened) {
					startWifiDisplaySinkActivity(sourceIp, mP2pControlPort);
				}
			}
		}
	};

	@Override
	public void onCreate() {
		Log.d(TAG, "onCreate()");
		super.onCreate();

		File file = new File(Environment.getDataDirectory(), getPackageName() + ".p2p");
		Log.v(TAG, file.getAbsolutePath());
		if(!file.exists()) {
			try {
				new FileOutputStream(file).close();
			} catch (IOException e) {
				e.printStackTrace();
			}
		}

		long i = file.lastModified();
		byte[] bytes = new byte[] {
			(byte) ((i >> 40) & 0xFF),
			(byte) ((i >> 32) & 0xFF),
			(byte) ((i >> 24) & 0xFF),
					(byte) ((i >> 16) & 0xFF),
					(byte) ((i >> 8) & 0xFF),
					(byte) (i & 0xFF)
		};

		mName = Build.MODEL + "-" + Base64.encodeToString(bytes, Base64.DEFAULT).substring(2, 8);

		utilsCheckP2pFeature();  // final WifiManager

		registerBroadcastReceiver();
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		Log.d(TAG, "onStartCommand()");
		onStart(intent, startId);
		return START_NOT_STICKY;
	}

	@Override
	public void onDestroy() {
		Log.d(TAG, "onDestroy()");
		super.onDestroy();

		Intent intent = new Intent(this, WifiDisplayService.class);
		startService(intent);
	}

	 //Because we know this service always
	 //runs in the same process as its clients, we don't need to deal with
	 //IPC.
	public class WifiDisplayBinder extends Binder {
		WifiDisplayService getService() {
			Log.d(TAG, "getService()");
			return WifiDisplayService.this;
		}
	}

	private final IBinder mBinder = new WifiDisplayBinder();

	@Override
	public IBinder onBind(Intent intent) {
		Log.d(TAG, "onBind()");
		return mBinder;
	}

	private void registerBroadcastReceiver() {
		if (mReceiver == null) {
			Log.d(TAG, "registerBroadcastReceiver()");
			IntentFilter filter = new IntentFilter();
			filter.addAction(WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION);
			filter.addAction(WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION);
			filter.addAction(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION);
			filter.addAction(WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION);
			mReceiver = new WiFiDirectBroadcastReceiver();
			registerReceiver(mReceiver, filter);
		}
	}

	private void unRegisterBroadcastReceiver() {
		if (mReceiver != null) {
			Log.d(TAG, "unRegisterBroadcastReceiver()");
			unregisterReceiver(mReceiver);
			mReceiver = null;
		}
	}


	private void utilsToastLog(String msg1, String msg2) {
		String log = msg1 + System.getProperty("line.separator") + msg2;
		Toast.makeText(this, log, Toast.LENGTH_SHORT).show();
	}

	private String utilsGetAndroidID() {
		return Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
	}

	private String utilsGetMACAddress() {
		WifiInfo wifiInfo = mWifiManager.getConnectionInfo();
		String mac = wifiInfo.getMacAddress();
		return mac;
	}

	private void utilsCheckP2pFeature() {
		Log.d(TAG, "utilsCheckP2pFeature()");
		Log.d(TAG, "ANDROID_ID: " + utilsGetAndroidID());

		mWifiManager = (WifiManager) getSystemService(Context.WIFI_SERVICE);

		if (mWifiManager == null) {
			Log.e(TAG, "we'll exit because, mWifiManager is null");
		}

		if (!mWifiManager.isWifiEnabled()) {
			if (!mWifiManager.setWifiEnabled(true)) {
				utilsToastLog("Warning", "Cannot enable wifi");
			}
		}

		Log.d(TAG, "MAC: " + utilsGetMACAddress());

		if (!p2pIsSupported()) {
			utilsToastLog("Warning", "This Package Does Not Supports P2P Feature!!");
			return;
		}
	}

	private boolean p2pIsSupported() {
		return getPackageManager().hasSystemFeature(PackageManager.FEATURE_WIFI_DIRECT);
	}

	private boolean p2pIsNull() {
		if (!mIsWiFiDirectEnabled) {
			Log.d(TAG, " Wi-Fi Direct is OFF! try Setting Menu");
			return true;
		}

		if (mWifiP2pManager == null) {
			Log.d(TAG, " mWifiP2pManager is NULL! try getSystemService");
			return true;
		}
		if (mChannel == null) {
			Log.d(TAG, " mChannel is NULL! try initialize");
			return true;
		}

		return false;
	}

	//NOTE: should call this before other p2p operation.
	public void p2pInitialize() {
		Log.d(TAG, "p2pInitialize()");

		if (mWifiP2pManager != null) {
			Log.d(TAG, "p2p manager have been initialized, please recheck the calling sequence.");
			return;
		}

		mWifiP2pManager = (WifiP2pManager) getSystemService(Context.WIFI_P2P_SERVICE);
		mChannel = mWifiP2pManager.initialize(this, getMainLooper(), new ChannelListener() {
			public void onChannelDisconnected() {
				Log.d(TAG, "ChannelListener: onChannelDisconnected()");
			}
		});

		Log.d(TAG, "P2P Channel: "+ mChannel );

		mWifiP2pManager.setDeviceName(mChannel,
				 mName,
				 new WifiP2pManager.ActionListener() {
			 public void onSuccess() {
				 Log.d(TAG, " device rename success");
			 }
			 public void onFailure(int reason) {
				 Log.d(TAG, " Failed to set device name");
			 }
		 });

		mWifiP2pManager.setMiracastMode(WifiP2pManager.MIRACAST_SINK);

		WifiP2pWfdInfo wfdInfo = new WifiP2pWfdInfo();
		wfdInfo.setWfdEnabled(true);
		wfdInfo.setDeviceType(WifiP2pWfdInfo.PRIMARY_SINK);
		wfdInfo.setSessionAvailable(true);
		wfdInfo.setControlPort(7236);
		wfdInfo.setMaxThroughput(50);

		mWifiP2pManager.setWFDInfo(mChannel, wfdInfo, new ActionListener() {
			@Override
			public void onSuccess() {
				Log.d(TAG, "Successfully set WFD info.");
			}
			@Override
			public void onFailure(int reason) {
				Log.d(TAG, "Failed to set WFD info with reason " + reason + ".");
			}
		});
	}

	public void p2pDiscoverPeers() {
		Log.d(TAG, "p2pDiscoverPeers()");
		if (p2pIsNull()) {
			Log.w(TAG, "should call p2p initialize at first.");
			return;
		}

		mWifiP2pManager.discoverPeers(mChannel, new ActionListener() {
			@Override
			public void onSuccess() {
				Log.d(TAG, "Successfully discoverPeers.");
			}
			@Override
			public void onFailure(int reason) {
				Log.d(TAG, "Failed to discoverPeers with reason " + reason + ".");
			}
		});
	}

	protected void cleanWifiP2pManager() {
		Log.d(TAG, "cleanWifiP2pManager()");
		if (mWifiP2pManager != null) {
			mWifiP2pManager.cancelConnect(mChannel, null);
			mWifiP2pManager.clearLocalServices(mChannel, null);
			mWifiP2pManager.removeGroup(mChannel, null);
			mWifiP2pManager.stopPeerDiscovery(mChannel, null);
			mWifiP2pManager.discoverServices(mChannel, null);
			mWifiP2pManager.discoverPeers(mChannel, null);
			mWifiP2pManager = null;
			mChannel = null;

		}
		p2pDiscoverPeers();
	}

	public boolean p2pDeviceIsConnected(WifiP2pDevice device) {
		if (device == null) {
			return false;
		}
		return device.status == WifiP2pDevice.CONNECTED;
	}

	public class WiFiDirectBroadcastReceiver extends BroadcastReceiver {

		@Override
		public void onReceive(Context context, Intent intent) {
			String action = intent.getAction();
			Log.d(TAG, "Received Broadcast: "+action+"");

			if (WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION.equals(action)) {
				onStateChanged(context, intent);
			} else if (WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION.equals(action)) {
				onPeersChanged(context, intent);
			} else if (WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION.equals(action)) {
				onConnectionChanged(context, intent);
			} else if (WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION.equals(action)) {
				onDeviceChanged(context, intent);
			}
		}

		private void onStateChanged(Context context, Intent intent) {
			Log.d(TAG, "***onStateChanged");

			mIsWiFiDirectEnabled = false;
			int state = intent.getIntExtra(WifiP2pManager.EXTRA_WIFI_STATE, -1);
			switch (state) {
			case WifiP2pManager.WIFI_P2P_STATE_ENABLED:
				Log.d(TAG, "state: WIFI_P2P_STATE_ENABLED");
				mIsWiFiDirectEnabled = true;
				p2pInitialize();
				break;
			case WifiP2pManager.WIFI_P2P_STATE_DISABLED:
				Log.d(TAG, "state: WIFI_P2P_STATE_DISABLED");
				cleanWifiP2pManager();
				break;
			default:
				Log.d(TAG, "state: " + state);
				break;
			}
		}

		private void onPeersChanged(Context context, Intent intent) {
			Log.d(TAG, "***onPeersChanged");
			//intent.getParcelableExtra(WifiP2pManager.EXTRA_P2P_DEVICE_LIST); or requestPeers anytime
		}

		private void onConnectionChanged(Context context, Intent intent)  {
			Log.d(TAG, "***onConnectionChanged");

			WifiP2pInfo wifiP2pInfo = (WifiP2pInfo) intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_INFO);
			Log.d(TAG, "WifiP2pInfo:");
			Log.d(TAG, "---------------------------------");
			Log.d(TAG, wifiP2pInfo.toString());
			Log.d(TAG, "=================================");

			NetworkInfo networkInfo = (NetworkInfo) intent.getParcelableExtra(WifiP2pManager.EXTRA_NETWORK_INFO);
			Log.d(TAG, "NetworkInfo:");
			Log.d(TAG, "---------------------------------");
			Log.d(TAG, networkInfo.toString());
			Log.d(TAG, "=================================");

			WifiP2pGroup wifiP2pGroupInfo = (WifiP2pGroup) intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_GROUP);
			Log.d(TAG, "WifiP2pGroup:");
			Log.d(TAG, "---------------------------------");
			Log.d(TAG, wifiP2pGroupInfo.toString());
			Log.d(TAG, "=================================");

			if (!networkInfo.isConnected()) {
				if (mConnected) {
					mConnected = false;
					Log.d(TAG, "disconnected");
					unRegisterBroadcastReceiver();
					cleanWifiP2pManager();
					registerBroadcastReceiver();
					mHandler.removeCallbacks(mConnectingChecker);
				}
			}

			if (networkInfo.isConnected()) {
				Log.d(TAG, "connected");
				if (!mConnected) {
					mConnected = true;
					Log.d(TAG, "removeCallbacks --- mConnectingChecker");
					mHandler.removeCallbacks(mConnectingChecker);
					if (!mIsSinkOpened) {
						startWfdSink(context, intent);
					}
				}
			}
		}

		private void onDeviceChanged(Context context, Intent intent) {
			Log.d(TAG, "***onDeviceChanged");

			mHandler.removeCallbacks(mRarpChecker);

			WifiP2pDevice device = (WifiP2pDevice) intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_DEVICE);
			Log.d(TAG, "WifiP2pDevice:");
			Log.d(TAG, device.toString());

			if (mIsWiFiDirectEnabled && !p2pDeviceIsConnected(device)) {
				if (!mConnected) {
					Log.d(TAG, "start connecting checker --- do p2pDiscoverPeers");
					mHandler.postDelayed(mConnectingChecker, 100);
				}
			}
		}

	}

	private String getStatus(int status) {
		switch(status) {
			case WifiP2pDevice.AVAILABLE:
				return "AVAILABLE";
			case WifiP2pDevice.CONNECTED:
				return "CONNECTED";
			case WifiP2pDevice.FAILED:
				return "FAILED";
			case WifiP2pDevice.INVITED:
				return "INVITED";
			case WifiP2pDevice.UNAVAILABLE:
				return "UNAVAILABLE";
		}

		return null;
	}

	private void startWfdSink(Context context, Intent intent) {
		WifiP2pInfo wifiP2pInfo = (WifiP2pInfo) intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_INFO);
		WifiP2pGroup wifiP2pGroupInfo = (WifiP2pGroup) intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_GROUP);

		mP2pControlPort = 7236;
		Collection<WifiP2pDevice> p2pDevs = wifiP2pGroupInfo.getClientList();
		for (WifiP2pDevice dev : p2pDevs) {
			Log.d(TAG, "wifiP2pGroupInfo.getClientList(): deviceAddress = " + dev.deviceAddress + ", deviceName = " + dev.deviceName + ", primaryDeviceType = " + dev.primaryDeviceType + ", secondaryDeviceType = " + dev.secondaryDeviceType + ", status = " + getStatus(dev.status));
			WifiP2pWfdInfo wfd = dev.wfdInfo;
			if (wfd != null && wfd.isWfdEnabled()) {
				int type = wfd.getDeviceType();
				if (type == WifiP2pWfdInfo.WFD_SOURCE || type == WifiP2pWfdInfo.SOURCE_OR_PRIMARY_SINK) {
					mP2pControlPort = wfd.getControlPort();
					Log.d(TAG, "got source port: " + mP2pControlPort);
					break;
				}
			} else {
				continue;
			}
		}

		if (wifiP2pGroupInfo.isGroupOwner()) {
			Log.d(TAG, "GO gets password: " + wifiP2pGroupInfo.getPassphrase());

			//Log.d(TAG, "isGroupOwner, G.O. don't know client IP, so check /proc/net/arp ");
			// it's a pity that the MAC (dev.deviceAddress) is not the one in arp table
			mP2pInterfaceName = wifiP2pGroupInfo.getInterface();
			Log.d(TAG, "GO gets p2p interface: " + mP2pInterfaceName);
			mHandler.postDelayed(mRarpChecker, 200);
		} else {
			mHandler.removeCallbacks(mRarpChecker);

			Log.d(TAG, "Client couldn't get password");

			String sourceIp = wifiP2pInfo.groupOwnerAddress.getHostAddress();
			Log.d(TAG, "Client gets GO's IP: " + sourceIp);

			startWifiDisplaySinkActivity(sourceIp, mP2pControlPort);
		}
	}

	private final void startWifiDisplaySinkActivity(String sourceAddr, int sourcePort) {
		Log.d(TAG, "source port: " + sourcePort + "   source ip addr:  " + sourceAddr);

		Intent intent = new Intent(this, WifiDisplaySinkActivity.class);
		intent.putExtra(WifiDisplaySinkConstants.SOURCE_PORT, sourcePort);
		intent.putExtra(WifiDisplaySinkConstants.SOURCE_ADDR, sourceAddr);
		intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

		startActivity(intent);
	}

}
