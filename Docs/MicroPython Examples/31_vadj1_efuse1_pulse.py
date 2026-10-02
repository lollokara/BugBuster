import random
import bugbuster

WAIT_UNIT_SECONDS = 60

while True:
    try:
        state = bugbuster.rail_power_up(1, 3.0, 1)
        if not state["pg"] or state["fault"]:
            raise RuntimeError("VADJ1 power-good or e-fuse 1 fault")
        bugbuster.sleep(3000)
    finally:
        bugbuster.efuse_set(1, False)

    choice = random.getrandbits(8)
    while choice >= 252:
        choice = random.getrandbits(8)
    delay = (choice % 12 + 1) * WAIT_UNIT_SECONDS
    while delay:
        step = min(delay, 60)
        bugbuster.sleep(step * 1000)
        delay -= step