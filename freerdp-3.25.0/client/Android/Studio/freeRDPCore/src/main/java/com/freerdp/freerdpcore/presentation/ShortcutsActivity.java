/*
   Android Shortcut activity

   Copyright 2013 Thincast Technologies GmbH, Author: Martin Fleisz

   This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
   If a copy of the MPL was not distributed with this file, You can obtain one at
   http://mozilla.org/MPL/2.0/.
*/

package com.freerdp.freerdpcore.presentation;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.TextView;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.pm.ShortcutInfoCompat;
import androidx.core.content.pm.ShortcutManagerCompat;
import androidx.core.graphics.drawable.IconCompat;

import com.freerdp.freerdpcore.R;
import com.freerdp.freerdpcore.application.GlobalApp;
import com.freerdp.freerdpcore.domain.BookmarkBase;
import com.freerdp.freerdpcore.services.SessionRequestHandlerActivity;
import com.freerdp.freerdpcore.utils.BookmarkArrayAdapter;

import java.util.ArrayList;

public class ShortcutsActivity extends AppCompatActivity
{

	public static final String TAG = "ShortcutsActivity";
	private ListView listView;

	@Override public void onCreate(Bundle savedInstanceState)
	{

		super.onCreate(savedInstanceState);

		Intent intent = getIntent();
		if (Intent.ACTION_CREATE_SHORTCUT.equals(intent.getAction()))
		{
			// set listeners for the list view
			listView = new ListView(this);
			setContentView(listView);

			if (getSupportActionBar() != null)
			{
				getSupportActionBar().setDisplayHomeAsUpEnabled(true);
			}

			listView.setOnItemClickListener((parent, view, position, id) -> {
				String refStr = view.getTag().toString();
				String defLabel =
				    ((TextView)(view.findViewById(R.id.bookmark_text1))).getText().toString();
				setupShortcut(refStr, defLabel);
			});
		}
		else
		{
			// just exit
			finish();
		}
	}

	@Override public void onResume()
	{
		super.onResume();
		// create bookmark cursor adapter
		ArrayList<BookmarkBase> bookmarks = GlobalApp.getManualBookmarkGateway().findAll();
		BookmarkArrayAdapter bookmarkAdapter =
		    new BookmarkArrayAdapter(this, android.R.layout.simple_list_item_2, bookmarks);
		listView.setAdapter(bookmarkAdapter);
	}

	@Override public void onPause()
	{
		super.onPause();
		listView.setAdapter(null);
	}

	@Override public boolean onSupportNavigateUp()
	{
		finish();
		return true;
	}

	private void setupShortcut(String strRef, String defaultLabel)
	{
		final String paramStrRef = strRef;
		final String paramDefaultLabel = defaultLabel;

		// display edit dialog to the user so he can specify the shortcut name
		final EditText input = new EditText(this);
		input.setText(defaultLabel);

		new AlertDialog.Builder(this)
		    .setTitle(R.string.dlg_title_create_shortcut)
		    .setMessage(R.string.dlg_msg_create_shortcut)
		    .setView(input)
		    .setPositiveButton(
		        android.R.string.ok,
		        (dialog, which) -> {
			        String label = input.getText().toString();
			        if (label.length() == 0)
				        label = paramDefaultLabel;

			        Intent shortcutIntent = new Intent(Intent.ACTION_VIEW);
			        shortcutIntent.setClassName(this,
			                                    SessionRequestHandlerActivity.class.getName());
			        shortcutIntent.setData(Uri.parse(paramStrRef));

			        // Use ShortcutManagerCompat for modern Android compatibility
			        ShortcutInfoCompat shortcutInfo =
			            new ShortcutInfoCompat.Builder(this, "shortcut_" + paramStrRef.hashCode())
			                .setShortLabel(label)
			                .setIcon(IconCompat.createWithResource(
			                    this, R.drawable.icon_launcher_freerdp))
			                .setIntent(shortcutIntent)
			                .build();

			        // Now, return the result to the launcher
			        Intent resultIntent =
			            ShortcutManagerCompat.createShortcutResultIntent(this, shortcutInfo);
			        setResult(RESULT_OK, resultIntent);
			        finish();
		        })
		    .setNegativeButton(android.R.string.cancel, (dialog, which) -> dialog.dismiss())
		    .show();
	}
}
