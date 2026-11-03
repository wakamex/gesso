package dev.wakamex.gesso;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Log;
import java.security.KeyStore;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

// gs_secret on Android (src/gs_secret.c): secrets sealed with an AES key kept in the Android
// Keystore, which never leaves it. A sealed secret is the 12-byte nonce, then the AES-GCM ciphertext.
public final class Secret {
    private static final String ALIAS = "dev.wakamex.gesso.secret";

    private static synchronized SecretKey key() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        java.security.Key k = store.getKey(ALIAS, null);
        if (k instanceof SecretKey) return (SecretKey)k;
        KeyGenerator g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        g.init(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                   .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                   .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                   .build());
        return g.generateKey();
    }

    // null when the Keystore cannot be used
    public static byte[] seal(byte[] plain) {
        try {
            Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
            c.init(Cipher.ENCRYPT_MODE, key());
            byte[] iv = c.getIV(), body = c.doFinal(plain), out = new byte[iv.length + body.length];
            System.arraycopy(iv, 0, out, 0, iv.length);
            System.arraycopy(body, 0, out, iv.length, body.length);
            return iv.length == 12 ? out : null;
        } catch (Exception e) {
            Log.w("gesso", "the Keystore did not seal a secret", e);
            return null;
        }
    }

    // null when it does not open (another key, a damaged file)
    public static byte[] open(byte[] sealed) {
        try {
            Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
            c.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(128, sealed, 0, 12));
            return c.doFinal(sealed, 12, sealed.length - 12);
        } catch (Exception e) {
            Log.w("gesso", "the Keystore did not open a secret", e);
            return null;
        }
    }
}
