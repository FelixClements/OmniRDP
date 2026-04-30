/*
   Activity that displays the help pages

   Copyright 2013 Thincast Technologies GmbH, Author: Martin Fleisz

   This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
   If a copy of the MPL was not distributed with this file, You can obtain one at
   http://mozilla.org/MPL/2.0/.
*/

package com.freerdp.freerdpcore.presentation;

import android.content.res.Configuration;
import android.os.Bundle;
import androidx.appcompat.app.AppCompatActivity;
import android.util.Log;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import com.freerdp.freerdpcore.R;

import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

public class HelpActivity extends AppCompatActivity
{

	private static final String TAG = HelpActivity.class.toString();

	@Override public boolean onSupportNavigateUp()
	{
		finish();
		return true;
	}

	@Override public void onCreate(Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);

		setContentView(R.layout.activity_help);

		if (getSupportActionBar() != null)
		{
			getSupportActionBar().setDisplayHomeAsUpEnabled(true);
		}

		WebView webview = findViewById(R.id.activity_help_webview);

		String filename;
		if ((getResources().getConfiguration().screenLayout &
		     Configuration.SCREENLAYOUT_SIZE_MASK) >= Configuration.SCREENLAYOUT_SIZE_LARGE)
			filename = "gestures.html";
		else
			filename = "gestures_phone.html";

		WebSettings settings = webview.getSettings();
		settings.setDomStorageEnabled(true);
		settings.setUseWideViewPort(true);
		settings.setLoadWithOverviewMode(true);
		settings.setSupportZoom(true);
		settings.setJavaScriptEnabled(true);

		settings.setAllowContentAccess(true);
		settings.setAllowFileAccess(true);

		final Locale def = Locale.getDefault();
		final String prefix = def.getLanguage().toLowerCase(def);

		final String base = "file:///android_asset/";
		final String baseName = "help_page";
		String dir = prefix + "_" + baseName + "/";
		String file = dir + filename;
		InputStream is;
		try
		{
			is = getAssets().open(file);
			is.close();
		}
		catch (IOException e)
		{
			Log.d(TAG, "No localized asset " + file + ", falling back to default");
			dir = baseName + "/";
			file = dir + filename;
		}

		webview.setWebViewClient(new WebViewClient() {
			@Override public boolean shouldOverrideUrlLoading(WebView view, String url)
			{
				if (url.startsWith("file:///android_asset/"))
				{
					view.loadUrl(url);
					return true;
				}
				return false;
			}
		});

		webview.loadUrl(base + file);
	}
}
