"""Print which IO slots are owned, by whom, and until when.

Read-only. Slots 0-11 are IO1-IO12, 12-15 are CH0-CH3.
"""
import bugbuster

KINDS = {0: "free", 1: "USB", 2: "HTTP", 3: "script", 4: "CLI", 5: "internal"}
names = ["IO%d" % n for n in range(1, 13)] + ["CH%d" % n for n in range(4)]

for slot in bugbuster.owner_status():
    kind = KINDS.get(slot["kind"], "?")
    lease = slot["lease_until_ms"]
    print("%-4s %-8s%s" % (names[slot["slot"]], kind, " (lease until %d ms)" % lease if lease else ""))
