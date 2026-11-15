import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public class DirectStorageProbe {
    static final String DEVICE = "android.hardware.keymaster@4.0::IKeymasterDevice";
    // Transaction IDs and layouts from android.hardware.keymaster@4.0.
    static final int IMPORT_KEY = 7, EXPORT_KEY = 10;
    static final int RAW_FORMAT = 3, UNSUPPORTED_KEY_FORMAT = -17;
    static final int ALGORITHM = 0x10000002, KEY_SIZE = 0x30000003, PURPOSE = 0x20000001;
    static final int ROLLBACK_RESISTANCE = 0x7000012f;
    static final int STORAGE_KEY = 0x700002d2, FBE_ICE = 0x70003f49;
    static final int VECTOR_SIZE = 16, PARAM_SIZE = 32, PARAM_VALUE = 8, PARAM_BLOB = 16;
    static final int SAMPLES = ReadOnlyWrappedVts.SAMPLES;
    static final Class<?> HWP = type("android.os.HwParcel"), HWB = type("android.os.HwBlob");
    static final Class<?> HWI = type("android.os.IHwBinder");
    static final Class<?>[] NONE = {};
    static final byte[] EMPTY = {};
    static Object binder;

    static Class<?> type(String name) {
        try { return Class.forName(name); }
        catch (ClassNotFoundException e) { throw new IllegalStateException(e); }
    }

    static Object call(Object instance, Class<?> api, String name, Class<?>[] types, Object... values)
            throws Exception {
        return api.getMethod(name, types).invoke(instance, values);
    }

    static void put(Object blob, String which, long offset, Class<?> field, Object value) throws Exception {
        call(blob, HWB, which, new Class<?>[] {long.class, field}, offset, value);
    }

    static Object blob(int size) throws Exception {
        return HWB.getConstructor(int.class).newInstance(size);
    }

    static void scalar(Object parcel, int value) throws Exception {
        call(parcel, HWP, "writeInt32", new Class<?>[] {int.class}, value);
    }

    static void vec(Object parcel, byte[] bytes) throws Exception {
        ArrayList<Byte> data = new ArrayList<>(bytes.length);
        for (byte b : bytes) data.add(b);
        call(parcel, HWP, "writeInt8Vector", new Class<?>[] {ArrayList.class}, data);
    }

    static void storageParams(Object parcel, boolean useIce) throws Exception {
        // AES-256, with both ENCRYPT and DECRYPT purposes. No rollback-resistance request.
        int[][] values = {
            {ALGORITHM, 32}, {KEY_SIZE, 256}, {PURPOSE, 0}, {PURPOSE, 1},
            {STORAGE_KEY, 1}, {FBE_ICE, 1}
        };
        int count = useIce ? values.length : values.length - 1;
        Object header = blob(VECTOR_SIZE), records = blob(PARAM_SIZE * count);
        put(header, "putInt32", 8, int.class, count);
        put(header, "putBool", 12, boolean.class, false);
        for (int i = 0; i < count; i++) {
            long offset = i * (long) PARAM_SIZE;
            put(records, "putInt32", offset, int.class, values[i][0]);
            put(records, "putInt32", offset + PARAM_VALUE, int.class, values[i][1]);
            put(records, "putBlob", offset + PARAM_BLOB, HWB, blob(0));
        }
        put(header, "putBlob", 0, HWB, records);
        call(parcel, HWP, "writeBuffer", new Class<?>[] {HWB}, header);
    }

    static Object request() throws Exception {
        Object obj = HWP.getConstructor().newInstance();
        call(obj, HWP, "writeInterfaceToken", new Class<?>[] {String.class}, DEVICE);
        return obj;
    }

    static int number(Object blob, long offset) throws Exception {
        return (Integer) call(blob, HWB, "getInt32", new Class<?>[] {long.class}, offset);
    }

    static void verifyAuthorizations(Object reply) throws Exception {
        // KeyCharacteristics contains software- and hardware-enforced vectors, in that order.
        Object characteristics = call(reply, HWP, "readBuffer", new Class<?>[] {long.class}, 32L);
        long handle = (Long) call(characteristics, HWB, "handle", NONE);
        Class<?>[] embedded = {long.class, long.class, long.class, boolean.class};
        for (int level = 0; level < 2; level++) {
            long offset = level * (long) VECTOR_SIZE;
            int count = number(characteristics, offset + 8);
            if (count < 0 || count > 256) throw new IllegalStateException("Invalid key characteristics");
            Object records = call(reply, HWP, "readEmbeddedBuffer", embedded,
                    count * (long) PARAM_SIZE, handle, offset, true);
            long recordsHandle = (Long) call(records, HWB, "handle", NONE);
            for (int i = 0; i < count; i++) {
                long field = i * (long) PARAM_SIZE;
                if (number(records, field) == ROLLBACK_RESISTANCE)
                    throw new IllegalStateException("Unexpected rollback-resistance authorization");
                // Consume each nested byte vector in parcel order before reading the next list.
                long payload = field + PARAM_BLOB;
                int size = number(records, payload + 8);
                call(reply, HWP, "readEmbeddedBuffer", embedded, (long) size, recordsHandle, payload, true);
            }
        }
    }

    static byte[] transaction(int id, Object request, String label, int expectedCode)
            throws Exception {
        Object reply = HWP.getConstructor().newInstance();
        try {
            call(binder, HWI, "transact", new Class<?>[] {int.class, HWP, HWP, int.class},
                    id, request, reply, 0);
            call(reply, HWP, "verifySuccess", NONE);
            int code = (Integer) call(reply, HWP, "readInt32", NONE);
            System.out.println(label + " status=" + code);
            if (code != expectedCode) throw new IllegalStateException(label + " returned " + code);
            if (code != 0) return null;
            List<?> content = (List<?>) call(reply, HWP, "readInt8Vector", NONE);
            byte[] data = new byte[content.size()];
            for (int i = 0; i < data.length; i++) data[i] = (Byte) content.get(i);
            if (id == IMPORT_KEY) verifyAuthorizations(reply);
            return data;
        } finally {
            call(request, HWP, "releaseTemporaryStorage", NONE);
            call(request, HWP, "release", NONE);
            call(reply, HWP, "release", NONE);
        }
    }

    static byte[] importAndExport(byte[] seed, boolean useIce) throws Exception {
        Object importRequest = request();
        storageParams(importRequest, useIce);
        scalar(importRequest, RAW_FORMAT);
        vec(importRequest, seed);
        String label = "HIDL RAW import with FBE_ICE=" + useIce;
        byte[] blob = transaction(IMPORT_KEY, importRequest, label, 0);
        if (blob.length == 0) throw new IllegalStateException("Empty imported key blob");
        try {
            Object exportRequest = request();
            scalar(exportRequest, RAW_FORMAT);
            vec(exportRequest, blob);
            vec(exportRequest, EMPTY);
            vec(exportRequest, EMPTY);
            return transaction(EXPORT_KEY, exportRequest, label + " -> RAW export",
                    useIce ? 0 : UNSUPPORTED_KEY_FORMAT);
        } finally {
            Arrays.fill(blob, (byte) 0);
        }
    }

    static int matchingSectors(ReadOnlyWrappedVts.Sample sample, byte[] reference) throws Exception {
        return ReadOnlyWrappedVts.sectorsMatching(sample.raw, sample.data, reference);
    }

    static void checkReferences(String helper, String token, byte[] seed, byte[] otherSeed,
                                byte[] first, byte[] repeat, byte[] other) throws Exception {
        byte[] reference = ReadOnlyWrappedVts.android13Kdf(seed);
        byte[] otherReference = ReadOnlyWrappedVts.android13Kdf(otherSeed);
        try {
            try (ReadOnlyWrappedVts.Sample software =
                    ReadOnlyWrappedVts.readSample(helper, token, "software", reference)) {
                int matched = matchingSectors(software, reference);
                System.out.println("Software dm-crypt AES-XTS: " + matched + "/" + SAMPLES + " sectors agree");
                if (matched != SAMPLES) throw new IllegalStateException("Software AES-XTS control failed");
            }
            try (ReadOnlyWrappedVts.Sample raw =
                    ReadOnlyWrappedVts.readSample(helper, token, "inline-raw", reference)) {
                System.out.println("Raw dm-default-key AES-XTS: " + matchingSectors(raw, reference)
                    + "/" + SAMPLES + " sectors agree");
            }
            try (ReadOnlyWrappedVts.Sample a = ReadOnlyWrappedVts.readSample(helper, token, first);
                 ReadOnlyWrappedVts.Sample b = ReadOnlyWrappedVts.readSample(helper, token, repeat);
                 ReadOnlyWrappedVts.Sample c = ReadOnlyWrappedVts.readSample(helper, token, other)) {
                if (!Arrays.equals(a.raw, b.raw) || !Arrays.equals(a.raw, c.raw))
                    throw new IllegalStateException("Raw UFS sample changed between wrapped mappings");
                int same = SAMPLES - ReadOnlyWrappedVts.differingSectors(a.data, b.data);
                int different = ReadOnlyWrappedVts.differingSectors(a.data, c.data);
                System.out.println("Same seed, separate imports: " + same + "/" + SAMPLES + " sectors agree");
                System.out.println("Different seed: " + different + "/" + SAMPLES + " sectors differ");
                if (same != SAMPLES || different != SAMPLES) throw new IllegalStateException("Seed control failed");
                int known = matchingSectors(a, reference), second = matchingSectors(c, otherReference);
                System.out.println("First imported seed AOSP AES-XTS: " + known + "/" + SAMPLES + " sectors agree");
                System.out.println("Second imported seed AOSP AES-XTS: " + second + "/" + SAMPLES + " sectors agree");
                System.out.println("FULL_AOSP_REFERENCE_VERIFIED=" + (known == SAMPLES && second == SAMPLES));
            }
        } finally {
            Arrays.fill(reference, (byte) 0);
            Arrays.fill(otherReference, (byte) 0);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("helper path and test token required");
        binder = call(null, type("android.os.HwBinder"), "getService",
                new Class<?>[] {String.class, String.class}, DEVICE, "default");
        byte[] seed = new byte[32], otherSeed = new byte[32];
        SecureRandom random = new SecureRandom();
        random.nextBytes(seed);
        random.nextBytes(otherSeed);
        byte[][] wrapped = new byte[3][];
        try {
            importAndExport(seed, false);
            wrapped[0] = importAndExport(seed, true);
            wrapped[1] = importAndExport(seed, true);
            wrapped[2] = importAndExport(otherSeed, true);
            checkReferences(args[0], args[1], seed, otherSeed, wrapped[0], wrapped[1], wrapped[2]);
        } finally {
            Arrays.fill(seed, (byte) 0);
            Arrays.fill(otherSeed, (byte) 0);
            for (byte[] key : wrapped) {
                if (key != null) Arrays.fill(key, (byte) 0);
            }
        }
    }
}
