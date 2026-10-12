#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise device ordering, reconnect, session changes and turn association."""

import copy
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools/voicefe_chat"))
from device_events import Devices  # noqa: E402


def packet(seq, boot="boot-1", companion=False):
    return dict(
        version=1, device_id="titan-mini", boot_id=boot, seq=seq,
        state=dict(video=False, audio=False, companion=companion,
                   wake_deadline=0, model_version="test-1", overrun=0, event_drop=0),
        events=[dict(
            version=1, seq=seq, utterance_id=10, monotonic_ms=2000,
            kind="intent", intent="companion_enter", resolved=2,
            score=30000, margin=20000, result="applied",
            video=False, audio=False, companion=companion,
        )],
    )


def main():
    devices = Devices()
    first = packet(1)
    assert len(devices.receive(first)[1]) == 1
    assert devices.receive(first)[1] == []
    assert devices.association("titan-mini", "boot-1", 10)["seq"] == 1
    assert len(devices.receive(packet(3, companion=True))[1]) == 1
    assert len(devices.receive(packet(2))[1]) == 1
    assert devices.snapshot()[0]["state"]["companion"]
    assert len(devices.receive(packet(1, "boot-2"))[1]) == 1
    assert devices.receive(packet(4, "boot-1"))[1] == []
    assert devices.snapshot()[0]["boot_id"] == "boot-2"
    for mutation in [
        lambda p: p.update(version=2),
        lambda p: p.update(device_id='bad"id'),
        lambda p: p["events"][0].update(score=32769),
        lambda p: p["events"][0].update(intent="arbitrary"),
        lambda p: p["state"].update(video=1),
        lambda p: p.update(events=p["events"] * 17),
        lambda p: p["events"][0].update(seq=99),
    ]:
        value = copy.deepcopy(first)
        mutation(value)
        try:
            devices.receive(value)
        except ValueError:
            pass
        else:
            raise AssertionError(value)
    for seq in range(1, 700):
        value = packet(seq, "boot-2")
        value["events"][0]["utterance_id"] = seq
        devices.receive(value)
    assert len(devices.associations) <= 256
    assert len(devices.devices["titan-mini"]["seen"]) <= 513
    assert len(devices.snapshot()[0]["recent"]) <= 16
    print("PASS event dedup, late event, stale snapshot, boot rollover,"
          " bounded ledger, validation and utterance association")


if __name__ == "__main__":
    main()
