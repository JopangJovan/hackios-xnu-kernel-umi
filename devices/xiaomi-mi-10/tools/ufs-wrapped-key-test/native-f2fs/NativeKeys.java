import java.io.BufferedReader;
import java.io.OutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.Base64;

public class NativeKeys extends DirectStorageProbe {
    private static byte[] importTestKey(byte[] seed) throws Exception {
        Object parcel = request();
        storageParams(parcel, true);
        scalar(parcel, RAW_FORMAT);
        vec(parcel, seed);
        return transaction(IMPORT_KEY, parcel, "FBE_ICE synthetic blob import (rollback tag absent)", 0);
    }

    private static byte[] exportTestKey(byte[] blob) throws Exception {
        Object parcel = request();
        scalar(parcel, RAW_FORMAT);
        vec(parcel, blob);
        vec(parcel, EMPTY);
        vec(parcel, EMPTY);
        return transaction(EXPORT_KEY, parcel, "FBE_ICE ephemeral export", 0);
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 3 || !(args[0].equals("exercise") || args[0].equals("verify")))
            throw new IllegalArgumentException("exercise or verify, runner, token required");
        binder = call(null, type("android.os.HwBinder"), "getService",
                new Class<?>[] {String.class, String.class}, DEVICE, "default");
        boolean exercise = args[0].equals("exercise");
        Base64.Encoder encoder = Base64.getEncoder();
        Base64.Decoder decoder = Base64.getDecoder();
        byte[][] blobs = new byte[2][], keys = new byte[2][];
        byte[] seed = new byte[32];
        String original = "";
        try {
            if (exercise) {
                SecureRandom random = new SecureRandom();
                for (int i = 0; i < blobs.length; ++i) {
                    random.nextBytes(seed);
                    blobs[i] = importTestKey(seed);
                }
            } else {
                BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.US_ASCII));
                String[] line = in.readLine().split(" ");
                if (line.length != 3 || !line[2].matches("[a-f0-9]{64}"))
                    throw new IllegalArgumentException("Missing test state");
                blobs[0] = decoder.decode(line[0]);
                blobs[1] = decoder.decode(line[1]);
                original = line[2];
            }
            for (int i = 0; i < blobs.length; ++i) keys[i] = exportTestKey(blobs[i]);
            Process child = new ProcessBuilder("/system/bin/sh", args[1], args[0], args[2])
                    .redirectOutput(ProcessBuilder.Redirect.INHERIT)
                    .redirectError(ProcessBuilder.Redirect.INHERIT).start();
            try (OutputStream pipe = child.getOutputStream()) {
                for (byte[] key : keys)
                    pipe.write((ReadOnlyWrappedVts.hex(key) + "\n").getBytes(StandardCharsets.US_ASCII));
                if (!exercise) pipe.write((original + "\n").getBytes(StandardCharsets.US_ASCII));
            }
            if (child.waitFor() != 0) throw new IllegalStateException("Native F2FS fixture failed");
            if (exercise) System.out.println("VOLATILE_BLOBS " + encoder.encodeToString(blobs[0]) + " " + encoder.encodeToString(blobs[1]));
        } finally {
            Arrays.fill(seed, (byte) 0);
            for (byte[] blob : blobs) if (blob != null) Arrays.fill(blob, (byte) 0);
            for (byte[] key : keys) if (key != null) Arrays.fill(key, (byte) 0);
        }
    }
}
