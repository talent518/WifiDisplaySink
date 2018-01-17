package com.lc.wifidisplaysink;

import android.content.BroadcastReceiver;  
import android.content.Context;  
import android.content.Intent;  
import android.util.Log;  
 
public class BootBroadcastReceiver extends BroadcastReceiver {  
   //重写onReceive方法  
   @Override  
   public void onReceive(Context context, Intent intent) {  
		Log.v("WifiDisplayService", "开机自动服务自动启动.....");  

		Intent service = new Intent(context, WifiDisplayService.class);  
		context.startService(service);
	}
}
