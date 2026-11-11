"""Writes dfu-frames.hex: the serial DFU frames adafruit-nrfutil sends for a 1100-byte image.

Run from tool/fixtures with a Python that has adafruit-nrfutil installed:
python make_dfu_frames.py
"""

from nordicsemi.dfu import crc16
from nordicsemi.dfu.dfu_transport_serial import (
    DFU_DATA_PACKET,
    DFU_INIT_PACKET,
    DFU_START_PACKET,
    DFU_STOP_DATA_PACKET,
    DFU_UPDATE_MODE_APP,
    DfuTransportSerial,
    HciPacket,
)
from nordicsemi.dfu.dfu_transport import DfuTransport
from nordicsemi.dfu.init_packet import Packet, PacketField
from nordicsemi.dfu.util import int16_to_bytes, int32_to_bytes

image = bytes((i * 7 + 3) & 0xFF for i in range(1100))
init = Packet(
    {
        PacketField.DEVICE_TYPE: 0x0052,
        PacketField.DEVICE_REVISION: 0xFFFF,
        PacketField.APP_VERSION: 0xFFFFFFFF,
        PacketField.REQUIRED_SOFTDEVICES_ARRAY: [0xFFFE],
        PacketField.NORDIC_PROPRIETARY_OPT_DATA_FIRMWARE_CRC16: crc16.calc_crc16(image, crc=0xFFFF),
    }
).generate_packet()

HciPacket.sequence_number = 0
payloads = [
    list(int32_to_bytes(DFU_START_PACKET))
    + list(int32_to_bytes(DFU_UPDATE_MODE_APP))
    + list(DfuTransport.create_image_size_packet(0, 0, len(image))),
    list(int32_to_bytes(DFU_INIT_PACKET)) + [chr(b) for b in init] + list(int16_to_bytes(0)),
]
size = DfuTransportSerial.DFU_PACKET_MAX_SIZE
for at in range(0, len(image), size):
    payloads.append(list(int32_to_bytes(DFU_DATA_PACKET)) + [chr(b) for b in image[at : at + size]])
payloads.append(list(int32_to_bytes(DFU_STOP_DATA_PACKET)))

with open("dfu-frames.hex", "w") as out:
    for payload in payloads:
        out.write(" ".join(f"{b:02x}" for b in HciPacket(payload).data) + "\n")
