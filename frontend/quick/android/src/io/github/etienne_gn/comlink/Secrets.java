package io.github.etienne_gn.comlink;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.util.Log;

import java.nio.charset.StandardCharsets;
import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/* The SIP password, encrypted with a key that never leaves the
 * Android Keystore.
 *
 * Not EncryptedSharedPreferences: androidx.security has been
 * deprecated without a like-for-like replacement, and it would drag
 * the AndroidX dependency tree into an app that otherwise needs
 * nothing but AGP. AES/GCM against a keystore key is what that library
 * did anyway, and on a device with a TEE or StrongBox the key is
 * hardware-backed and cannot be extracted at all.
 *
 * What this does and does not buy, stated plainly because the
 * difference matters for a 911 centre:
 *
 *  - It does protect the password at rest. The prefs file holds
 *    ciphertext, so a backup, an adb pull, a lost handset examined
 *    offline, or another process that can read the file learns
 *    nothing. android:allowBackup="false" is set for the same reason,
 *    and matters doubly here: the keystore key is not backed up, so a
 *    restored ciphertext would be undecryptable anyway.
 *  - It does not protect against code running as this app. Anything
 *    that can execute in this process can ask the keystore to
 *    decrypt, exactly as this class does. Keystore raises the cost of
 *    theft; it does not make the password unreachable.
 *
 * Requiring the user's lock screen per decryption
 * (setUserAuthenticationRequired) would close some of that gap, and it
 * would also stop the foreground service re-registering by itself
 * after Android restarts it -- which is the whole point of storing the
 * password. That trade belongs to whoever deploys this, not here. */
public final class Secrets {

    private static final String TAG = "comlink";
    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String ALIAS = "comlink.secrets.v1";
    private static final String TRANSFORM = "AES/GCM/NoPadding";
    private static final int TAG_BITS = 128;

    private static final String PREFS = "comlink.secrets";

    /* The Qt frontend has no SharedPreferences of its own to pass in,
       so this takes the Context and fetches them. Same file, same
       key, same ciphertext as the Java frontend wrote -- an account
       set up under that build is still readable under this one. */
    private static SharedPreferences prefs(Context ctx) {
        return ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /* Answers whether the keystore took it. A void call could not,
       and store() *clears* the entry when encryption fails -- so a
       keystore failure used to erase the password and report
       success. */
    public static boolean set(Context ctx, String name, String value) {
        return store(prefs(ctx), name, value);
    }

    public static String get(Context ctx, String name) {
        return load(prefs(ctx), name);
    }

    public static void remove(Context ctx, String name) {
        clear(prefs(ctx), name);
    }

    private static SecretKey key() throws Exception {
        KeyStore ks = KeyStore.getInstance(KEYSTORE);
        ks.load(null);
        KeyStore.Entry e = ks.getEntry(ALIAS, null);
        if (e instanceof KeyStore.SecretKeyEntry)
            return ((KeyStore.SecretKeyEntry) e).getSecretKey();

        KeyGenerator g = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
        g.init(new KeyGenParameterSpec.Builder(ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT
                  | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                /* GCM must never reuse an IV with the same key, so the
                   cipher generates one per encryption and it is stored
                   alongside the ciphertext. Supplying our own would be
                   the way to get that wrong. */
                .setRandomizedEncryptionRequired(true)
                .build());
        return g.generateKey();
    }

    /* Stored as iv:ciphertext, both base64. The IV is not a secret;
       its only requirement is that it is never reused. */
    private static boolean store(SharedPreferences p, String name,
                                String value) {
        if (value == null || value.isEmpty()) {
            clear(p, name);
            return true;
        }
        try {
            Cipher c = Cipher.getInstance(TRANSFORM);
            c.init(Cipher.ENCRYPT_MODE, key());
            byte[] ct = c.doFinal(value.getBytes(StandardCharsets.UTF_8));
            /* commit, not apply: the caller is told whether it was
               written, and apply() answers before it has been. */
            return p.edit().putString(name,
                b64(c.getIV()) + ":" + b64(ct)).commit();
        } catch (Exception e) {
            /* Never the value, and never the exception's message
               unexamined: a failure here must not be the thing that
               puts a password in a log. */
            Log.e(TAG, "could not store " + name + ": "
                       + e.getClass().getSimpleName());
            clear(p, name);
            return false;
        }
    }

    /* Empty string when there is nothing stored, or when what is
       stored can no longer be decrypted -- which happens for real: the
       keystore key is dropped when the app's data is cleared, and on
       some devices when the screen lock is removed. The caller's job
       is then to ask again, not to fail. */
    private static String load(SharedPreferences p, String name) {
        String s = p.getString(name, "");
        if (s.isEmpty()) return "";
        int sep = s.indexOf(':');
        if (sep <= 0) return "";
        try {
            Cipher c = Cipher.getInstance(TRANSFORM);
            c.init(Cipher.DECRYPT_MODE, key(),
                   new GCMParameterSpec(TAG_BITS, unb64(s.substring(0, sep))));
            return new String(c.doFinal(unb64(s.substring(sep + 1))),
                              StandardCharsets.UTF_8);
        } catch (Exception e) {
            Log.e(TAG, "could not read " + name + ": "
                       + e.getClass().getSimpleName());
            clear(p, name);
            return "";
        }
    }

    private static void clear(SharedPreferences p, String name) {
        p.edit().remove(name).apply();
    }

    private static String b64(byte[] b) {
        return Base64.encodeToString(b, Base64.NO_WRAP);
    }
    private static byte[] unb64(String s) {
        return Base64.decode(s, Base64.NO_WRAP);
    }

    private Secrets() {}
}
