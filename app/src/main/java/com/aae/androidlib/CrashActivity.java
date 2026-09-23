package com.aae.androidlib;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

/** Shows the crash report captured by AaeApp. Copy button puts it on the clipboard. */
public class CrashActivity extends Activity {

    public static final String EXTRA_REPORT = "report";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_crash);

        String report = getIntent().getStringExtra(EXTRA_REPORT);
        if (report == null) {
            report = "(no report)";
        }
        final String finalReport = report;

        TextView tvReport = (TextView) findViewById(R.id.tvCrashReport);
        tvReport.setText(finalReport);

        ((Button) findViewById(R.id.btnCopy)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        ClipboardManager cm = (ClipboardManager)
                                getSystemService(Context.CLIPBOARD_SERVICE);
                        cm.setPrimaryClip(ClipData.newPlainText("aae-crash", finalReport));
                        Toast.makeText(CrashActivity.this,
                                "Crash report copied", Toast.LENGTH_SHORT).show();
                    }
                });

        ((Button) findViewById(R.id.btnClose)).setOnClickListener(
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        finish();
                    }
                });
    }

    @Override
    public void onBackPressed() {
        finish();
    }
}
