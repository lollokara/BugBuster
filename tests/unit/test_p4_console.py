"""Unit tests for the P4 serial console driver, using a fake serial port."""
import pytest

from tests.lib.p4_console import P4Console, FakeSerial


def test_cmd_writes_the_line_with_a_newline():
    ser = FakeSerial(responses=["ok\np4> "])
    con = P4Console(port=None, _serial=ser)
    con.cmd("faststat")
    assert ser.written == [b"faststat\r\n"]


def test_cmd_returns_output_up_to_the_prompt():
    ser = FakeSerial(responses=["ODR=64000 SPS\np4> "])
    con = P4Console(port=None, _serial=ser)
    assert "ODR=64000" in con.cmd("odr")


def test_cmd_strips_the_echoed_command_from_the_output():
    ser = FakeSerial(responses=["odr 64\nratio set to 64\np4> "])
    con = P4Console(port=None, _serial=ser)
    out = con.cmd("odr 64")
    assert out.strip().startswith("ratio set to 64")


def test_cmd_raises_on_deadline_with_no_prompt():
    ser = FakeSerial(responses=[""])
    con = P4Console(port=None, _serial=ser)
    with pytest.raises(TimeoutError):
        con.cmd("odr", deadline=0.2)


def test_set_odr_stops_acquisition_reprograms_then_restarts():
    ser = FakeSerial(responses=["p4> "] * 4)
    con = P4Console(port=None, _serial=ser)
    con.set_odr(64)
    assert ser.written == [b"fast off\r\n", b"odr 64\r\n", b"fast on\r\n"]
