package net.fractumseraph.morrowind;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.util.Log;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The app's self-updater, as the 3DS build's (source/mwmp/selfupdate.cpp): the same releases, the same check.
 *
 * A release's build (BuildConfig.MW3DS_RELEASE_BUILD: built from a release's tag, mp-X.Y.Z, by
 * .github/workflows/release.yml) asks GitHub for the latest release when the game starts, on a thread of its own. When
 * it is newer than this one and holds the app (FractumMorrowind-android.apk), the player is asked:
 *
 * - Update: the app is downloaded, checked against the size and SHA-256 GitHub lists for it, and handed to Android's
 *   package installer, which asks to confirm, replaces the game (closing it) and starts the new version
 *   (UpdateReceiver). The game data and the saves stay: they are the app's files, and only the program changes.
 * - Not now: nothing, until the next start.
 * - Never: 3ds/mw3ds/update_off is made, the file that turns the 3DS build's checks off too; deleting it asks again.
 *
 * Every release is signed with the same key (FractumSeraph's, the repository's secrets), which Android checks: an app
 * signed with any other is refused.
 */
final class Updater {
    private static final String LOG_TAG = "mw3ds";
    private static final String LATEST = "https://api.github.com/repos/FractumSeraph/mw3ds/releases/latest";
    /** the release's app (release.yml) */
    static final String ASSET = "FractumMorrowind-android.apk";
    private static final String TITLE = "FractumMorrowind";
    private static final int TIMEOUT_MILLISECONDS = 20000;
    /** the most the app (about 30 MB) or GitHub's answer may be */
    private static final long MAXIMUM_APP_SIZE = 256L * 1024 * 1024;
    private static final long MAXIMUM_ANSWER_SIZE = 4L * 1024 * 1024;

    /** A newer release's app: its version, where it is, how big and its SHA-256 */
    private static final class Release {
        String version, url, sha256;
        long size;
    }

    private Updater() {
    }

    /** the file that turns update checks off, the 3DS build's sdmc:/3ds/mw3ds/update_off */
    private static File offFile(Activity activity) {
        File root = activity.getExternalFilesDir(null);
        return root != null ? new File(root, "3ds/mw3ds/update_off") : null;
    }

    /** At the game's start: looks for a new version, in the background */
    static void start(Activity activity) {
        File off = offFile(activity);
        if (!BuildConfig.MW3DS_RELEASE_BUILD || off == null || off.exists())
            return;
        new Thread(() -> {
            Release release = latestRelease();
            if (release != null)
                activity.runOnUiThread(() -> ask(activity, release));
        }, "update check").start();
    }

    /** Whether version (X.Y.Z) is newer than current, by the numbers */
    static boolean newer(String version, String current) {
        Pattern pattern = Pattern.compile("(\\d+)\\.(\\d+)\\.(\\d+).*");
        Matcher latest = pattern.matcher(version), now = pattern.matcher(current);
        if (!latest.matches() || !now.matches())
            return false;
        for (int part = 1; part <= 3; part++) {
            long a = Long.parseLong(latest.group(part)), b = Long.parseLong(now.group(part));
            if (a != b)
                return a > b;
        }
        return false;
    }

    private static HttpURLConnection open(String address) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
        connection.setConnectTimeout(TIMEOUT_MILLISECONDS);
        connection.setReadTimeout(TIMEOUT_MILLISECONDS);
        connection.setRequestProperty("User-Agent", "FractumMorrowind/" + BuildConfig.VERSION_NAME + " (Android)");
        connection.setInstanceFollowRedirects(true);
        return connection;
    }

    private static byte[] readAll(InputStream stream, long maximum) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[16384];
        int count;
        while ((count = stream.read(buffer)) > 0) {
            out.write(buffer, 0, count);
            if (out.size() > maximum)
                throw new IOException("the answer is larger than expected");
        }
        return out.toByteArray();
    }

    /** GitHub's latest release, when it is newer than this app and holds the app with its SHA-256; else null */
    private static Release latestRelease() {
        try {
            HttpURLConnection connection = open(LATEST);
            connection.setRequestProperty("Accept", "application/vnd.github+json");
            JSONObject json;
            try (InputStream stream = connection.getInputStream()) {
                json = new JSONObject(new String(readAll(stream, MAXIMUM_ANSWER_SIZE), StandardCharsets.UTF_8));
            } finally {
                connection.disconnect();
            }
            String tag = json.optString("tag_name");
            String version = tag.startsWith("mp-") ? tag.substring(3) : tag;
            if (!version.matches("\\d{1,4}\\.\\d{1,4}\\.\\d{1,4}")) {
                Log.i(LOG_TAG, "update: the latest release's tag " + tag + " is not a version");
                return null;
            }
            if (!newer(version, BuildConfig.VERSION_NAME)) {
                Log.i(LOG_TAG, "update: " + BuildConfig.VERSION_NAME + " is the latest");
                return null;
            }
            JSONArray assets = json.optJSONArray("assets");
            for (int i = 0; assets != null && i < assets.length(); i++) {
                JSONObject asset = assets.getJSONObject(i);
                if (!ASSET.equals(asset.optString("name")))
                    continue;
                Release release = new Release();
                release.version = version;
                release.url = asset.optString("browser_download_url");
                release.size = asset.optLong("size");
                String digest = asset.optString("digest");
                release.sha256 = digest.startsWith("sha256:") ? digest.substring(7).toLowerCase(Locale.ROOT) : "";
                if (!release.url.startsWith("https://github.com/") || release.size <= 0
                        || release.size > MAXIMUM_APP_SIZE || !release.sha256.matches("[0-9a-f]{64}")) {
                    Log.i(LOG_TAG, "update: " + version + "'s app has no usable address, size or SHA-256");
                    return null;
                }
                Log.i(LOG_TAG, "update: " + version + " is out (this is " + BuildConfig.VERSION_NAME + ")");
                return release;
            }
            Log.i(LOG_TAG, "update: " + version + " has no Android app");
            return null;
        } catch (Exception e) {
            Log.i(LOG_TAG, "update: could not check for a new version: " + e);
            return null;
        }
    }

    private static void ask(Activity activity, Release release) {
        if (activity.isFinishing())
            return;
        RoundDialog.show(activity, TITLE + " " + release.version, "A new version is out (this is "
            + BuildConfig.VERSION_NAME + "). Update now? The game closes, and your saves and game data stay.", null, false,
            null, new RoundDialog.Choice("Update", () -> update(activity, release)), new RoundDialog.Choice("Not now", null),
            new RoundDialog.Choice("Never", () -> confirmNever(activity)));
    }

    private static void confirmNever(Activity activity) {
        RoundDialog.show(activity, TITLE,
            "Stop asking about new versions? To ask again, delete 3ds/mw3ds/update_off in the app's folder.", null, false,
            null, new RoundDialog.Choice("Stop asking", () -> {
                File off = offFile(activity);
                try {
                    if (off != null && off.getParentFile() != null) {
                        off.getParentFile().mkdirs();
                        new FileOutputStream(off).close();
                    }
                } catch (IOException e) {
                    Log.i(LOG_TAG, "update: could not write " + off + ": " + e);
                }
            }), new RoundDialog.Choice("Keep asking", null));
    }

    private static void update(Activity activity, Release release) {
        LinearLayout layout = new LinearLayout(activity);
        TextView status = new TextView(activity);
        ProgressBar bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        int padding = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16,
            activity.getResources().getDisplayMetrics());
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(padding, padding / 2, padding, 0);
        status.setText("Downloading...");
        bar.setMax(1000);
        layout.addView(status);
        layout.addView(bar);
        AlertDialog progress = RoundDialog.show(activity, TITLE + " " + release.version, null, layout, false, null);

        new Thread(() -> {
            try {
                File directory = new File(activity.getCacheDir(), "update");
                directory.mkdirs();
                File apk = new File(directory, ASSET);
                download(release, apk, (received, total) -> activity.runOnUiThread(() -> {
                    bar.setProgress((int) (received * 1000 / Math.max(1, total)));
                    status.setText("Downloading... " + (received >> 20) + " of " + (total >> 20) + " MB");
                }));
                activity.runOnUiThread(() -> status.setText("Installing..."));
                install(activity, apk);
                activity.runOnUiThread(progress::dismiss);
            } catch (Exception e) {
                Log.i(LOG_TAG, "update: failed: " + e);
                activity.runOnUiThread(() -> {
                    progress.dismiss();
                    RoundDialog.show(activity, TITLE, "The update failed: " + e.getMessage(), null, true, null,
                        new RoundDialog.Choice("OK", null));
                });
            }
        }, "update download").start();
    }

    private interface Progress {
        void report(long received, long total);
    }

    /** The release's app, checked against the size and SHA-256 GitHub lists for it */
    private static void download(Release release, File file, Progress progress) throws IOException {
        HttpURLConnection connection = open(release.url);
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException("no SHA-256 to check the download with");
        }
        long received = 0, reported = 0;
        try {
            int code = connection.getResponseCode();
            if (code != 200)
                throw new IOException("GitHub answered " + code);
            byte[] buffer = new byte[65536];
            try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(file)) {
                int count;
                while ((count = in.read(buffer)) > 0) {
                    received += count;
                    if (received > release.size)
                        throw new IOException("the download is larger than the release says");
                    out.write(buffer, 0, count);
                    digest.update(buffer, 0, count);
                    if (received - reported >= 256 * 1024 || received == release.size) {
                        progress.report(received, release.size);
                        reported = received;
                    }
                }
            }
        } finally {
            connection.disconnect();
        }
        if (received != release.size)
            throw new IOException("the download broke off");
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest())
            hex.append(String.format(Locale.ROOT, "%02x", b));
        if (!hex.toString().equals(release.sha256)) {
            file.delete();
            throw new IOException("the download's SHA-256 is not the release's");
        }
    }

    /**
     * The app to Android's package installer, in a session: it asks the player to confirm (and, the first time, to
     * allow this app to install apps), then replaces the game; the session's outcome comes to UpdateReceiver
     */
    private static void install(Activity activity, File apk) throws IOException {
        PackageInstaller installer = activity.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
            PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(activity.getPackageName());
        params.setSize(apk.length());
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(apk); OutputStream out = session.openWrite(ASSET, 0, apk.length())) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = in.read(buffer)) > 0)
                    out.write(buffer, 0, count);
                session.fsync(out);
            }
            Intent result = new Intent(activity, UpdateReceiver.class).setAction(UpdateReceiver.ACTION_INSTALLED);
            PendingIntent pending = PendingIntent.getBroadcast(activity, id, result,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(pending.getIntentSender());
        }
    }
}
