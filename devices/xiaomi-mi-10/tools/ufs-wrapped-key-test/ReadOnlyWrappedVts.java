import java.io.DataInputStream;
import java.io.OutputStream;
import java.lang.reflect.Array;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.SecureRandom;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

public class ReadOnlyWrappedVts {
    static final String KS = "android.system.keystore2.";
    static final String KM = "android.hardware.security.keymint.";
    static final String SERVICE = "android.system.keystore2.IKeystoreService/default";
    static final int BLOCK = 16, SECTOR = 4096, SAMPLES = 256;

    static int namedInt(String type, String member) throws Exception {
        return Class.forName(type).getField(member).getInt(null);
    }

    static Object param(String tag, String methodName, Class<?> argumentType, Object value)
            throws Exception {
        Class<?> paramType = Class.forName(KM + "KeyParameter");
        Class<?> valueType = Class.forName(KM + "KeyParameterValue");
        Object p = paramType.getConstructor().newInstance();
        paramType.getField("tag").setInt(p, namedInt(KM + "Tag", tag));
        paramType.getField("value").set(p, valueType.getMethod(methodName, argumentType)
                                               .invoke(null, value));
        return p;
    }

    static byte[][] wrapSyntheticKey(byte[] key, boolean generated, boolean standardTag, boolean iceTag)
            throws Exception {
        Class<?> binder = Class.forName("android.os.IBinder");
        Object handle = Class.forName("android.os.ServiceManager").getMethod("getService", String.class)
                             .invoke(null, SERVICE);
        if (handle == null) throw new IllegalStateException("Keystore2 service missing");
        Class<?> service = Class.forName(KS + "IKeystoreService");
        Object api = Class.forName(KS + "IKeystoreService$Stub").getMethod("asInterface", binder)
                          .invoke(null, handle);
        Object tee = service.getMethod("getSecurityLevel", int.class)
                            .invoke(api, namedInt(KM + "SecurityLevel", "TRUSTED_ENVIRONMENT"));
        Class<?> getSecurityLevel = Class.forName(KS + "IKeystoreSecurityLevel");
        Class<?> descriptorType = Class.forName(KS + "KeyDescriptor");
        Object desc = descriptorType.getConstructor().newInstance();
        descriptorType.getField("domain").setInt(desc, namedInt(KS + "Domain", "BLOB"));
        descriptorType.getField("nspace").setLong(desc, 0L);

        int parameterCount = 4 + (standardTag ? 1 : 0) + (iceTag ? 1 : 0);
        Object args = Array.newInstance(Class.forName(KM + "KeyParameter"), parameterCount);
        Array.set(args, 0, param("ALGORITHM", "algorithm", int.class, namedInt(KM + "Algorithm", "AES")));
        Array.set(args, 1, param("KEY_SIZE", "integer", int.class, key.length * 8));
        Array.set(args, 2, param("PURPOSE", "keyPurpose", int.class, namedInt(KM + "KeyPurpose", "ENCRYPT")));
        Array.set(args, 3, param("PURPOSE", "keyPurpose", int.class, namedInt(KM + "KeyPurpose", "DECRYPT")));
        if (standardTag)
            Array.set(args, 4, param("STORAGE_KEY", "boolValue", boolean.class, true));
        // The stock UMI vold adds the FBE ICE tag independently of rollback resistance.
        if (iceTag) {
            Class<?> keyParameter = Class.forName(KM + "KeyParameter");
            Object ice = keyParameter.getConstructor().newInstance();
            keyParameter.getField("tag").setInt(ice, 0x70003f49);
            keyParameter.getField("value").set(ice, Class.forName(KM + "KeyParameterValue")
                .getMethod("boolValue", boolean.class).invoke(null, true));
            Array.set(args, standardTag ? 5 : 4, ice);
        }
        System.out.println("Requested STORAGE_KEY=" + standardTag + ", FBE_ICE=" + iceTag
            + ", rollback resistance=false");
        // AOSP's BLOB path without rollback resistance. All key material stays in memory.
        Object metadata;
        if (generated) {
            Method generateKey = getSecurityLevel.getMethod("generateKey", descriptorType,
                descriptorType, args.getClass(), int.class, byte[].class);
            metadata = generateKey.invoke(tee, desc, null, args, 0, new byte[0]);
        } else {
            Method importKey = getSecurityLevel.getMethod("importKey", descriptorType, descriptorType,
                args.getClass(), int.class, byte[].class);
            metadata = importKey.invoke(tee, desc, null, args, 0, key);
        }
        Object blob = metadata.getClass().getField("key").get(metadata);
        byte[] longTerm = (byte[])descriptorType.getField("blob").get(blob);
        if (longTerm == null || longTerm.length == 0) throw new IllegalStateException("Empty test blob");
        System.out.println("Keystore2 " + (generated ? "generated" : "imported") +
            " a synthetic TEE storage key in BLOB mode");
        Object auth = metadata.getClass().getField("authorizations").get(metadata);
        int rollbackTag = namedInt(KM + "Tag", "ROLLBACK_RESISTANCE");
        if (auth == null) throw new IllegalStateException("Key has no reported authorizations");
        for (int i = 0; i < Array.getLength(auth); i++) {
            Object param = Array.get(auth, i).getClass().getField("keyParameter").get(Array.get(auth, i));
            if (param.getClass().getField("tag").getInt(param) == rollbackTag)
                throw new IllegalStateException("Key unexpectedly has rollback resistance");
        }
        System.out.println("Returned authorizations have no rollback-resistance tag");
        try {
            Method export = getSecurityLevel.getMethod("convertStorageKeyToEphemeral", descriptorType);
            byte[][] keys = new byte[2][];
            for (int i = 0; i < keys.length; i++) {
                Object response = export.invoke(tee, blob);
                keys[i] = (byte[])response.getClass().getField("ephemeralKey").get(response);
                if (keys[i] == null || keys[i].length < 64 || keys[i].length > 128)
                    throw new IllegalStateException("Unexpected ephemeral test key length");
            }
            System.out.println("Keystore2 exported the test key twice in ephemeral wrapped form");
            return keys;
        } finally {
            Arrays.fill(longTerm, (byte)0);
        }
    }

    static Cipher aes(byte[] key) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/ECB/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"));
        return cipher;
    }

    static byte[] cmacDouble(byte[] in) {
        byte[] out = new byte[BLOCK];
        int carry = 0;
        for (int i = BLOCK - 1; i >= 0; i--) {
            int next = in[i] & 255;
            out[i] = (byte)((next << 1) | carry);
            carry = next >>> 7;
        }
        if (carry != 0) out[BLOCK - 1] ^= (byte)0x87;
        return out;
    }

    static byte[] cmac(byte[] key, byte[] data) throws Exception {
        Cipher cipher = aes(key);
        byte[] state = new byte[BLOCK];
        byte[] subkey = cmacDouble(cipher.doFinal(state));
        int last = Math.max(0, (data.length - 1) / BLOCK * BLOCK);
        for (int i = 0; i < last; i += BLOCK) {
            for (int j = 0; j < BLOCK; j++) state[j] ^= data[i + j];
            state = cipher.doFinal(state);
        }
        int n = data.length - last;
        if (n != BLOCK) {
            subkey = cmacDouble(subkey);
            state[n] ^= (byte)0x80;
        }
        for (int j = 0; j < n; j++) state[j] ^= data[last + j];
        for (int j = 0; j < BLOCK; j++) state[j] ^= subkey[j];
        return cipher.doFinal(state);
    }

    static byte[] android13Kdf(byte[] key) throws Exception {
        // Android 13 AOSP VTS inline encryption key label and context (SP 800-108).
        byte[] label = {0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0x20};
        byte[] context = {
            'i','n','l','i','n','e',' ','e','n','c','r','y','p','t','i','o',
            'n',' ','k','e','y',0,0,0,0,0,0,2,0x43,0,(byte)0x82,0x50,0,0,0,0
        };
        byte[] result = new byte[64];
        for (int i = 0; i < 4; i++) {
            ByteBuffer input = ByteBuffer.allocate(4 + label.length + 1 + context.length + 4);
            input.putInt(i + 1).put(label).put((byte)0).put(context).putInt(512);
            System.arraycopy(cmac(key, input.array()), 0, result, i * BLOCK, BLOCK);
        }
        return result;
    }

    static void xtsMultiply(byte[] tweak) {
        int carry = 0;
        for (int i = 0; i < BLOCK; i++) {
            int next = tweak[i] & 255;
            tweak[i] = (byte)((next << 1) | carry);
            carry = next >>> 7;
        }
        if (carry != 0) tweak[0] ^= (byte)0x87;
    }

    static int sectorsMatching(byte[] raw, byte[] data, byte[] key) throws Exception {
        Cipher aesData = aes(Arrays.copyOfRange(key, 0, 32));
        Cipher aesTweak = aes(Arrays.copyOfRange(key, 32, 64));
        byte[] in = new byte[BLOCK], out = new byte[BLOCK], iv = new byte[BLOCK];
        ByteBuffer dun = ByteBuffer.wrap(iv).order(ByteOrder.LITTLE_ENDIAN);
        int good = 0;
        for (int i = 0; i < SAMPLES; i++) {
            dun.putLong(0, i);
            byte[] tweak = aesTweak.doFinal(iv);
            int end = (i + 1) * SECTOR;
            boolean same = true;
            for (int offset = i * SECTOR; offset < end; offset += BLOCK) {
                for (int j = 0; j < BLOCK; j++) in[j] = (byte)(data[offset + j] ^ tweak[j]);
                aesData.doFinal(in, 0, BLOCK, out, 0);
                for (int j = 0; j < BLOCK; j++)
                    if ((byte)(out[j] ^ tweak[j]) != raw[offset + j]) same = false;
                xtsMultiply(tweak);
            }
            if (same) good++;
        }
        return good;
    }

    static String hex(byte[] bytes) {
        final char[] chars = "0123456789abcdef".toCharArray();
        char[] out = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            out[2 * i] = chars[(bytes[i] & 255) >>> 4];
            out[2 * i + 1] = chars[bytes[i] & 15];
        }
        return new String(out);
    }

    static final class Sample implements AutoCloseable {
        final byte[] raw = new byte[SAMPLES * SECTOR], data = new byte[SAMPLES * SECTOR];
        @Override public void close() {
            Arrays.fill(raw, (byte)0);
            Arrays.fill(data, (byte)0);
        }
    }

    static Sample readSample(String helper, String token, byte[] hardwareKey) throws Exception {
        Sample sample = new Sample();
        byte[] input = (hex(hardwareKey) + "\n").getBytes("US-ASCII");
        Process process = new ProcessBuilder(helper, token)
            .redirectError(ProcessBuilder.Redirect.INHERIT).start();
        boolean ok = false;
        try {
            try (OutputStream pipe = process.getOutputStream()) { pipe.write(input); }
            try (DataInputStream pipe = new DataInputStream(process.getInputStream())) {
                pipe.readFully(sample.raw);
                pipe.readFully(sample.data);
                if (pipe.read() != -1) throw new IllegalStateException("Extra readback data");
            }
            if (process.waitFor() != 0) throw new IllegalStateException("wrapped readback helper failed");
            if (Arrays.equals(sample.raw, sample.data)) throw new IllegalStateException("Readback did not decrypt data");
            ok = true;
            return sample;
        } finally {
            process.destroy();
            Arrays.fill(input, (byte)0);
            if (!ok) sample.close();
        }
    }

    static int differingSectors(byte[] a, byte[] b) {
        int n = 0;
        for (int i = 0; i < SAMPLES; i++) {
            for (int off = i * SECTOR; off < (i + 1) * SECTOR; off++) {
                if (a[off] != b[off]) {
                    n++;
                    break;
                }
            }
        }
        return n;
    }

    static void testGenerated(String helper, String token, byte[] synthetic) throws Exception {
        byte[][] a = wrapSyntheticKey(synthetic, true, true, true);
        byte[][] b = wrapSyntheticKey(synthetic, true, true, true);
        try (Sample first = readSample(helper, token, a[0]);
             Sample repeated = readSample(helper, token, a[1]);
             Sample different = readSample(helper, token, b[0])) {
            if (!Arrays.equals(first.raw, repeated.raw) || !Arrays.equals(first.raw, different.raw))
                throw new IllegalStateException("Ciphertext changed between mappings");
            int sameKey = SAMPLES - differingSectors(first.data, repeated.data);
            int differentKey = differingSectors(first.data, different.data);
            int changedFromRaw = differingSectors(first.data, first.raw);
            System.out.println("Raw UFS ciphertext stayed identical across all maps");
            System.out.println("Same key, two new mappings: " + sameKey + "/" + SAMPLES + " sectors agree");
            System.out.println("Independent generated key: " + differentKey + "/" + SAMPLES + " sectors differ");
            System.out.println("Mapped output vs raw UFS: " + changedFromRaw + "/" + SAMPLES + " sectors differ");
            if (sameKey != SAMPLES || differentKey != SAMPLES || changedFromRaw != SAMPLES)
                throw new IllegalStateException("Key/control comparison did not pass");
            System.out.println("GENERATED_WRAPPED_READ_ONLY_CONTROL_PASS");
        } finally {
            for (byte[] key : a) Arrays.fill(key, (byte)0);
            for (byte[] key : b) Arrays.fill(key, (byte)0);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("helper path and test token required");
        byte[] synthetic = new byte[32];
        new SecureRandom().nextBytes(synthetic);
        boolean oraclePass = false;
        try {
            for (boolean std : new boolean[] {true, false}) {
                byte[][] wrapped;
                try {
                    wrapped = wrapSyntheticKey(synthetic, false, std, true);
                } catch (InvocationTargetException e) {
                    if (!String.valueOf(e.getCause()).contains("code -17")) throw e;
                    System.err.println("Known imported seed could not be exported: " + e.getCause());
                    continue;
                }
                byte[] derived = android13Kdf(synthetic);
                try (Sample sample = readSample(args[0], args[1], wrapped[0])) {
                    int matched = sectorsMatching(sample.raw, sample.data, derived);
                    System.out.println("AOSP independent AES-XTS reference: " + matched + "/" + SAMPLES + " sectors agree");
                    oraclePass = matched == SAMPLES;
                    if (oraclePass) break;
                } finally {
                    Arrays.fill(derived, (byte)0);
                    for (byte[] key : wrapped) Arrays.fill(key, (byte)0);
                }
            }
            try {
                byte[][] baseline = wrapSyntheticKey(synthetic, true, true, false);
                for (byte[] key : baseline) Arrays.fill(key, (byte)0);
                throw new IllegalStateException("Unexpected successful export without vendor ICE");
            } catch (InvocationTargetException e) {
                if (!String.valueOf(e.getCause()).contains("code -17")) throw e;
                System.out.println("Generated key without FBE_ICE: ephemeral export rejected with -17");
            }
            testGenerated(args[0], args[1], synthetic);
            System.out.println("FULL_AOSP_REFERENCE_VERIFIED=" + oraclePass);
        } catch (InvocationTargetException e) {
            System.err.println("Keystore2 operation error: " + e.getCause());
            System.exit(1);
        } catch (Exception e) {
            System.err.println("Test error: " + e);
            System.exit(1);
        } finally {
            Arrays.fill(synthetic, (byte)0);
        }
    }
}
