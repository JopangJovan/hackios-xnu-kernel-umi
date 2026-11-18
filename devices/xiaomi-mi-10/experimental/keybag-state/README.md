# Independent-bag state ABI prerequisite

This experiment follows `keybag` and `crypt-volume`. It adds the iOS 16.5
public device-state selectors `0x11` and `0x23`, and bag UUID selector `0x17`,
only to the existing explicitly opted-in independent AKS client. It does
not select/install a system bag, enable Settings' passcode flow, issue
system lock notifications, or modify ordinary USB/keychain clients.

The state fields use the schema already present in MiniAppleKeyStore,
with `ss` and `sls` derived from the bag's actual authentication state:

| State | Flags `ss` | Lock state `sls` |
|---|---:|---:|
| Loaded, never authenticated in this session | 1 | 1 |
| Authenticated/unlocked | 4 | 0 |
| Locked again after first unlock | 5 | 1 |

Wrong passwords do not change these states. Relocking clears the unwrapped
master via the previously tested bag-lock path. The no-passcode flag is
never set for these password-protected bags. Other schema fields remain
zero; no attempt counter, retry delay, auto-erase or hardware enforcement
is claimed. A bag UUID is public metadata; authentication of a loaded
envelope still requires successful unlock.

The test reads an existing public synthetic keybag and checks native
IOConnect calls before/after unlock and relock, wrong-password rejection,
short-output refusal, stable UUID, refusal of handle 0, and unchanged
normal system flags. It does not ask for a real password or write a bag.

```sh
python3 stage.py --xnu /path/to/prepared/xnu --check
python3 stage.py --xnu /path/to/prepared/xnu
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.5 \
  -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations \
  state-test.c -framework IOKit -o aks-state-test
```

Run `aks-state-test EXISTING_SYNTHETIC_AKS_DIRECTORY` in a temporary helper
with `umi_aks_lab=1`. The directory must be a fixture from `aks-lab-test.c`,
not a real system keybag. Record device results in
[LOCKSCREEN-PASSCODE.md](../../LOCKSCREEN-PASSCODE.md).

The native-lock-screen route still needs shared system-bag ownership,
first password installation, durable keybag updates, real lock-state
notifications, filesystem protection classes/cache invalidation, and
restart-to-lock-screen handling. This state adapter alone is not that route.

Device session `native-28` passed the state/UUID/short-output checks, previous
keybag regression probes and paired USB/syslog checks. The framebuffer
showed the iOS desktop. Full observations and remaining integration steps
are in the linked progress document; this is not a successful Settings
passcode setup or encryption of the actual user data volume.
