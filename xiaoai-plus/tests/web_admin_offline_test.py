"""Loopback and fake-device checks; no SSH, no actual API Key or cloud calls."""

import http.client
import importlib.util
from pathlib import Path
import re
import subprocess
import threading
import unittest
from unittest.mock import patch
from urllib.parse import urlencode


SOURCE = Path(__file__).resolve().parents[1] / "toolchain/web-admin.py"
spec = importlib.util.spec_from_file_location("oh2p_web_admin", SOURCE)
admin = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(admin)


class WebAdminOfflineTest(unittest.TestCase):
    def setUp(self):
        self.device = admin.MockDevice()
        self.handler = admin.create_handler(self.device, True)
        self.server = admin.HTTPServer(("127.0.0.1", 0), self.handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_port
        self.host = f"127.0.0.1:{self.port}"
        self.origin = f"http://{self.host}"
        self.cookie = ""

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.device.close()

    def request(self, method, path="/", data=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        supplied = {"Host": self.host}
        if headers:
            supplied.update(headers)
        if data is not None:
            supplied.setdefault("Origin", self.origin)
            supplied.setdefault("Content-Type", "application/x-www-form-urlencoded")
        conn.request(method, path, data, supplied)
        response = conn.getresponse()
        body = response.read().decode("utf-8")
        result = response.status, body, dict(response.getheaders())
        conn.close()
        return result

    def login(self):
        payload = urlencode({"code": self.handler.access_code})
        status, _, headers = self.request("POST", "/login", payload)
        self.assertEqual(status, 303)
        self.cookie = headers["Set-Cookie"].split(";", 1)[0]

    def csrf(self):
        status, page, _ = self.request("GET", headers={"Cookie": self.cookie})
        self.assertEqual(status, 200)
        self.assertIn("/save", page)
        return re.search(r'name=csrf value="([^"]+)"', page).group(1)

    def test_full_save_and_keyword_change_without_key_echo(self):
        status, page, headers = self.request("GET")
        self.assertEqual(status, 200)
        self.assertIn("no-store", headers["Cache-Control"])
        self.assertEqual(headers["Referrer-Policy"], "same-origin")
        self.assertNotIn("/save", page)
        self.login()
        csrf = self.csrf()
        payload = urlencode({"csrf": csrf, "key": "fake-web-only", "word": "canonical", "pref": "on"})
        status, body, _ = self.request("POST", "/save", payload, {"Cookie": self.cookie})
        self.assertEqual(status, 303)
        self.assertNotIn("fake-web-only", body)
        status, page, _ = self.request("GET", headers={"Cookie": self.cookie})
        self.assertEqual(status, 200)
        self.assertIn("API Key：已配置", page)
        self.assertIn("豆包（两字候选）", page)
        self.assertIn("希望以后启用", page)
        self.assertIn("当前 boot.sh：不可执行", page)
        self.assertNotIn("fake-web-only", page)
        self.assertEqual((self.device.root / "config.ini").stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.device.root / "boot.sh").stat().st_mode & 0o111)
        first = (self.device.root / "config.ini").read_text()
        self.assertIn("api_key = fake-web-only", first)
        payload = urlencode({"csrf": csrf, "key": "", "word": "baseline", "pref": "off"})
        status, _, _ = self.request("POST", "/save", payload, {"Cookie": self.cookie})
        self.assertEqual(status, 303)
        later = (self.device.root / "config.ini").read_text()
        self.assertIn("api_key = fake-web-only", later)
        self.assertIn("keywords_baseline_doubao_tongxue.txt", later)
        self.assertEqual(list(self.device.root.glob(".config.ini.web-admin.v1.*")), [])

    def test_host_origin_csrf_and_control_character_rejection(self):
        status, _, _ = self.request("GET", headers={"Host": f"localhost:{self.port}"})
        self.assertEqual(status, 404)
        self.login()
        csrf = self.csrf()
        payload = urlencode({"csrf": csrf, "key": "fake-safe", "word": "baseline", "pref": "off"})
        status, _, _ = self.request("POST", "/save", "",
                                    {"Cookie": self.cookie, "Origin": "http://evil.example"})
        self.assertEqual(status, 403)
        status, _, _ = self.request("POST", "/save", "",
                                    {"Cookie": self.cookie, "Origin": "null"})
        self.assertEqual(status, 403)
        payload = urlencode({"csrf": "wrong", "key": "fake-safe", "word": "baseline", "pref": "off"})
        status, _, _ = self.request("POST", "/save", payload, {"Cookie": self.cookie})
        self.assertEqual(status, 403)
        for bad in ("fake-a\napi_key = stolen", "fake-a\rnext", "fake-a\x00b", "fake-#x", "fake-;x"):
            payload = urlencode({"csrf": csrf, "key": bad, "word": "baseline", "pref": "off"})
            status, _, _ = self.request("POST", "/save", payload, {"Cookie": self.cookie})
            self.assertEqual(status, 400)
        self.assertFalse((self.device.root / "config.ini").exists())

    def test_remote_guard_and_failed_save_leave_old_config(self):
        # Exercise the closed state without ever letting this test contact OH2P.
        with patch.object(admin, "REAL_DEVICE_WRITE_APPROVED", False):
            with self.assertRaises(admin.AdminError):
                admin.SSHDevice.__new__(admin.SSHDevice).run("save", "baseline\noff\nfake-only\n")
        self.device.run("save", "baseline\noff\nfake-first\n")
        before = (self.device.root / "config.ini").read_bytes()
        with self.assertRaises(admin.AdminError):
            self.device.run("save", "canonical\noff\nfake-#bad\n")
        self.assertEqual((self.device.root / "config.ini").read_bytes(), before)
        self.assertEqual(list(self.device.root.glob(".config.ini.web-admin.v1.*")), [])

    def test_stale_secret_cleanup_is_scoped(self):
        stale = self.device.root / ".config.ini.web-admin.v1.stale01"
        stale.write_text("# oh2p-web-admin-pending-v1\napi_key = fake-old\n")
        stale.chmod(0o600)
        self.assertEqual(self.device.run("cleanup"), "PENDING_CLEAN=1\n")
        self.assertFalse(stale.exists())
        foreign = self.device.root / ".config.ini.web-admin.v1.foreign"
        foreign.write_text("unrecognized data\n")
        foreign.chmod(0o600)
        with self.assertRaises(admin.AdminError):
            self.device.run("cleanup")
        self.assertTrue(foreign.exists())

    def test_exact_current_and_rollback_binary_hashes(self):
        source = admin.REMOTE_SCRIPT.read_text(encoding="utf-8")
        self.assertEqual(admin.MANIFEST["ALLOW_WEB_ADMIN_WRITE"], "0")
        for name in ("ELF_SHA256", "INSTALLED_ELF_SHA256", "LEGACY_PCM_ELF_SHA256"):
            self.assertIn(f'${name}', source)
            value = admin.MANIFEST[name]
            self.assertTrue(not value or re.fullmatch(r"[0-9a-f]{64}", value))
            self.assertNotIn(value, source) if value else None

    def test_fake_ssh_keeps_key_out_of_command(self):
        fake_key = "fake-ssh-stdin-only"
        payload = f"baseline\noff\n{fake_key}\n"
        response = subprocess.CompletedProcess([], 0, "SAVED_KEY_SET=1\nBOOT_UNCHANGED=1\n", "")
        with patch.object(admin, "REAL_DEVICE_WRITE_APPROVED", True):
            with patch.object(admin.subprocess, "run", return_value=response) as fake_run:
                output = admin.SSHDevice.__new__(admin.SSHDevice).run("save", payload)
        self.assertEqual(output, response.stdout)
        args, kwargs = fake_run.call_args
        self.assertEqual(args[0][0], "ssh")
        self.assertEqual(kwargs["input"], payload)
        self.assertNotIn(fake_key, " ".join(args[0]))
        self.assertNotIn("-o StrictHostKeyChecking=no", " ".join(args[0]))


if __name__ == "__main__":
    unittest.main()
