"""BUS-003: the external SPI bus was initialised with SPI_DMA_DISABLED, which
caps an ESP-IDF SPI master transaction at 64 bytes, while the firmware, Python
client, MCP tools and docs all advertise 512-byte transfers. Anything above
64 B failed. B: the bus uses a DMA channel and the bounce buffers are
DMA-capable, so the advertised 512 B limit holds."""

import re
from pathlib import Path

import pytest

SRC = (Path(__file__).resolve().parents[2] / "Firmware" / "ESP32" / "src" / "bus" /
       "ext_bus.cpp").read_text(encoding="utf-8")


@pytest.mark.xfail(strict=True, reason="BUS-003")
def test_spi_bus_uses_dma():
    m = re.search(r"spi_bus_initialize\(\s*EXT_SPI_HOST\s*,\s*&bus_cfg\s*,\s*(\w+)\s*\)", SRC)
    assert m and m.group(1) != "SPI_DMA_DISABLED", m and m.group(1)


@pytest.mark.xfail(strict=True, reason="BUS-003")
def test_spi_bounce_buffers_are_dma_capable():
    for name in ("s_spi_tx", "s_spi_rx"):
        decl = re.search(rf"^.*\b{name}\[EXT_SPI_MAX_TRANSFER\].*$", SRC, re.M)
        assert decl and "DMA_ATTR" in decl.group(0), decl and decl.group(0)
