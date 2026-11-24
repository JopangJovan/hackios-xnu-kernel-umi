# Isolated keybag/data crypto test

This program is a prerequisite experiment for the
[passcode/data-protection work](../../LOCKSCREEN-PASSCODE.md). It must not
be used as a production keystore: its passphrase is deliberately public,
its format is local to this test, and it has no hardware-key binding,
rollback protection, crash-atomic multi-file update, or protection-class
integration. It does not modify the iOS system keybag or enable a password.

On macOS:

```sh
clang -O2 -Wall -Wextra -Werror crypto-selftest.c -o /tmp/umi-crypto-selftest
/tmp/umi-crypto-selftest create /path/to/new-test-directory
/tmp/umi-crypto-selftest verify /path/to/new-test-directory
```

For the isolated iOS helper, compile the same source with
`-target arm64-apple-ios16.5 -isysroot /path/to/iPhoneOS16.5.sdk`. Run
`create` once in a new directory below `/private/var/tmp`, return to Android
normally, and run `verify` against the same directory in a second temporary
boot. `verify` opens its two files read-only. Repeated `create` refuses an
existing directory. No personal password should be supplied: the program
has no password argument and uses its own fixed synthetic fixture.

Outputs contain test statuses and the generated plaintext's checksum,
never key bytes. Keep generated artifacts outside Git. Passing this test
does not make `/private/var` encrypted or make lock-screen passwords work.

To exercise single-file keybag rotation and process interruption:

```sh
/tmp/umi-crypto-selftest exercise /path/to/new-rotation-directory
/tmp/umi-crypto-selftest verify-exercise /path/to/new-rotation-directory
```

`exercise` creates four subdirectories, then runs child processes to test
normal replacement, an incomplete pending file, termination before rename,
and termination after rename. It checks that the active old bag remains
usable before commit, discards pending test files only after authenticating
the active bag, finishes the update, and checks that the new test passphrase
opens the bag while the old one fails. The encrypted data file must remain
byte-for-byte unchanged. `verify-exercise` is read-only and can be used in
a second iOS boot. All passphrases remain public synthetic fixtures.

The test is single-writer and does not establish physical power-loss safety,
old-keybag anti-rollback, or secure deletion. A failed sync after rename has
an uncertain durability outcome. It is not a transactional system keybag
service. Direct `rotate-*` modes are failure-injection helpers and may exit
with code 86 deliberately.

`loader-rng-test.c` tests the actual UMI loader's EFI RNG helper against a
mock provider, including missing protocols, errors, stuck output, and
zeroing the destination on failure:

```sh
clang -std=c11 -O2 -Wall -Wextra -Werror loader-rng-test.c -o /tmp/umi-rng-test
/tmp/umi-rng-test
```

These fault cases do not measure the real firmware provider's entropy.

`aks-state.c` is a separate read-only probe for AppleKeyStore selector 7.
It queries public lock-state flags; it never submits a passphrase or reads
a keybag. For iOS, compile with the same SDK/target flags and `-framework IOKit`.
