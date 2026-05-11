"""
16_io_claim_example.py — IO-ownership claim / release (PR-5).

Demonstrates bugbuster.claim() as a context manager.  While inside the
`with` block the firmware ownership table records this script as the sole
owner of slots 3 and 6 (IO4 and IO7 on the terminal strip), and channel 0
(CH0, which is also mapped to IO3 / slot 12 in the ownership table).

Slot numbering (plan §1):
  0..11  = IO1..IO12
  12..15 = CH0..CH3

Any other interface (USB BBP, HTTP REST, desktop tab) that tries to mutate
those slots while this script holds the lease will receive IO_HELD_BY_OTHER.
"""

import bugbuster as bb

# Claim IO slots 3, 6 and CH0 (slot 12) for 10 seconds.
# The context manager calls io_owner_acquire on __enter__ and
# io_owner_release on __exit__ (even if an exception is raised).
with bb.claim([3, 6, 12], purpose="adc-sweep", lease_ms=10000) as _:
    ch = bb.Channel(0)
    ch.set_function(bb.FUNC_VIN)
    v = ch.read_voltage()
    print("V =", v)

# Slots are now released.  Checking the ownership table:
table = bb.owner_status()
for entry in table:
    if entry["kind"] != 0:   # 0 = IO_OWNER_NONE
        print("still held:", entry)
