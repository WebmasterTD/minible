## [](#header-1) Mooltipass Protocol
This page explains the new Mooltipass messaging protocol and the different commands implemented in its firmware.
   
## [](#header-2) High Level Message Structure Overview

| byte 0-1           | byte 2-3       | bytes 4-X |
|:-------------------|:---------------|:----------|
| Command Identifier | Payload Length | Payload   |
   
Debug and test commands have identifiers above 0x8000.  
As with our previous devices, every non-debug / non-test message sent by the computer will receive an answer.  
In all command descriptions below, hmstrlen() is a custom strlen function treating a character as a uint16_t (Unicode BMP). Therefore, hmstrlen("a") = 2.  
  
## [](#header-2) Mooltipass Commands

0x0001: Ping
------------

From the PC:

| byte 0-1 | byte 2-3         | bytes 4-X         |
|:---------|:-----------------|:------------------|
| 0x0001   | up to the sender | Arbitrary payload |

The device will send back the very same message.  
Tested status: tested


0x0002: Please Retry
--------------------

From the device:

| byte 0-1 | byte 2-3         | bytes 4-X         |
|:---------|:-----------------|:------------------|
| 0x0002   | 0                | N/A               |

When the device is busy and can't deal with the message sent by the computer, it will reply a message with a "Please Retry" one, inviting the computer to re-send its packet.  
Tested status: tested


0x0003: Get Platform Info
-------------------------

From the PC:

| byte 0-1 | byte 2-3         | bytes 4-X         |
|:---------|:-----------------|:------------------|
| 0x0003   | 0                | N/A               |

Device answer:

| bytes  | value  |
|:-------|:-------|
| 0->1   | 0x0003 |
| 2->3   | 14     |
| 4->5   | Main MCU fw major |
| 6->7   | Main MCU fw minor |
| 8->9   | Aux MCU fw major |
| 10->11 | Aux MCU fw minor |
| 12->15 | Platform serial number |
| 16->17 | DB memory size |

Tested status: NOT tested


0x0004: Set Current Date
------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-5                          | bytes 6-X         |
|:---------|:----------------------------|:-----------------------------------|:------------------|
| 0x0004   | 2 | current date (15 dn 9 -> Year (2010 + val), 8 dn 5 -> Month, 4 dn 0 -> Day of Month) | N/A |

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x0004   | 1 | 0x01 (indicates command success) |

Tested status: NOT tested


0x0005: Store Credential
------------------------

From the PC: 

| bytes  | value  |
|:-------|:-------|
| 0->1   | 0x0005 |
| 2->3   | depends on message contents |
| 4->5   | index to the service name (14) |
| 6->7   | index to the login name or 0 |
| 8->9   | index to the description or 0 |
| 10->11 | index to the third field or 0 |
| 12->13 | index to the password or 0 |
| 14->xxx | all above 0x0000 terminated fields concatenated |

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x0005   | 1 | 0x01 or 0x00 (success or fail) |

Tested status: NOT tested


0x0006: Get Credential
----------------------

From the PC: 

| bytes  | value  |
|:-------|:-------|
| 0->1   | 0x0006 |
| 2->3   | depends on message contents |
| 4->5   | index to the service (8) |
| 6->7   | index to the login or 0 |
| 8->xxx | all above 0x0000 terminated fields concatenated |

Device Answer:

| bytes  | value  |
|:-------|:-------|
| 0->1   | 0x0006 |
| 2->3   | 0 for fail, otherwise depends on message contents |
| 4->5   | index to the login name or 0 |
| 6->7   | index to the description or 0 |
| 8->9   | index to the third field or 0 |
| 10->11 | index to the password or 0 |
| 12->xxx | all above 0x0000 terminated fields concatenated |

Tested status: NOT tested
  
## [](#header-2) Mooltipass Debug and Test Commands

0x8000: Debug Message
-------------------

| byte 0-1 | byte 2-3                    | bytes 4-X                          |
|:---------|:----------------------------|:-----------------------------------|
| 0x8000   | hmstrlen(debug_message) + 2 | Debug Message + terminating 0x0000 |

Can be sent from both the device or the computer. **Does not require an answer.** 


0x8001: Open Display Buffer
---------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X                          |
|:---------|:----------------------------|:-----------------------------------|
| 0x8001   | 0 | Nothing |

Open the oled display buffer for writing.

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x8001   | 1 | 0x01 (indicates command success) |



0x8002: Send Pixel Data to Display Buffer
---------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X                          |
|:---------|:----------------------------|:-----------------------------------|
| 0x8002   | Payload size = number of pixels x 2 | Pixel data |

Send raw display data to opened display buffer. 

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x8002   | 1 | 0x01 (indicates command success) |



0x8003: Close Display Buffer
---------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X                          |
|:---------|:----------------------------|:-----------------------------------|
| 0x8003   | 0 | Nothing |

Stop ongoing display buffer data writing.

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x8003   | 1 | 0x01 (indicates command success) |



0x8004: Erase Data Flash
------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X                          |
|:---------|:----------------------------|:-----------------------------------|
| 0x8004   | 0 | Nothing |

Fully erase data flash. Returns before the flash is erased.

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x8004   | 1 | 0x01 (indicates command success) |



0x8005: Query Dataflash Ready Status
------------------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X                          |
|:---------|:----------------------------|:-----------------------------------|
| 0x8005   | 0 | Nothing |

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x8005   | 1 | 0x00 (dataflash busy) or 0x01 (dataflash ready) |



0x8006: Write 256 Bytes to Dataflash
------------------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-7                          | bytes 8-263                        |
|:---------|:----------------------------|:-----------------------------------|:-----------------------------------|
| 0x8006   | 260 | Write address | 256 bytes payload |

As it is a debug command, no boundary checks are performed. The place at which these 256 bytes are written should be previously erased.

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x8006   | 1 | 0x01 (indicates command success) |



0x8007: Reboot main MCU to Bootloader
-------------------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X |
|:---------|:----------------------------|:----------|
| 0x8007   | 0 | Nothing |

Start main microcontroller bootloader. **No device answer**.



0x8008: Get 32 Samples of Accelerometer Data
--------------------------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X |
|:---------|:----------------------------|:----------|
| 0x8008   | 0 | Nothing |

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x8008   | 192 | 32x (accX, accY, accZ - uint16_t) |



0x8009: Flash Aux MCU with Bundle Contents
------------------------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X |
|:---------|:----------------------------|:----------|
| 0x8009   | 0 | Nothing |

Flash aux MCU with binary file included in the bundle. **No device answer**.



0x800A: Get Platform Info
-------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X |
|:---------|:----------------------------|:----------|
| 0x800A   | 0 | Nothing |

Request platform info

Device Answer:

| Bytes | Description |
|:------|:------------|
| 0-1   | 0x800A |
| 2-3   | payload length (TBD) |
| 4-55  | see [here](aux_platform_spec_message) |
| 56-67 | reserved |
| 67-68 | main MCU fw version, major |
| 69-70 | main MCU fw version, minor |



0x800B: Reindex Bundle
------------------------------------

From the PC: 

| byte 0-1 | byte 2-3                    | bytes 4-X                          |
|:---------|:----------------------------|:-----------------------------------|
| 0x800B   | 0 | Nothing |

Ask the platform to reindex the bundle (used after uploading a new bundle).

Device Answer:

| byte 0-1 | byte 2-3                    | byte 4                          |
|:---------|:----------------------------|:--------------------------------|
| 0x800B   | 1 | 0x01 (indicates command success) |