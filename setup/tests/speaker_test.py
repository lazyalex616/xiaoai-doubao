"""Offline checks of the startup log wait; no SSH or device access."""
from pathlib import Path
import sys
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import speaker

class ReadyTest(unittest.TestCase):
    def test_waits_for_both_new_messages(self):
        clock=SimpleNamespace(monotonic=Mock(side_effect=[0,0,1]),sleep=Mock())
        with patch.object(speaker,'time',clock), patch.object(speaker,'ssh',side_effect=[
            'app components started',
            'app components started\nkws keywords: 1 wake, 6 exit',
        ]) as ssh:
            speaker.wait_ready(None,123)
            self.assertEqual(ssh.call_count,2)
            self.assertIn('tail -c +124',ssh.call_args.args[1])
            clock.sleep.assert_called_once_with(1)

    def test_ready_after_log_truncation(self):
        with tempfile.TemporaryDirectory() as tmp:
            log=Path(tmp)/'xiaoai_plus.log'
            log.write_text('app components started\nkws keywords: 1 wake, 6 exit\n')
            def local_ssh(a, command):
                return subprocess.check_output(['sh','-c',command],text=True)
            with patch.object(speaker,'APP',tmp), patch.object(speaker,'ssh',side_effect=local_ssh):
                speaker.wait_ready(None,4*1024*1024+1)

    def test_missing_either_message_stops_at_deadline(self):
        for text in ('app components started','kws keywords: 1 wake, 6 exit',''):
            with self.subTest(text=text):
                clock=SimpleNamespace(monotonic=Mock(side_effect=[0,0,35]),sleep=Mock())
                with patch.object(speaker,'time',clock), patch.object(speaker,'ssh',return_value=text):
                    with self.assertRaisesRegex(RuntimeError,'启动35秒'):
                        speaker.wait_ready(None,123)

if __name__=='__main__':unittest.main()
