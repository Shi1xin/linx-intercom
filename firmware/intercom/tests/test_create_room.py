"""Source guards for the voice create-room path.

self.intercom.create (agent voice entry) and the ctrl op "create" (cloud
/admin/op) must both funnel into a join with target {"kind":"new"}, and the
success result must carry invite_spoken so the agent reads the six-digit code
digit by digit.
"""

import re
import unittest
from pathlib import Path

MAIN_DIR = Path(__file__).resolve().parent.parent / "main"
APP_MAIN = (MAIN_DIR / "app_main.c").read_text()
CTRL = (MAIN_DIR / "ctrl_client.c").read_text()
LINX = (MAIN_DIR / "linx_client.c").read_text()


class TestCreateToolDeclared(unittest.TestCase):
    def test_linx_tools_contains_create(self):
        self.assertIn('self.intercom.create', LINX)
        m = re.search(r'self\.intercom\.create\\?", LINX', LINX) or \
            re.search(r'self\.intercom\.create', LINX)
        self.assertIsNotNone(m)
        # description tells the agent to read invite_spoken digit by digit
        self.assertIn('invite_spoken', LINX)

    def test_ctrl_op_maps_create(self):
        self.assertRegex(
            CTRL,
            r'strcmp\(op->valuestring, "create"\) == 0\)\s*{\s*tool = "self\.intercom\.create"')

    def test_ctrl_send_join_new_has_no_arg(self):
        m = re.search(
            r'ctrl_send_join\(.*?\{(?P<body>.*?)return ctrl_send_json',
            CTRL, re.S)
        self.assertIsNotNone(m)
        self.assertIn('strcmp(kind, "new") == 0', m.group("body"))


class TestCreateDispatch(unittest.TestCase):
    def test_dispatch_starts_new_join(self):
        m = re.search(
            r'strcmp\(tool, "self\.intercom\.create"\) == 0\)\s*\{(?P<body>.*?)\}\s*else if',
            APP_MAIN, re.S)
        self.assertIsNotNone(m)
        self.assertIn('start_join("new"', m.group("body"))
        self.assertIn('s_mcp_op.active = true', m.group("body"))

    def test_start_join_accepts_argless_new(self):
        m = re.search(
            r'start_join_ex\(.*?\{(?P<body>.*?)ctrl_send_join\(kind, arg\);',
            APP_MAIN, re.S)
        self.assertIsNotNone(m)
        self.assertIn('strcmp(kind, "new") != 0', m.group("body"))

    def test_result_carries_invite_spoken(self):
        self.assertIn('"invite_spoken"', APP_MAIN.replace('\\"', '"'))
        # spoken digits table includes 零 for digit 0
        self.assertIn('"零"', APP_MAIN)
        # confirm path finishes create with the spoken-code result
        self.assertRegex(
            APP_MAIN,
            r'mcp_op_is\("self\.intercom\.create"\)\)\s*\{\s*mcp_op_finish_ok_create\(\)')


if __name__ == "__main__":
    unittest.main()
