# Firmware charger polling during native USB ownership

This isolated loader experiment follows the USB loss in `native-62` during
the class-A/C encrypted-file restart test. It does not change the passcode,
keybag, native USB driver, pairing records, Android, GPT or boot partitions.
It is not yet integrated into the normal cellular build.

## Observation and change

The resident UEFI loop calls `charger_poll()` every five seconds, even after
`umi_usb_park()` stops the firmware USB protocol and XNU takes DWC3 ownership.
That polling includes Qualcomm `GetChargingAction`, in addition to the basic
ChargerEx presence and voltage queries. Not calling `TakeAction` does not
establish that the firmware's status implementation has no controller effects.

The patch records external USB ownership only after firmware `Stop` succeeds.
While that ownership is active, it omits `GetChargingAction`; ChargerEx presence
and voltage reads continue. Successful firmware USB restart clears the flag.
The kernel and its USB implementation are unchanged. This does not adjust
charging limits, disable PMIC thermal protection or implement a new charger
driver. The exact internal firmware operation causing the transport loss has
not been traced.

Without that policy query, the shared cache uses its existing voltage-based
SOC estimate and marks it as estimated. Current is unavailable (`0`), battery
temperature is unknown (`-273`), and action is unknown (`0xffffffff`), instead
of continually publishing stale pre-handoff values as new gauge readings.
In this isolated no-ADSP helper, this is a battery-telemetry limitation. A normal
build's native battery manager and combined cellular/USB behavior need their
own verification. The earlier all-polling-off diagnostic freezes telemetry and
is not the retained candidate.

## Controlled runs

All runs used temporary `fastboot boot`, the existing UFS user volume, the
user's existing passcode and pairing, and no session countdown. Completed runs
returned through the loader to stock Android using a normal shutdown/restart. No private
logs, credentials, firmware images or generated binaries are included here.

| Case | Kernel/helper | Firmware polling during native ownership | Result |
|---|---|---|---|
| native-62 | v13 A/C / verify | All queries enabled | Native USB disappeared shortly after enumeration; iOS remained running |
| native-63 | Exact native-60 v12 / verify | All queries enabled | Same failure reproduced without the class-C extension; normal shutdown recovered final logs and stock Android |
| native-64 | Same v12 / verify | All charger queries temporarily suspended | Pairing and metadata succeeded, locked syslog correctly refused, unlocked 45-second capture collected 671,744 bytes / 3,770 lines; normal diagnostics restart and Android return succeeded |
| native-65 | Same v12 / verify | Only GetChargingAction omitted | 13 authenticated metadata checks across 120.83 seconds all passed; locked syslog and diagnostics restart correctly returned Password protected; unlocked 30-second capture returned 434,176 bytes / 2,336 lines, then normal restart succeeded |
| native-66 | v13 A/C / verify | Same narrow fence | Existing pairing passed; 180-second syslog captured 4,341,760 bytes / 29,185 lines across lock/unlock; physical unplug/replug re-enumerated, reused pairing and captured another 368,640 bytes / 1,984 lines over 30 seconds; normal diagnostics restart accepted |

The native-64 interval from the last pre-handoff capture to the successful
restart request was 360 seconds. This is a short controlled run, not a long-term
stability claim. The older kernel also reproducing the fault means the class-C
change is not required to trigger it; it does not independently prove every
encryption path free of USB regressions.

The native-65 final firmware log also recorded the old class-A fixture's
`verify PASS` and raw inner-ciphertext check. Battery voltage samples continued
changing (4367, 4366, 4302 mV), with the estimate/unknown-field markers above
and no failed battery reads in those samples. This establishes that the narrow
candidate does not simply freeze all battery polling to keep USB alive.

The v13 integration recovered the same three A/C files and passed the inner
ciphertext scan before native USB handoff. The user completed the requested
relock check; lock and unlock VFS callbacks returned zero while the existing
log stream continued. First-unlock refusal was not repeated in this run because
the phone was already unlocked before native USB handoff. The previously
verified password/cryptographic implementation is byte-for-byte unchanged.
There was no second Trust prompt or replacement pairing record in the host
flow. The physical replug is visible as USB device count `1 → 0 → 1`.

These results qualify the temporary no-modem v13 image for continued isolated
encryption development. They do not establish hours of suspend/resume stability
or normal cellular-build integration. Full iOS user-volume encryption is still
disabled; unsupported protection/bootstrap paths must not be bypassed.
Both native-65 and native-66 completed final-log collection and returned to
stock Android (`umi`, `V14.0.4.0.TJBCNXM`, `sys.boot_completed=1`). Sanitized
measurements and kernel hashes are in [results.json](results.json).

## Reproduce the source change

`source-lock.json` pins the prepared native-63 loader and the narrow candidate.
This base includes the preceding UFS/USB/passcode experiment and is **not** the
normal repository loader or the earlier native-12 checkpoint. Use disposable
copies; unknown source hashes are refused without modification.

```sh
python3 stage.py --loader /path/to/prepared-loader-copy --check
python3 stage.py --loader /path/to/prepared-loader-copy
```

The patch reconstruction, repeated application and unknown-source refusal
were checked locally. Both temporary ARM64 EFI candidates compiled.
