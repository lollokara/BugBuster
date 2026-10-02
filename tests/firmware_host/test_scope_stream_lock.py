"""AN-07: processScopeStream() held g_stateMutex while it sent every new scope
bucket over USB and the WebSocket, so the ADC poll task (which needs the same
mutex to publish samples) waited behind the transport and dropped data.

B: copy the new buckets under the lock (bounded batch, static buffer), release,
then encode and send.
"""

import re


from tests.firmware_host.fwhost import extract_function
from tests.lib.srcread import REPO_ROOT

BBP = REPO_ROOT / "Firmware/ESP32/src/bbp/bbp.cpp"


def test_scope_events_are_sent_after_the_state_mutex_is_released():
    body = re.sub(r"//[^\n]*", "", extract_function(BBP, r"static void processScopeStream\(void\)"))
    last_give = body.rindex("xSemaphoreGive(g_stateMutex)")
    first_send = body.index("sendEvent(BBP_EVT_SCOPE_DATA")
    assert first_send > last_give, "scope events still sent under g_stateMutex"
