# Treadmill research sources

Source documents retained for review and reproducible protocol research. Original
PDF/HTML bytes are stored unchanged; URLs, retrieval timestamps and SHA-256
checksums are in [sources.json](sources.json). Archived PDF/HTML files are tracked
as binary so Git preserves source bytes and line endings. A source document is evidence about
its stated protocol or product, not proof that the W1 implements every feature.

| Local document | Original source | Applicability |
|---|---|---|
| [FitShow FS-BLE-V3x manufacturer manual](fitshow-fs-ble-v3x-user-manual.pdf) | [Source](https://fccid.io/2AUIEFS-BLEV3X/User-Manual/User-Manual-5753770.pdf) | Manufacturer FCC exhibit 5753770; different module/protocol family. |
| [Bluetooth SIG Assigned Numbers](bluetooth-assigned-numbers.html) | [Source](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Assigned_Numbers/out/en/index-en.html) | Standard service and characteristic identifiers, including FTMS. |
| [Bluetooth SIG Fitness Machine Service ICS p5](bluetooth-ftms-ics-p5.pdf) | [Source](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2024/10/FTMS.ICS.p5.pdf) | FTMS supported feature and status conformance checklist. |
| [Bluetooth SIG Fitness Machine Service Test Suite p6](bluetooth-ftms-test-suite-p6.pdf) | [Source](https://files.bluetooth.com/wp-content/uploads/dlm_uploads/2024/10/FTMS.TS_.p6.pdf) | FTMS test definitions and target status semantics. |
| [Bluetooth SIG GATT Specification Supplement](bluetooth-gatt-specification-supplement.pdf) | [Source](https://btprodspecificationrefs.blob.core.windows.net/gatt-specification-supplement/GATT_Specification_Supplement.pdf) | Standard Treadmill Data and inclination units; does not validate device conformance or physical calibration. |
| [Fit Monster treadmill protocol v1.1](fitmonster-treadmill-protocol-v1.1.html) | [Source](https://www.fitmonster.club/SDK/FM-Protocol-Treadmill.html) | Newer primary provider document with related framing; opcode differences mean it is not verified W1 firmware documentation. |
| [FitShow treadmill protocol v1.1 (2017-08-20)](fitshow-treadmill-protocol-v1.1-2017.pdf) | [Source](https://fitshow.com/download/%E8%BF%90%E5%8A%A8%E7%A7%80%E7%94%B5%E8%B7%91%E8%93%9D%E7%89%99%E9%80%9A%E8%AE%AF%E5%8D%8F%E8%AE%AE.pdf) | Official FitShow source; command framing, sleep reason and READY compatibility bit. Firmware applicability requires trials. |
| [Conlin FitShow treadmill manual (not RunPad W1)](conlin-fitshow-treadmill-manual-not-runpad-w1.pdf) | [Source](https://www.conlinfitness.com/uploads/soft/230726/1-230H6143238.pdf) | Different treadmill model. Describes console volume/sound controls, but gives no Bluetooth mute command and does not establish W1 support. |
| [Microsoft GattSession.FromDeviceIdAsync API](microsoft-gattsession-fromdeviceid.html) | [Source](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattsession.fromdeviceidasync?view=winrt-26100) | Windows GATT session creation API, reviewed during keepalive research. |
| [Microsoft GattSession.MaintainConnection API](microsoft-gattsession-maintainconnection.html) | [Source](https://learn.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.genericattributeprofile.gattsession.maintainconnection?view=winrt-26100) | Windows connection lifetime API; does not promise to prevent treadmill console sleep. |
| [RunPad W1 manufacturer product specifications](runpad-w1-product.html) | [Source](https://runpad.eu/en/products/bieznia-z-biurkiem-w1-runpad-walkingpad-regulowana) | Manufacturer advertisement; maximum 11% and eight levels, no intermediate grade table. |


The W1 technical summary is also tracked at
[RunPad W1 WalkingDesk — Technical Specifications.md](../RunPad%20W1%20WalkingDesk%20%E2%80%94%20Technical%20Specifications.md).

## Protocol findings relevant to current experiments

The official FitShow v1.1 protocol identifies status 6 detail 2 as sleep and
detail 1 as the safety key. It describes `53/01` as READY when the mode byte has
its high compatibility bit set (`0x80`); with that bit clear it starts the
treadmill for compatibility. READY is therefore a motor-capable control command
requiring its own bounded validation on W1. It is not admitted to nonmotor
diagnostics. A future trial must detect countdown/running and stop immediately
if the console ignores this distinction. No READY keepawake test has run.

User instruction: retain newly consulted Internet technical documents here
and commit the archived files and provenance. Runtime BLE captures remain local.
