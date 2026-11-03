package net.fractumseraph.morrowind;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.util.Log;
import android.widget.Toast;

import java.io.File;

/**
 * The package installer's answers about an update (Updater.install): it needs the player to confirm (its screen is
 * opened), or it failed (said so). Then, once the new version has replaced the app, its download goes and the game is
 * started again (Android lets an app start itself from here only in some cases; otherwise the installer's Open does).
 */
public class UpdateReceiver extends BroadcastReceiver {
    static final String ACTION_INSTALLED = "net.fractumseraph.morrowind.UPDATE_INSTALLED";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (ACTION_INSTALLED.equals(intent.getAction())) {
            int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
            if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirm != null) {
                    confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    context.startActivity(confirm);
                }
            } else if (status != PackageInstaller.STATUS_SUCCESS) {
                String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                Log.i("mw3ds", "update: the installer refused it (" + status + "): " + message);
                Toast.makeText(context, "The update was not installed" + (message != null ? ": " + message : ""),
                    Toast.LENGTH_LONG).show();
            }
            return;
        }
        if (!Intent.ACTION_MY_PACKAGE_REPLACED.equals(intent.getAction()))
            return;
        new File(new File(context.getCacheDir(), "update"), Updater.ASSET).delete();
        try {
            Intent start = new Intent(context, MorrowindActivity.class);
            start.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(start);
        } catch (RuntimeException e) {
            // not allowed from the background here: the installer's Open button starts it
        }
    }
}
