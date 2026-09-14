#!/bin/sh
# r0lab arm→close 生命周期压测脚本（针对当前命令体系，适配 P5/P6）。
#
# 目的：验证 macOS/编译机编译的 r0lab-m1 KPM 在真机上重复 arm/close 时
#       无 panic、无 use-after-free、session 正确回收、worker 稳定。
#
# 用法：
#   ANDROID_SERIAL=<serial> STRESS_LOOPS=200 sh scripts/test_arm_close_stress_device.sh
#   # 不设 ANDROID_SERIAL 时默认 P6 (oriole)；STRESS_LOOPS 默认 100。
#
# 前置：
#   - 设备已加载 r0lab-m1 模块（lab_uid 正确）
#   - lab-app (dev.r0hook.lab) 已安装
#   - adb 在 PATH（或 export ADB=/path/to/adb）
#
# 退出码：0 = 全部通过；非 0 = 某轮失败（含内核异常）。

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ADB=${ADB:-adb}
SERIAL=${ANDROID_SERIAL:-18201FDF6002GR}   # 默认 P6 (oriole)
LOOPS=${STRESS_LOOPS:-100}
ACTIVITY=dev.r0hook.lab/.MainActivity
TOKEN=0x6a1a10
LAB_UID=${LAB_UID:-10285}                  # 默认 P6 的 uid

adb_device() { "$ADB" -s "$SERIAL" "$@"; }

run_cmd() {
  # 触发一条 lab 命令，返回 logcat 中 R0Lab 的 I 级别输出（单行拼接）。
  adb_device logcat -c >/dev/null 2>&1 || true
  adb_device shell "am start -W -n $ACTIVITY --es r0lab_command '$1'" >/dev/null 2>&1 || true
  sleep 0.4
  adb_device logcat -d -v brief -s R0Lab:I '*:S' 2>/dev/null | tr -d '\r'
}

warn_count() { adb_device shell "su -c 'cat /sys/kernel/warn_count'" 2>/dev/null | tr -d '\r\n'; }
boot_id()   { adb_device shell "cat /proc/sys/kernel/random/boot_id" 2>/dev/null | tr -d '\r\n'; }

fail() { printf 'STRESS FAIL [iter=%s]: %s\n' "$ITER" "$*" >&2; exit 1; }

# 防御性清理：若上次被中断残留 active session，先 close 掉。
# 返回 0 表示已无 active session。
clear_residual_session() {
  local st i
  for i in 1 2 3; do
    st=$(run_cmd "status" 2>/dev/null || true)
    case "$st" in
      *"active=0"*) return 0 ;;
      *"active=1"*) run_cmd "close $TOKEN" >/dev/null 2>&1 || true ;;
      *) return 0 ;;
    esac
    sleep 1
  done
  return 1
}

# 中断/退出时清理，避免留下 active session 影响下次运行。
cleanup_on_exit() {
  run_cmd "close $TOKEN" >/dev/null 2>&1 || true
}
trap cleanup_on_exit EXIT INT TERM

# ---- 前置校验 ----
BOOT_BEFORE=$(boot_id)
WARN_BEFORE=$(warn_count)
[ -n "$BOOT_BEFORE" ] || fail "cannot read boot_id (device offline?)"
[ "$WARN_BEFORE" -ge 0 ] 2>/dev/null || fail "cannot read warn_count (need root?)"

# 确认模块已加载
MODLIST=$(adb_device shell "su -c 'kpctl --key amigo123 list'" 2>/dev/null || true)
case "$MODLIST" in
  *r0lab-m1*) ;;
  *) fail "r0lab-m1 模块未加载：$MODLIST" ;;
esac

# 防御性清理残留 active session（上次被中断遗留）
if ! clear_residual_session; then
  fail "存在残留 active session 且无法清理，请手动 close 后重试"
fi

printf '=== r0lab arm/close 压测 ===\n'
printf 'serial=%s loops=%s token=%s lab_uid=%s\n' "$SERIAL" "$LOOPS" "$TOKEN" "$LAB_UID"
printf 'boot_before=%s warn_before=%s\n' "$BOOT_BEFORE" "$WARN_BEFORE"

PASS=0
FAIL_CNT=0
ITER=0
while [ "$ITER" -lt "$LOOPS" ]; do
  ITER=$((ITER + 1))

  # arm
  ARM_OUT=$(run_cmd "arm $TOKEN") || fail "arm 命令执行失败"
  case "$ARM_OUT" in
    *"armed uid=$LAB_UID"*) ;;
    *) fail "arm 输出异常: $ARM_OUT" ;;
  esac

  # status 应 active=1
  ST1=$(run_cmd "status") || fail "status(armed) 执行失败"
  case "$ST1" in
    *"active=1"*) ;;
    *) fail "arm 后 active!=1: $ST1" ;;
  esac

  # close
  CLOSE_OUT=$(run_cmd "close $TOKEN") || fail "close 命令执行失败"
  case "$CLOSE_OUT" in
    *"closed"*) ;;
    *) fail "close 输出异常: $CLOSE_OUT" ;;
  esac

  # status 应回到 active=0
  ST2=$(run_cmd "status") || fail "status(closed) 执行失败"
  case "$ST2" in
    *"active=0"*"owner_tgid=0"*) ;;
    *) fail "close 后未回收: $ST2" ;;
  esac

  # 每轮做一次内核健康快照（boot_id 不变 + warn_count 不增）
  B=$(boot_id)
  W=$(warn_count)
  [ "$B" = "$BOOT_BEFORE" ] || fail "设备重启（boot_id 变化）"
  [ "$W" -le "$WARN_BEFORE" ] || fail "warn_count 增加 ($WARN_BEFORE -> $W)：内核产生 WARN/BUG"

  PASS=$((PASS + 1))
  if [ $((ITER % 10)) -eq 0 ]; then
    printf '  iter=%d/%d ok (warn_count=%s)\n' "$ITER" "$LOOPS" "$W"
  fi
done

# 收尾：最终内核健康确认
BOOT_AFTER=$(boot_id)
WARN_AFTER=$(warn_count)
[ "$BOOT_AFTER" = "$BOOT_BEFORE" ] || fail "结束时设备重启"
[ "$WARN_AFTER" -le "$WARN_BEFORE" ] || fail "warn_count 结束增加"

printf '=== PASS ===\n'
printf 'loops=%d passed=%d failed=%d warn_before=%s warn_after=%s boot_unchanged=yes\n' \
  "$LOOPS" "$PASS" "$FAIL_CNT" "$WARN_BEFORE" "$WARN_AFTER"
exit 0
