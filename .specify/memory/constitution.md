# AlphaTouch Buddy Constitution

## I. Hardware Truth
Firmware MUST use the verified ESP32-S3 Touch LCD 1.28 hardware mapping: GC9A01A over SPI, CST816S over I2C, battery ADC on GPIO1, and backlight on GPIO2. Every hardware feature begins with a board-level validation test.

## II. Touch-First, Accessible UX
The round touch UI is the primary local interface. Controls MUST use high contrast, clear state feedback, a minimum practical touch area of 44 px where geometry permits, and a predictable Home/Back path. Critical actions require confirmation.

## III. Modern UI Parity
The device UI and web UI MUST share an intentional visual system: dark surface, high-contrast text, blue primary action, semantic success/warning/error colors, concise labels, explicit loading and empty states. The web UI MUST be responsive and work without internet access.

## IV. Offline-First and Private
Core keyboard, files, and device settings MUST work without Internet access. Wi-Fi credentials are saved in NVS and never exposed through status APIs or logs. The management portal must be local-network-only and secured with a first-run admin password before privileged actions are enabled.

## V. Safe State and Recovery
Configuration changes MUST be atomic where possible and recoverable. Factory reset and destructive file operations require confirmation. Firmware must offer a physical recovery path via BOOT/RESET and must not erase user files during normal upgrades.

## VI. Modular, Testable Firmware
Hardware, storage, BLE HID, provisioning, HTTP API, and UI modules remain decoupled behind small interfaces. Unit-test pure logic on the host; validate hardware integration on-device at each milestone.

## VII. Scope Discipline
The initial release targets the ESP32-S3 board. AlphaSmart NEO support is a companion-management adapter, not a claim of replacing NEO firmware. No direct USB protocol support is promised until the device transport is tested and validated on the target hardware.

## Quality Gates
Before a feature is complete: acceptance scenarios are demonstrable; errors are handled; user data is preserved; the web and touch flows are reviewed against the shared UI rules; and the device remains usable after Wi-Fi or BLE failure.
