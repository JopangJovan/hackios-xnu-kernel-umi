#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
export XNU_SRC="$ROOT/out/xiaomi-mi-10/xnu"
python3 "$DEVICE_DIR/prepare.py" kernel
python3 "$DEVICE_DIR/tools/embed-firmware.py" \
  --patch "$ROOT/fw-bluetooth/htbtfw20.tlv" --nvm "$ROOT/fw-bluetooth/htnv20.bin" \
  --output "$XNU_SRC/bsd/dev/umi_bt_firmware.inc"
python3 "$DEVICE_DIR/tools/embed-sensor-registry.py" \
  --registry "$ROOT/fw-sensors/registry.tar" \
  --output "$XNU_SRC/bsd/dev/umi_sensor_registry.inc"
python3 "$DEVICE_DIR/tools/embed-sdx55-sbl1.py" \
  --input "$ROOT/fw-sdx55/sbl1.mbn" \
  --output "$XNU_SRC/bsd/dev/umi_sdx55_sbl1.inc"
python3 "$DEVICE_DIR/tools/embed-sdx55-ddr.py" \
  --input "$ROOT/fw-sdx55/mdmddr-partition.bin" \
  --msadp "$ROOT/fw-sdx55/msadp-partition.bin" \
  --output "$XNU_SRC/bsd/dev/umi_sdx55_ddr.inc"
voice_cal_args=()
if [ -n "${UMI_VOICE_CAL_DIR:-}" ]; then
  voice_cal_args=(--input "$UMI_VOICE_CAL_DIR")
elif [ -d "$ROOT/fw-audio/voice-cal" ]; then
  voice_cal_args=(--input "$ROOT/fw-audio/voice-cal")
fi
if [ -n "${UMI_VOICE_SPEAKER_CAL_DIR:-}" ]; then
  voice_cal_args+=(--speaker-input "$UMI_VOICE_SPEAKER_CAL_DIR")
fi
python3 "$DEVICE_DIR/tools/embed-sdx55-voice-cal.py" \
  --output "$XNU_SRC/bsd/dev/umi_sdx55_voice_cal_data.inc" ${voice_cal_args[@]+"${voice_cal_args[@]}"}
mkdir -p "$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT"
[ ! -f "$ROOT/xnu-deps/build/ath11k/src/ath11k/qmi.c" ] || "$ROOT/tools/mk-wlfw.sh"
exec "$ROOT/xnu-build.sh" "${@:--j8}"
