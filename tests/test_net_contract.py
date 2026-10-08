#!/usr/bin/env python3
"""Static contracts for the联网服务的两个已修缺陷。

两处缺陷都属于"代码看起来在工作、实际把关键事实吞掉"的类型，普通主机测试很难覆盖
（它们在 ESP-IDF 强依赖的 app_net.c / app_state.c 里），因此用源码契约把它们钉住：

1. Wi-Fi 实验：`esp_wifi_80211_tx()` 的返回值必须被统计，帧数只能是"驱动接受的帧"，
   不能再出现无条件累加导致的假计数。
2. HTTPS：校准入口必须同时写 newlib 系统时钟（mbedTLS 用它校验证书有效期），并且在
   发起请求前兜底保证时钟可用。
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


class NetContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app_net = read("main/net/app_net.c")
        cls.app_state = read("main/app_state.c")
        cls.ui_wifilab = read("main/ui/ui_wifilab.c")

    def test_every_raw_tx_result_is_counted(self) -> None:
        # 每一处发射都必须把返回值交给 wifilab_count_tx()；裸调用会让"已发帧数"重新
        # 退化成循环次数，用户会以为帧已经上了空口。
        calls = re.findall(r"(?m)^\s*(\S.*?esp_wifi_80211_tx\(.*)$", self.app_net)
        self.assertTrue(calls, "no esp_wifi_80211_tx() call sites found")
        for call in calls:
            self.assertTrue(
                call.startswith("wifilab_count_tx(esp_wifi_80211_tx("),
                f"esp_wifi_80211_tx() result is not counted: {call.strip()}",
            )

    def test_frame_counter_only_counts_driver_accepted_frames(self) -> None:
        counter = function_body(self.app_net, "wifilab_count_tx")
        self.assertIn("err == ESP_OK", counter)
        self.assertIn("s_wifilab_sent++", counter)
        self.assertIn("s_wifilab_failed++", counter)

        # 攻击循环里不允许再出现无条件自增（旧写法：先发再 s_wifilab_sent += 2）。
        loop = function_body(self.app_net, "wifilab_attack_task")
        self.assertNotIn("s_wifilab_sent", loop)
        self.assertNotIn("s_wifilab_failed", loop)

    def test_failed_frames_are_visible_to_the_ui(self) -> None:
        self.assertIn("app_net_wifilab_packets_failed", self.app_net)
        self.assertIn("app_net_wifilab_tx_error_text", self.app_net)
        self.assertIn("app_net_wifilab_packets_failed()", self.ui_wifilab)
        self.assertIn("app_net_wifilab_tx_error_text()", self.ui_wifilab)

    def test_time_calibration_also_sets_the_system_clock(self) -> None:
        # epoch_base 只服务界面；HTTPS 的证书校验读 time(NULL)。
        body = function_body(self.app_state, "app_state_set_time")
        self.assertIn("settimeofday", body)
        self.assertIn("unix_utc", body)

    def test_https_requests_ensure_the_system_clock_first(self) -> None:
        body = function_body(self.app_net, "http_get_json")
        self.assertIn("ensure_system_clock()", body)
        self.assertLess(body.index("ensure_system_clock()"),
                        body.index("esp_http_client_init"))
        # 打开失败（DNS/TCP/TLS）时要把原因交给调用方，而不是让上层写一句笼统的"请求失败"。
        self.assertIn("http_set_error(", body)
        self.assertRegex(body, r"net_clock_is_set\(\)\s*\?\s*\"网络连接失败\"\s*:\s*"
                               r"\"系统时间未校准\"")


if __name__ == "__main__":
    unittest.main()